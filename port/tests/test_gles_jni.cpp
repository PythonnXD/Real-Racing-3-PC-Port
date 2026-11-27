#include "config.h"
#include "cpu_dynarmic.h"
#include "elf_loader.h"
#include "gles_bridge.h"
#include "jni_env.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(expr)                                                                 \
    do {                                                                            \
        if (!(expr)) {                                                              \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

namespace {

void* AllocExecPage(const std::vector<uint32_t>& insns) {
    void* p = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(p != nullptr);
    std::memcpy(p, insns.data(), insns.size() * sizeof(uint32_t));
    return p;
}

void test_angle_and_guest_gl() {
    SDL_SetMainReady();
    CHECK(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_Window* win = SDL_CreateWindow("RR3 GL Test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                       320, 240, SDL_WINDOW_HIDDEN);
    CHECK(win != nullptr);
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    CHECK(SDL_GetWindowWMInfo(win, &wm));
    CHECK(wm.info.win.window != nullptr);

    CpuRuntime cpu;
    ElfLoader loader(cpu);
    GlesBridge gles(cpu);

    PcConfig cfg{};
    cfg.width = 320;
    cfg.height = 240;
    cfg.render_scale = 1.0f;
    cfg.anisotropy = 16;
    cfg.msaa_samples = 0;
    cfg.vsync = false;

    CHECK(gles.Init(wm.info.win.window, 320, 240, cfg));
    gles.RegisterSymbols(loader);

    std::string renderer = gles.GlRenderer();
    CHECK(!renderer.empty());
    CHECK(renderer.find("ANGLE") != std::string::npos);

    // Guest AArch64 routine: calls glGetString(GL_RENDERER=0x1F01) passed as function ptr in X0.
    std::vector<uint32_t> code = {
        0xAA0003E1, // mov x1, x0
        0x5283E020, // mov w0, #0x1f01
        0xD61F0020, // br  x1
    };
    void* page = AllocExecPage(code);
    uint64_t gl_get_str = loader.FindHostSymbol("glGetString");
    CHECK(gl_get_str != 0);

    uint64_t ret_ptr = cpu.CallGuest(reinterpret_cast<uint64_t>(page), {gl_get_str});
    CHECK(ret_ptr != 0);
    const char* guest_renderer = reinterpret_cast<const char*>(static_cast<uintptr_t>(ret_ptr));
    CHECK(std::strcmp(guest_renderer, renderer.c_str()) == 0);

    // Verify dynamic eglGetProcAddress returns a guest SVC stub (NOT raw host x86_64 pointer)
    uint64_t egl_gpa = loader.FindHostSymbol("eglGetProcAddress");
    CHECK(egl_gpa != 0);
    const char* ext_name = "glBindVertexArrayOES";
    uint64_t vao_stub = cpu.CallGuest(egl_gpa, {reinterpret_cast<uint64_t>(ext_name)});
    if (vao_stub != 0) {
        uint32_t first_insn = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(vao_stub));
        CHECK((first_insn & 0xFFE0001Fu) == 0xD4000001u);
    }

    // Verify clear + readback via guest glClearColor(0.0, 1.0, 0.0, 1.0) & glClear(GL_COLOR_BUFFER_BIT)
    uint64_t gl_clear_color = loader.FindHostSymbol("glClearColor");
    uint64_t gl_clear = loader.FindHostSymbol("glClear");
    CHECK(gl_clear_color && gl_clear);
    std::vector<uint32_t> clear_code = {
        0x1E2703E0, // fmov s0, wzr (0.0f)
        0x1E2E1001, // fmov s1, #1.0 (1.0f)
        0x1E2703E2, // fmov s2, wzr (0.0f)
        0x1E2E1003, // fmov s3, #1.0 (1.0f)
        0xD61F0000, // br x0
    };
    void* clear_page = AllocExecPage(clear_code);
    cpu.CallGuest(reinterpret_cast<uint64_t>(clear_page), {gl_clear_color});
    cpu.CallGuest(gl_clear, {0x00004000ULL}); // GL_COLOR_BUFFER_BIT

    std::vector<uint8_t> rgba;
    int rw = 0, rh = 0;
    CHECK(gles.ReadPixelsRgba(rgba, rw, rh));
    CHECK(rw == 320 && rh == 240);
    size_t mid = (static_cast<size_t>(120) * 320 + 160) * 4;
    CHECK(rgba[mid + 0] == 0 && rgba[mid + 1] == 255 && rgba[mid + 2] == 0);

    std::printf("  [PASS] test_angle_and_guest_gl (renderer='%s', aniso=%.0fx, clear_px=(%u,%u,%u))\n",
                renderer.c_str(), gles.AppliedAnisotropy(), rgba[mid], rgba[mid + 1], rgba[mid + 2]);

    VirtualFree(page, 0, MEM_RELEASE);
    VirtualFree(clear_page, 0, MEM_RELEASE);
    gles.Shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
}

void test_fake_jvm_roundtrip() {
    CpuRuntime cpu;
    FakeJvm jvm(cpu);

    JniClassDef cls;
    cls.name = "com/firemint/realracing/TestBridge";
    cls.static_methods["compute:(Ljava/lang/String;IF)I"] =
        [](FakeJvm& j, uint64_t, const std::vector<JniVal>& args) {
            CHECK(args.size() == 3);
            std::string s = j.GetStringUtf(args[0].AsObj());
            int32_t k = args[1].AsInt();
            float f = args[2].AsFloat();
            int32_t ans = static_cast<int32_t>(s.size()) * 100 + k + static_cast<int32_t>(f * 10.0f);
            return JniVal::Int(ans);
        };
    jvm.RegisterJniClass(std::move(cls));

    std::vector<uint32_t> code = {
        0xA9BB7BFD, // stp x29, x30, [sp, #-80]!
        0xA90153F3, // stp x19, x20, [sp, #16]
        0xA9025BF5, // stp x21, x22, [sp, #32]
        0xA90363F7, // stp x23, x24, [sp, #48]
        0xAA0003F3, // mov x19, x0 (env)
        0xAA0203F5, // mov x21, x2 (mname)
        0xAA0303F6, // mov x22, x3 (msig)
        0xAA0403F7, // mov x23, x4 (text)
        0xF9400274, // ldr x20, [x19] (vtable = *env)

        // FindClass(env, cls_name) -> x24 = cls
        0xF9401A88, // ldr x8, [x20, #0x30]
        0xD63F0100, // blr x8
        0xAA0003F8, // mov x24, x0

        // GetStaticMethodID(env, cls, mname, msig) -> x21 = mid
        0xAA1303E0, // mov x0, x19
        0xAA1803E1, // mov x1, x24
        0xAA1503E2, // mov x2, x21
        0xAA1603E3, // mov x3, x22
        0xF941C688, // ldr x8, [x20, #0x388]
        0xD63F0100, // blr x8
        0xAA0003F5, // mov x21, x0

        // NewStringUTF(env, text) -> x22 = jstr
        0xAA1303E0, // mov x0, x19
        0xAA1703E1, // mov x1, x23
        0xF9429E88, // ldr x8, [x20, #0x538]
        0xD63F0100, // blr x8
        0xAA0003F6, // mov x22, x0

        // CallStaticIntMethod(env, cls, mid, jstr, 17, (double)2.5)
        0xAA1303E0, // mov x0, x19
        0xAA1803E1, // mov x1, x24
        0xAA1503E2, // mov x2, x21
        0xAA1603E3, // mov x3, x22
        0xD2800224, // mov x4, #17
        0x1E609000, // fmov d0, #2.5
        0xF9420688, // ldr x8, [x20, #0x408]
        0xD63F0100, // blr x8

        // Epilogue
        0xA94363F7, // ldp x23, x24, [sp, #48]
        0xA9425BF5, // ldp x21, x22, [sp, #32]
        0xA94153F3, // ldp x19, x20, [sp, #16]
        0xA8C57BFD, // ldp x29, x30, [sp], #80
        0xD65F03C0, // ret
    };
    void* page = AllocExecPage(code);

    const char* cname = "com/firemint/realracing/TestBridge";
    const char* mname = "compute";
    const char* msig = "(Ljava/lang/String;IF)I";
    const char* text = "RR3PC"; // len=5 -> 5*100 + 17 + 25 = 542

    uint64_t res = cpu.CallGuest(reinterpret_cast<uint64_t>(page), {
        jvm.GetJniEnvGuestPtr(),
        reinterpret_cast<uint64_t>(cname),
        reinterpret_cast<uint64_t>(mname),
        reinterpret_cast<uint64_t>(msig),
        reinterpret_cast<uint64_t>(text),
    });
    CHECK(static_cast<int32_t>(res) == 542);

    // Also test JavaVM->GetEnv(vm, &out_env, JNI_VERSION_1_6)
    uint64_t out_env = 0;
    uint64_t vm_ptr = jvm.GetJavaVmGuestPtr();
    uint64_t* vm_vtable = *reinterpret_cast<uint64_t**>(static_cast<uintptr_t>(vm_ptr));
    uint64_t get_env_stub = vm_vtable[6];
    uint64_t rc = cpu.CallGuest(get_env_stub, {vm_ptr, reinterpret_cast<uint64_t>(&out_env), 0x00010006ULL});
    CHECK(rc == 0 && out_env == jvm.GetJniEnvGuestPtr());

    std::printf("  [PASS] test_fake_jvm_roundtrip (CallStaticIntMethod=%d, JavaVM::GetEnv OK)\n",
                static_cast<int32_t>(res));
    VirtualFree(page, 0, MEM_RELEASE);
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    test_angle_and_guest_gl();
    test_fake_jvm_roundtrip();
    std::printf("test_gles_jni ALL PASSED\n");
    return 0;
}
