#include "cpu_dynarmic.h"
#include "elf_loader.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#define CHECK(expr)                                                                 \
    do {                                                                            \
        if (!(expr)) {                                                              \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

namespace {
// Helper: allocate executable guest-readable page and copy AArch64 instructions.
uint64_t AllocCode(const std::vector<uint32_t>& insns) {
    void* mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(mem != nullptr);
    std::memcpy(mem, insns.data(), insns.size() * sizeof(uint32_t));
    return reinterpret_cast<uint64_t>(mem);
}

void TestTpidrCanaryAndCall() {
    CpuRuntime cpu;
    cpu.EnsureThreadInit();

    // mrs x0, tpidr_el0          -> 0xd53bd040
    // ldr x0, [x0, #0x28]        -> 0xf9401400
    // ret                        -> 0xd65f03c0
    uint64_t fn = AllocCode({0xd53bd040u, 0xf9401400u, 0xd65f03c0u});
    uint64_t canary = cpu.CallGuest(fn);
    uint64_t tpidr = cpu.CurrentThreadTpidr();
    CHECK(tpidr != 0);
    uint64_t expected = *reinterpret_cast<uint64_t*>(tpidr + CpuRuntime::kStackCanaryOffset);
    CHECK(canary != 0);
    CHECK(canary == expected);
    VirtualFree(reinterpret_cast<void*>(fn), 0, MEM_RELEASE);
    std::printf("  [PASS] test_tpidr_canary_and_call (canary=0x%016llx)\n",
                static_cast<unsigned long long>(canary));
}

void TestSvcThunkAndReentrancy() {
    CpuRuntime cpu;
    cpu.EnsureThreadInit();

    // Inner guest function: f(x0, x1) = x0 * 3 + x1
    // add x2, x0, x0    -> 0x8b000002
    // add x2, x2, x0    -> 0x8b000042
    // add x0, x2, x1    -> 0x8b010040
    // ret               -> 0xd65f03c0
    uint64_t inner_fn = AllocCode({0x8b000002u, 0x8b000042u, 0x8b010040u, 0xd65f03c0u});

    // Host SVC thunk: calls inner_fn(x0 + 1, x1 + 2) re-entrantly and returns result + 10.
    uint64_t svc_stub = cpu.AllocateSvcStub("test_reentrant_thunk", [&](CpuContext& ctx) {
        uint64_t a = ctx.GetX(0);
        uint64_t b = ctx.GetX(1);
        uint64_t r = cpu.CallGuest(inner_fn, {a + 1, b + 2});
        ctx.SetX(0, r + 10);
    });

    // Outer guest function: preserves x19, x29, x30 across BLR to host thunk, adds x19 back.
    // stp x29, x30, [sp, #-0x20]! -> 0xa9be7bfd
    // str x19, [sp, #0x10]        -> 0xf9000bf3
    // mov x19, #100               -> 0xd2800c93
    // blr x2                      -> 0xd63f0040  (x2 = svc_stub)
    // add x0, x0, x19             -> 0x8b130000
    // ldr x19, [sp, #0x10]        -> 0xf9400bf3
    // ldp x29, x30, [sp], #0x20   -> 0xa8c27bfd
    // ret                         -> 0xd65f03c0
    uint64_t outer_fn = AllocCode({
        0xa9be7bfdu,
        0xf9000bf3u,
        0xd2800c93u,
        0xd63f0040u,
        0x8b130000u,
        0xf9400bf3u,
        0xa8c27bfdu,
        0xd65f03c0u,
    });

    // inner(5+1, 7+2) = 6*3 + 9 = 27; thunk returns 37; outer adds 100 -> 137.
    uint64_t result = cpu.CallGuest(outer_fn, {5, 7, svc_stub});
    CHECK(result == 137);

    VirtualFree(reinterpret_cast<void*>(inner_fn), 0, MEM_RELEASE);
    VirtualFree(reinterpret_cast<void*>(outer_fn), 0, MEM_RELEASE);
    std::printf("  [PASS] test_svc_thunk_and_reentrancy (result=%llu)\n",
                static_cast<unsigned long long>(result));
}

void TestElfLoadRealLibs() {
    CpuRuntime cpu;
    ElfLoader loader(cpu);

    const std::filesystem::path bin_dir = R"(d:\Real Racing 3\port\work_bins\lib\arm64-v8a)";
    LoadedModule* libcpp = loader.LoadModule(bin_dir / "libc++_shared.so");
    LoadedModule* fmod = loader.LoadModule(bin_dir / "libfmodex.so");
    LoadedModule* rr3 = loader.LoadModule(bin_dir / "libRealRacing3.so");

    CHECK(libcpp != nullptr && libcpp->base != nullptr);
    CHECK(fmod != nullptr && fmod->base != nullptr);
    CHECK(rr3 != nullptr && rr3->base != nullptr);
    CHECK(rr3->sha256_hex.size() == 64);

    CHECK(loader.FindExport("Java_com_firemint_realracing_MainActivity_onCreateJNI") != 0);
    CHECK(loader.FindExport("Java_com_firemint_realracing_MainActivity_onViewRenderJNI") != 0);
    CHECK(loader.FindExport("Java_com_firemint_realracing_ControllerManager_SetJoystickValueJNI") != 0);
    CHECK(loader.FindExport("FMOD_System_Create") != 0);
    CHECK(loader.FindExport("_Znwm") != 0); // operator new from libc++_shared.so

    std::vector<std::string> unresolved;
    loader.ApplyRelocations(&unresolved);
    // All C++ symbols imported by libRealRacing3.so from libc++_shared.so and FMOD symbols from libfmodex.so
    // must now be resolved across modules (only external Android/libc/GL/AR/Anzu/Nimble/Fuel remain).
    for (const auto& name : unresolved) {
        CHECK(name != "_Znwm");
        CHECK(name != "FMOD_System_Create");
    }

    std::printf("  [PASS] test_elf_load_real_libs (rr3_sha256=%s, init_array=%zu, remaining_external=%zu)\n",
                rr3->sha256_hex.c_str(), rr3->init_array.size(), unresolved.size());
}
} // namespace

int main() {
    TestTpidrCanaryAndCall();
    TestSvcThunkAndReentrancy();
    TestElfLoadRealLibs();
    std::printf("test_elf_cpu ALL PASSED\n");
    return 0;
}
