#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace Dynarmic {
class ExclusiveMonitor;
namespace A64 {
class Jit;
}
} // namespace Dynarmic

struct CpuContext {
    Dynarmic::A64::Jit* jit = nullptr;
    uint64_t tpidr_el0 = 0;

    uint64_t GetX(int idx) const;
    void SetX(int idx, uint64_t val);
    uint32_t GetW(int idx) const;
    void SetW(int idx, uint32_t val);
    double GetD(int idx) const;
    void SetD(int idx, double val);
    float GetS(int idx) const;
    void SetS(int idx, float val);
    uint64_t GetSP() const;
    void SetSP(uint64_t sp);
    uint64_t GetPC() const;
    void SetPC(uint64_t pc);
    uint64_t GetLR() const;
    void SetLR(uint64_t lr);

    // Read nth 64-bit stack slot above SP (AAPCS64 overflow args start at offset 0).
    uint64_t ReadStackU64(size_t slot) const;
};

using SvcHandlerFn = std::function<void(CpuContext&)>;

class CpuRuntime {
public:
    static constexpr uint32_t kSentinelReturnSvc = 0xFFFF;
    static constexpr size_t kMaxThreads = 128;

    CpuRuntime();
    ~CpuRuntime();

    // Allocates an 8-byte SVC stub (SVC #id; RET) in the guest-readable trampoline page
    // and registers its host handler. Returns the stub's guest virtual address.
    uint64_t AllocateSvcStub(const std::string& debug_name, SvcHandlerFn handler);

    // Allocates pure AArch64 machine code instructions directly in guest-readable memory
    // (runs 100% inside Dynarmic JIT with zero SVC exits).
    uint64_t AllocateCodeStub(const std::vector<uint32_t>& insns);

    // Sentinel stub address (SVC #0xFFFF; RET) used as LR for CallGuest.
    uint64_t SentinelReturnAddr() const { return sentinel_stub_addr_; }

    // Ensures current OS thread has a guest stack, TPIDR_EL0 TLS block (+0x28 canary),
    // and Dynarmic JIT pool initialized. Safe to call multiple times per thread.
    void EnsureThreadInit();

    // Calls an AArch64 guest function on the current thread (supports arbitrary nested
    // re-entrancy via per-thread JIT depth pool, sharing SP and TPIDR_EL0).
    uint64_t CallGuest(uint64_t addr, std::initializer_list<uint64_t> x_args = {});
    uint64_t CallGuestWithFloats(uint64_t addr,
                                 std::initializer_list<uint64_t> x_args,
                                 std::initializer_list<double> d_args);
    uint64_t CallGuestWithF32(uint64_t addr,
                              std::initializer_list<uint64_t> x_args,
                              std::initializer_list<float> s_args);

    // Access current thread's active CpuContext (only valid inside an SVC handler).
    static CpuContext* CurrentContext();
    static CpuRuntime* CurrentRuntime();

    // Thread TLS / canary helpers.
    uint64_t CurrentThreadTpidr() const;
    static constexpr uint64_t kStackCanaryOffset = 0x28;

    // Dispatches an SVC from a Dynarmic UserCallbacks instance.
    void DispatchSvc(uint32_t svc_id, Dynarmic::A64::Jit* jit, uint64_t tpidr_el0);

    // Memory diagnostics hook.
    void SetFaultFatal(bool fatal) { fault_fatal_ = fatal; }
    bool IsFaultFatal() const { return fault_fatal_; }
    void SetSvcProfileEnabled(bool enabled) { profile_svc_enabled_ = enabled; }
    void ResetSvcProfile();
    void DumpTopSvcs(int frame_num, double frame_ms, int top_n = 8) const;

    static const char* LastSvcName();
    static uint64_t LastSvcLR();

    struct ThreadState;

private:
    ThreadState& GetOrCreateThreadState();

    uint8_t* trampoline_page_ = nullptr;
    size_t trampoline_capacity_ = 0;
    uint8_t* code_stub_page_ = nullptr;
    size_t code_stub_capacity_ = 0;
    size_t code_stub_used_ = 0;
    uint32_t next_svc_id_ = 1;
    uint64_t sentinel_stub_addr_ = 0;

    struct SvcEntry {
        std::string name;
        SvcHandlerFn handler;
        mutable uint64_t prof_calls = 0;
        mutable uint64_t prof_ns = 0;
    };
    std::vector<SvcEntry> svc_table_;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> exclusive_monitor_;
    bool fault_fatal_ = true;
    bool profile_svc_enabled_ = false;
};
