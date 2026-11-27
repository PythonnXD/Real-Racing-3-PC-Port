#include "config.h"

#include <SDL.h>
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <zlib.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {
struct SmokeEnv final : Dynarmic::A64::UserCallbacks {
    uint32_t code[4] = {
        0x8b010000u, // add x0, x0, x1
        0xd4000021u, // svc #1 (halt)
    };
    Dynarmic::A64::Jit* jit = nullptr;

    std::optional<uint32_t> MemoryReadCode(Dynarmic::A64::VAddr vaddr) override {
        if (vaddr >= 0x1000 && vaddr < 0x1010) {
            return code[(vaddr - 0x1000) / 4];
        }
        return std::nullopt;
    }
    uint8_t MemoryRead8(Dynarmic::A64::VAddr) override { return 0; }
    uint16_t MemoryRead16(Dynarmic::A64::VAddr) override { return 0; }
    uint32_t MemoryRead32(Dynarmic::A64::VAddr) override { return 0; }
    uint64_t MemoryRead64(Dynarmic::A64::VAddr) override { return 0; }
    Dynarmic::A64::Vector MemoryRead128(Dynarmic::A64::VAddr) override { return {0, 0}; }
    void MemoryWrite8(Dynarmic::A64::VAddr, uint8_t) override {}
    void MemoryWrite16(Dynarmic::A64::VAddr, uint16_t) override {}
    void MemoryWrite32(Dynarmic::A64::VAddr, uint32_t) override {}
    void MemoryWrite64(Dynarmic::A64::VAddr, uint64_t) override {}
    void MemoryWrite128(Dynarmic::A64::VAddr, Dynarmic::A64::Vector) override {}
    void InterpreterFallback(Dynarmic::A64::VAddr, size_t) override { std::abort(); }
    void CallSVC(uint32_t) override { jit->HaltExecution(); }
    void ExceptionRaised(Dynarmic::A64::VAddr, Dynarmic::A64::Exception) override { std::abort(); }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1000000; }
    uint64_t GetCNTPCT() override { return 0; }
};
} // namespace

int main(int, char**) {
    // 1. Config test
    auto tmp = std::filesystem::temp_directory_path() / "rr3_pc_test_cfg.ini";
    {
        std::ofstream f(tmp);
        f << "width=2560\nheight=1440\nrender_scale=1.5\nanisotropy=16\nunlock_menu_fps=true\nvsync=false\n";
    }
    PcConfig cfg = LoadConfig(tmp);
    std::filesystem::remove(tmp);
    assert(cfg.width == 2560);
    assert(cfg.height == 1440);
    assert(cfg.render_scale == 1.5f);
    assert(cfg.anisotropy == 16);
    assert(cfg.unlock_menu_fps == true);
    assert(cfg.vsync == false);

    // 2. SDL2 + zlib link check
    SDL_version ver{};
    SDL_GetVersion(&ver);
    assert(ver.major == 2);
    assert(zlibVersion() != nullptr);

    // 3. Dynarmic A64 JIT smoke test
    SmokeEnv env;
    Dynarmic::ExclusiveMonitor monitor(1);
    Dynarmic::A64::UserConfig ucfg;
    ucfg.callbacks = &env;
    ucfg.global_monitor = &monitor;
    ucfg.enable_cycle_counting = false;
    Dynarmic::A64::Jit jit(ucfg);
    env.jit = &jit;
    jit.SetPC(0x1000);
    jit.SetRegister(0, 19);
    jit.SetRegister(1, 23);
    jit.Run();
    uint64_t res = jit.GetRegister(0);
    assert(res == 42);

    std::printf("test_smoke PASS (SDL %d.%d.%d, zlib %s, Dynarmic 19+23=%llu)\n",
                ver.major, ver.minor, ver.patch, zlibVersion(), static_cast<unsigned long long>(res));
    return 0;
}
