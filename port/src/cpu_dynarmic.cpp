#include "cpu_dynarmic.h"

#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/interface/halt_reason.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

thread_local CpuContext* tls_active_ctx = nullptr;
thread_local CpuRuntime* tls_active_runtime = nullptr;
thread_local const char* tls_last_svc_name = "(none)";
thread_local uint64_t tls_last_svc_lr = 0;
constexpr size_t kGuestStackSize = 4 * 1024 * 1024; // 4 MiB per thread
constexpr size_t kTlsBlockSize = 4096;
constexpr size_t kMaxProcessorIds = 256;

struct GuestCallbacks final : Dynarmic::A64::UserCallbacks {
    CpuRuntime* runtime = nullptr;
    Dynarmic::A64::Jit* jit = nullptr;
    uint64_t* tpidr_ptr = nullptr;

    void CheckAddr(Dynarmic::A64::VAddr vaddr, const char* op) {
        if (vaddr < 0x10000) {
            std::fprintf(stderr,
                         "[CPU FAULT] %s invalid addr=0x%016llx PC=0x%016llx LR=0x%016llx SP=0x%016llx\n"
                         "  X0=0x%llx X1=0x%llx X2=0x%llx X3=0x%llx\n",
                         op,
                         static_cast<unsigned long long>(vaddr),
                         static_cast<unsigned long long>(jit->GetPC()),
                         static_cast<unsigned long long>(jit->GetRegister(30)),
                         static_cast<unsigned long long>(jit->GetSP()),
                         static_cast<unsigned long long>(jit->GetRegister(0)),
                         static_cast<unsigned long long>(jit->GetRegister(1)),
                         static_cast<unsigned long long>(jit->GetRegister(2)),
                         static_cast<unsigned long long>(jit->GetRegister(3)));
            std::fflush(stderr);
            if (runtime->IsFaultFatal()) {
                std::abort();
            }
            jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
        }
    }

    std::optional<uint32_t> MemoryReadCode(Dynarmic::A64::VAddr vaddr) override {
        if (vaddr < 0x10000) {
            CheckAddr(vaddr, "ReadCode");
            return std::nullopt;
        }
        uint32_t val = 0;
        std::memcpy(&val, reinterpret_cast<const void*>(vaddr), sizeof(val));
        return val;
    }

    uint8_t MemoryRead8(Dynarmic::A64::VAddr vaddr) override {
        CheckAddr(vaddr, "Read8");
        uint8_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(vaddr), sizeof(v));
        return v;
    }
    uint16_t MemoryRead16(Dynarmic::A64::VAddr vaddr) override {
        CheckAddr(vaddr, "Read16");
        uint16_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(vaddr), sizeof(v));
        return v;
    }
    uint32_t MemoryRead32(Dynarmic::A64::VAddr vaddr) override {
        CheckAddr(vaddr, "Read32");
        uint32_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(vaddr), sizeof(v));
        return v;
    }
    uint64_t MemoryRead64(Dynarmic::A64::VAddr vaddr) override {
        CheckAddr(vaddr, "Read64");
        uint64_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(vaddr), sizeof(v));
        return v;
    }
    Dynarmic::A64::Vector MemoryRead128(Dynarmic::A64::VAddr vaddr) override {
        CheckAddr(vaddr, "Read128");
        Dynarmic::A64::Vector v{0, 0};
        std::memcpy(&v, reinterpret_cast<const void*>(vaddr), sizeof(v));
        return v;
    }

    void MemoryWrite8(Dynarmic::A64::VAddr vaddr, uint8_t value) override {
        CheckAddr(vaddr, "Write8");
        std::memcpy(reinterpret_cast<void*>(vaddr), &value, sizeof(value));
    }
    void MemoryWrite16(Dynarmic::A64::VAddr vaddr, uint16_t value) override {
        CheckAddr(vaddr, "Write16");
        std::memcpy(reinterpret_cast<void*>(vaddr), &value, sizeof(value));
    }
    void MemoryWrite32(Dynarmic::A64::VAddr vaddr, uint32_t value) override {
        CheckAddr(vaddr, "Write32");
        std::memcpy(reinterpret_cast<void*>(vaddr), &value, sizeof(value));
    }
    void MemoryWrite64(Dynarmic::A64::VAddr vaddr, uint64_t value) override {
        CheckAddr(vaddr, "Write64");
        std::memcpy(reinterpret_cast<void*>(vaddr), &value, sizeof(value));
    }
    void MemoryWrite128(Dynarmic::A64::VAddr vaddr, Dynarmic::A64::Vector value) override {
        CheckAddr(vaddr, "Write128");
        std::memcpy(reinterpret_cast<void*>(vaddr), &value, sizeof(value));
    }

    bool MemoryWriteExclusive8(Dynarmic::A64::VAddr vaddr, uint8_t value, uint8_t expected) override {
        CheckAddr(vaddr, "WriteExcl8");
        auto* p = reinterpret_cast<volatile char*>(vaddr);
        return _InterlockedCompareExchange8(p, static_cast<char>(value), static_cast<char>(expected)) == static_cast<char>(expected);
    }
    bool MemoryWriteExclusive16(Dynarmic::A64::VAddr vaddr, uint16_t value, uint16_t expected) override {
        CheckAddr(vaddr, "WriteExcl16");
        auto* p = reinterpret_cast<volatile short*>(vaddr);
        return _InterlockedCompareExchange16(p, static_cast<short>(value), static_cast<short>(expected)) == static_cast<short>(expected);
    }
    bool MemoryWriteExclusive32(Dynarmic::A64::VAddr vaddr, uint32_t value, uint32_t expected) override {
        CheckAddr(vaddr, "WriteExcl32");
        auto* p = reinterpret_cast<volatile long*>(vaddr);
        return _InterlockedCompareExchange(p, static_cast<long>(value), static_cast<long>(expected)) == static_cast<long>(expected);
    }
    bool MemoryWriteExclusive64(Dynarmic::A64::VAddr vaddr, uint64_t value, uint64_t expected) override {
        CheckAddr(vaddr, "WriteExcl64");
        auto* p = reinterpret_cast<volatile long long*>(vaddr);
        return _InterlockedCompareExchange64(p, static_cast<long long>(value), static_cast<long long>(expected)) == static_cast<long long>(expected);
    }
    bool MemoryWriteExclusive128(Dynarmic::A64::VAddr vaddr, Dynarmic::A64::Vector value, Dynarmic::A64::Vector expected) override {
        CheckAddr(vaddr, "WriteExcl128");
        long long comparand[2] = {static_cast<long long>(expected[0]), static_cast<long long>(expected[1])};
        return _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(vaddr),
                                              static_cast<long long>(value[1]),
                                              static_cast<long long>(value[0]),
                                              comparand) != 0;
    }

    void InterpreterFallback(Dynarmic::A64::VAddr pc, size_t num_instructions) override {
        std::fprintf(stderr, "[CPU FATAL] InterpreterFallback at PC=0x%016llx count=%zu\n",
                     static_cast<unsigned long long>(pc), num_instructions);
        std::abort();
    }

    void CallSVC(uint32_t swi) override {
        runtime->DispatchSvc(swi, jit, *tpidr_ptr);
    }

    void ExceptionRaised(Dynarmic::A64::VAddr pc, Dynarmic::A64::Exception exception) override {
        if (exception == Dynarmic::A64::Exception::Yield ||
            exception == Dynarmic::A64::Exception::SendEvent ||
            exception == Dynarmic::A64::Exception::SendEventLocal ||
            exception == Dynarmic::A64::Exception::WaitForEvent) {
            return;
        }
        std::fprintf(stderr, "[CPU EXCEPTION] pc=0x%016llx ex=%d LR=0x%016llx\n",
                     static_cast<unsigned long long>(pc),
                     static_cast<int>(exception),
                     static_cast<unsigned long long>(jit->GetRegister(30)));
        std::fflush(stderr);
        if (runtime->IsFaultFatal()) {
            std::abort();
        }
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }

    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1000000000ULL; }
    uint64_t GetCNTPCT() override {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return static_cast<uint64_t>(counter.QuadPart);
    }
};

struct JitSlot {
    std::unique_ptr<GuestCallbacks> cb;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
};

std::mutex g_svc_mutex;
std::atomic<size_t> g_next_proc_id{0};

} // namespace

// ---- CpuContext helpers ----
uint64_t CpuContext::GetX(int idx) const { return jit->GetRegister(static_cast<size_t>(idx)); }
void CpuContext::SetX(int idx, uint64_t val) { jit->SetRegister(static_cast<size_t>(idx), val); }
uint32_t CpuContext::GetW(int idx) const { return static_cast<uint32_t>(jit->GetRegister(static_cast<size_t>(idx))); }
void CpuContext::SetW(int idx, uint32_t val) { jit->SetRegister(static_cast<size_t>(idx), static_cast<uint64_t>(val)); }

double CpuContext::GetD(int idx) const {
    auto v = jit->GetVector(static_cast<size_t>(idx));
    double d = 0.0;
    std::memcpy(&d, &v[0], sizeof(d));
    return d;
}
void CpuContext::SetD(int idx, double val) {
    Dynarmic::A64::Vector v{0, 0};
    std::memcpy(&v[0], &val, sizeof(val));
    jit->SetVector(static_cast<size_t>(idx), v);
}
float CpuContext::GetS(int idx) const {
    auto v = jit->GetVector(static_cast<size_t>(idx));
    float f = 0.0f;
    std::memcpy(&f, &v[0], sizeof(f));
    return f;
}
void CpuContext::SetS(int idx, float val) {
    Dynarmic::A64::Vector v{0, 0};
    std::memcpy(&v[0], &val, sizeof(val));
    jit->SetVector(static_cast<size_t>(idx), v);
}
uint64_t CpuContext::GetSP() const { return jit->GetSP(); }
void CpuContext::SetSP(uint64_t sp) { jit->SetSP(sp); }
uint64_t CpuContext::GetPC() const { return jit->GetPC(); }
void CpuContext::SetPC(uint64_t pc) { jit->SetPC(pc); }
uint64_t CpuContext::GetLR() const { return jit->GetRegister(30); }
void CpuContext::SetLR(uint64_t lr) { jit->SetRegister(30, lr); }

uint64_t CpuContext::ReadStackU64(size_t slot) const {
    uint64_t val = 0;
    std::memcpy(&val, reinterpret_cast<const void*>(jit->GetSP() + slot * 8), sizeof(val));
    return val;
}

// ---- ThreadState ----
struct CpuRuntime::ThreadState {
    CpuRuntime* owner = nullptr;
    uint8_t* stack_base = nullptr;
    uint64_t initial_sp = 0;
    uint8_t* tls_block = nullptr;
    uint64_t tls_addr = 0;
    size_t active_depth = 0;
    std::vector<std::unique_ptr<JitSlot>> slots;

    ~ThreadState() {
        slots.clear();
        if (stack_base) VirtualFree(stack_base, 0, MEM_RELEASE);
        if (tls_block) VirtualFree(tls_block, 0, MEM_RELEASE);
    }
};

namespace {
thread_local std::unique_ptr<CpuRuntime::ThreadState> tls_thread_state;
} // namespace

CpuRuntime::CpuRuntime() {
    trampoline_capacity_ = 64 * 1024;
    trampoline_page_ = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, trampoline_capacity_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    code_stub_capacity_ = 64 * 1024;
    code_stub_page_ = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, code_stub_capacity_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!trampoline_page_ || !code_stub_page_) {
        std::fprintf(stderr, "[CPU FATAL] VirtualAlloc trampoline/code_stub failed\n");
        std::abort();
    }

    // Slot 0: sentinel return stub (SVC #0xFFFF; RET)
    auto* stub0 = reinterpret_cast<uint32_t*>(trampoline_page_);
    stub0[0] = 0xD4000001u | (kSentinelReturnSvc << 5);
    stub0[1] = 0xD65F03C0u;
    sentinel_stub_addr_ = reinterpret_cast<uint64_t>(trampoline_page_);

    // Pre-allocate fixed capacity matching trampoline_capacity_ / 8 so DispatchSvc reads lock-free.
    svc_table_.resize(trampoline_capacity_ / 8);
    exclusive_monitor_ = std::make_unique<Dynarmic::ExclusiveMonitor>(kMaxProcessorIds);
}

CpuRuntime::~CpuRuntime() {
    // If current thread owns a ThreadState bound to this runtime, tear it down before exclusive_monitor_.
    if (tls_thread_state && tls_thread_state->owner == this) {
        tls_thread_state.reset();
    }
    if (trampoline_page_) {
        VirtualFree(trampoline_page_, 0, MEM_RELEASE);
        trampoline_page_ = nullptr;
    }
    if (code_stub_page_) {
        VirtualFree(code_stub_page_, 0, MEM_RELEASE);
        code_stub_page_ = nullptr;
    }
}

uint64_t CpuRuntime::AllocateCodeStub(const std::vector<uint32_t>& insns) {
    std::lock_guard<std::mutex> lock(g_svc_mutex);
    size_t bytes = insns.size() * sizeof(uint32_t);
    size_t aligned = (code_stub_used_ + 15) & ~size_t{15};
    if (aligned + bytes > code_stub_capacity_) {
        std::fprintf(stderr, "[CPU FATAL] AArch64 code stub page exhausted!\n");
        std::abort();
    }
    uint8_t* dst = code_stub_page_ + aligned;
    std::memcpy(dst, insns.data(), bytes);
    code_stub_used_ = aligned + bytes;
    return reinterpret_cast<uint64_t>(dst);
}

uint64_t CpuRuntime::AllocateSvcStub(const std::string& debug_name, SvcHandlerFn handler) {
    std::lock_guard<std::mutex> lock(g_svc_mutex);
    const size_t max_stubs = trampoline_capacity_ / 8;
    if (next_svc_id_ >= max_stubs || next_svc_id_ >= kSentinelReturnSvc) {
        std::fprintf(stderr, "[CPU FATAL] SVC trampoline table exhausted!\n");
        std::abort();
    }
    uint32_t id = next_svc_id_++;
    uint8_t* addr = trampoline_page_ + static_cast<size_t>(id) * 8;
    auto* words = reinterpret_cast<uint32_t*>(addr);
    words[0] = 0xD4000001u | (id << 5); // svc #id
    words[1] = 0xD65F03C0u;             // ret
    svc_table_[id].name = debug_name;
    svc_table_[id].handler = std::move(handler);
    svc_table_[id].prof_calls = 0;
    svc_table_[id].prof_ns = 0;
    return reinterpret_cast<uint64_t>(addr);
}

CpuRuntime::ThreadState& CpuRuntime::GetOrCreateThreadState() {
    if (!tls_thread_state || tls_thread_state->owner != this) {
        auto st = std::make_unique<ThreadState>();
        st->owner = this;
        st->stack_base = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kGuestStackSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        st->initial_sp = (reinterpret_cast<uint64_t>(st->stack_base) + kGuestStackSize - 0x100) & ~15ULL;

        st->tls_block = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kTlsBlockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        st->tls_addr = reinterpret_cast<uint64_t>(st->tls_block);

        // Populate Bionic AArch64 TLS slots:
        // +0x08: pthread_internal_t* self (non-null unique thread pointer)
        // +0x28: __stack_chk_guard (random non-zero canary with LSB 0x00)
        uint64_t canary = 0;
        BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&canary), sizeof(canary), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        canary = (canary & ~0xFFULL) | 0x52523300ULL;
        if (canary == 0) canary = 0x52523343414E4100ULL;
        std::memcpy(st->tls_block + 0x08, &st->tls_addr, sizeof(uint64_t));
        std::memcpy(st->tls_block + kStackCanaryOffset, &canary, sizeof(uint64_t));

        tls_thread_state = std::move(st);
    }
    return *tls_thread_state;
}

void CpuRuntime::EnsureThreadInit() {
    (void)GetOrCreateThreadState();
}

uint64_t CpuRuntime::CurrentThreadTpidr() const {
    if (tls_thread_state && tls_thread_state->owner == this) {
        return tls_thread_state->tls_addr;
    }
    return 0;
}

uint64_t CpuRuntime::CallGuest(uint64_t addr, std::initializer_list<uint64_t> x_args) {
    return CallGuestWithFloats(addr, x_args, {});
}

uint64_t CpuRuntime::CallGuestWithFloats(uint64_t addr,
                                         std::initializer_list<uint64_t> x_args,
                                         std::initializer_list<double> d_args) {
    ThreadState& st = GetOrCreateThreadState();
    const size_t depth = st.active_depth;

    if (depth >= st.slots.size()) {
        auto slot = std::make_unique<JitSlot>();
        slot->cb = std::make_unique<GuestCallbacks>();
        slot->cb->runtime = this;
        slot->cb->tpidr_ptr = &st.tls_addr;

        Dynarmic::A64::UserConfig cfg;
        cfg.callbacks = slot->cb.get();
        cfg.processor_id = g_next_proc_id.fetch_add(1) % kMaxProcessorIds;
        cfg.global_monitor = exclusive_monitor_.get();
        cfg.tpidr_el0 = &st.tls_addr;
        cfg.tpidrro_el0 = &st.tls_addr;
        cfg.enable_cycle_counting = false;
        cfg.wall_clock_cntpct = true;
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        cfg.cntfrq_el0 = static_cast<uint32_t>(freq.QuadPart);
        cfg.code_cache_size = (depth == 0) ? (128 * 1024 * 1024) : (16 * 1024 * 1024);
        // Flat 64-bit host-addressed fastmem: single x86_64 [r13+vaddr] load/store with r13=0.
        cfg.fastmem_pointer = static_cast<uintptr_t>(0);
        cfg.fastmem_address_space_bits = 64;
        cfg.silently_mirror_fastmem = false;
        cfg.recompile_on_fastmem_failure = true;
        cfg.fastmem_exclusive_access = true;
        cfg.recompile_on_exclusive_fastmem_failure = true;

        slot->jit = std::make_unique<Dynarmic::A64::Jit>(cfg);
        slot->cb->jit = slot->jit.get();
        st.slots.push_back(std::move(slot));
    }

    JitSlot& slot = *st.slots[depth];
    uint64_t sp = (depth == 0) ? st.initial_sp
                               : ((st.slots[depth - 1]->jit->GetSP() - 32) & ~15ULL);

    slot.jit->ClearHalt();
    slot.jit->SetSP(sp);
    slot.jit->SetPC(addr);
    slot.jit->SetRegister(30, sentinel_stub_addr_); // LR -> SVC #0xFFFF
    slot.jit->SetRegister(29, 0);

    size_t xi = 0;
    for (uint64_t v : x_args) {
        if (xi < 8) {
            slot.jit->SetRegister(xi, v);
        }
        ++xi;
    }
    size_t di = 0;
    for (double d : d_args) {
        if (di < 8) {
            Dynarmic::A64::Vector vec{0, 0};
            std::memcpy(&vec[0], &d, sizeof(d));
            slot.jit->SetVector(di, vec);
        }
        ++di;
    }

    st.active_depth = depth + 1;
    CpuRuntime* prev_rt = tls_active_runtime;
    tls_active_runtime = this;

    slot.jit->Run();

    uint64_t ret = slot.jit->GetRegister(0);
    tls_active_runtime = prev_rt;
    st.active_depth = depth;
    return ret;
}

uint64_t CpuRuntime::CallGuestWithF32(uint64_t addr,
                                      std::initializer_list<uint64_t> x_args,
                                      std::initializer_list<float> s_args) {
    // Pack each 32-bit float into low 32 bits of double bit-pattern so CallGuestWithFloats loads S0..S7!
    double packed[8]{};
    size_t n = 0;
    for (float f : s_args) {
        if (n >= 8) break;
        uint64_t bits = 0;
        std::memcpy(&bits, &f, sizeof(float));
        std::memcpy(&packed[n], &bits, sizeof(double));
        ++n;
    }
    switch (n) {
        case 0: return CallGuestWithFloats(addr, x_args, {});
        case 1: return CallGuestWithFloats(addr, x_args, {packed[0]});
        case 2: return CallGuestWithFloats(addr, x_args, {packed[0], packed[1]});
        case 3: return CallGuestWithFloats(addr, x_args, {packed[0], packed[1], packed[2]});
        case 4: return CallGuestWithFloats(addr, x_args, {packed[0], packed[1], packed[2], packed[3]});
        case 5: return CallGuestWithFloats(addr, x_args, {packed[0], packed[1], packed[2], packed[3], packed[4]});
        case 6: return CallGuestWithFloats(addr, x_args, {packed[0], packed[1], packed[2], packed[3], packed[4], packed[5]});
        case 7: return CallGuestWithFloats(addr, x_args, {packed[0], packed[1], packed[2], packed[3], packed[4], packed[5], packed[6]});
        default: return CallGuestWithFloats(addr, x_args, {packed[0], packed[1], packed[2], packed[3], packed[4], packed[5], packed[6], packed[7]});
    }
}

void CpuRuntime::ResetSvcProfile() {
    for (uint32_t i = 1; i < next_svc_id_; ++i) {
        svc_table_[i].prof_calls = 0;
        svc_table_[i].prof_ns = 0;
    }
}

void CpuRuntime::DumpTopSvcs(int frame_num, double frame_ms, int top_n) const {
    std::vector<std::pair<uint64_t, uint32_t>> items;
    uint64_t total_ns = 0;
    uint64_t total_calls = 0;
    for (uint32_t i = 1; i < next_svc_id_; ++i) {
        if (svc_table_[i].prof_calls > 0) {
            items.emplace_back(svc_table_[i].prof_ns, i);
            total_ns += svc_table_[i].prof_ns;
            total_calls += svc_table_[i].prof_calls;
        }
    }
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(stdout, "[PROF] frame=%d total=%.1fms svc_total=%.1fms (%llu calls) cpu_est=%.1fms\n",
                 frame_num, frame_ms, total_ns / 1e6,
                 static_cast<unsigned long long>(total_calls),
                 frame_ms - total_ns / 1e6);
    int n = std::min<int>(top_n, static_cast<int>(items.size()));
    for (int k = 0; k < n; ++k) {
        uint32_t id = items[k].second;
        std::fprintf(stdout, "  #%d %-40s %8.2f ms (%llu calls)\n",
                     k + 1, svc_table_[id].name.c_str(),
                     svc_table_[id].prof_ns / 1e6,
                     static_cast<unsigned long long>(svc_table_[id].prof_calls));
    }
}

void CpuRuntime::DispatchSvc(uint32_t svc_id, Dynarmic::A64::Jit* jit, uint64_t tpidr_el0) {
    if (svc_id == kSentinelReturnSvc) {
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        return;
    }
    if (svc_id == 0 || svc_id >= svc_table_.size() || !svc_table_[svc_id].handler) {
        std::fprintf(stderr, "[CPU FATAL] Unknown SVC #%u at PC=0x%016llx LR=0x%016llx\n",
                     svc_id,
                     static_cast<unsigned long long>(jit->GetPC()),
                     static_cast<unsigned long long>(jit->GetRegister(30)));
        std::abort();
    }
    const SvcEntry& entry = svc_table_[svc_id];

    CpuContext ctx;
    ctx.jit = jit;
    ctx.tpidr_el0 = tpidr_el0;

    CpuContext* prev_ctx = tls_active_ctx;
    CpuRuntime* prev_rt = tls_active_runtime;
    tls_active_ctx = &ctx;
    tls_active_runtime = this;
    tls_last_svc_name = entry.name.c_str();
    tls_last_svc_lr = jit->GetRegister(30);

    if (profile_svc_enabled_) {
        auto t0 = std::chrono::steady_clock::now();
        entry.handler(ctx);
        auto t1 = std::chrono::steady_clock::now();
        entry.prof_calls += 1;
        entry.prof_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    } else {
        entry.handler(ctx);
    }

    tls_active_ctx = prev_ctx;
    tls_active_runtime = prev_rt;
}

CpuContext* CpuRuntime::CurrentContext() { return tls_active_ctx; }
CpuRuntime* CpuRuntime::CurrentRuntime() { return tls_active_runtime; }
const char* CpuRuntime::LastSvcName() { return tls_last_svc_name; }
uint64_t CpuRuntime::LastSvcLR() { return tls_last_svc_lr; }
