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

void test_multi_gpu_extensions_and_decoders() {
    // 1. Verify guest GL_EXTENSIONS curation for all 4 RR3 GPU texture packs
    {
        SDL_SetMainReady();
        CHECK(SDL_Init(SDL_INIT_VIDEO) == 0);
        SDL_Window* win = SDL_CreateWindow("RR3 Multi-GPU Test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                           160, 120, SDL_WINDOW_HIDDEN);
        CHECK(win != nullptr);
        SDL_SysWMinfo wm{};
        SDL_VERSION(&wm.version);
        CHECK(SDL_GetWindowWMInfo(win, &wm));

        PcConfig cfg{};
        cfg.width = 160;
        cfg.height = 120;
        cfg.render_scale = 1.0f;
        cfg.vsync = false;

        auto probe_ext = [&](DepotGpuFormat fmt) -> std::string {
            CpuRuntime cpu;
            GlesBridge gles(cpu);
            gles.SetTargetGpuFormat(fmt);
            CHECK(gles.Init(wm.info.win.window, 160, 120, cfg));
            std::string ext = gles.GuestExtensions();
            gles.Shutdown();
            return ext;
        };

        const std::string ext_atc = probe_ext(DepotGpuFormat::Atc);
        CHECK(ext_atc.find("GL_AMD_compressed_ATC_texture") != std::string::npos);
        CHECK(ext_atc.find("GL_OES_compressed_ETC1_RGB8_texture") != std::string::npos);
        CHECK(ext_atc.find("GL_IMG_texture_compression_pvrtc") == std::string::npos);
        CHECK(ext_atc.find("texture_compression_s3tc") == std::string::npos);

        const std::string ext_etc = probe_ext(DepotGpuFormat::Etc);
        CHECK(ext_etc.find("GL_OES_compressed_ETC1_RGB8_texture") != std::string::npos);
        CHECK(ext_etc.find("GL_AMD_compressed_ATC_texture") == std::string::npos);
        CHECK(ext_etc.find("GL_IMG_texture_compression_pvrtc") == std::string::npos);
        CHECK(ext_etc.find("texture_compression_s3tc") == std::string::npos);
        CHECK(ext_etc.find("texture_compression_dxt") == std::string::npos);

        const std::string ext_dxt = probe_ext(DepotGpuFormat::Dxt);
        CHECK(ext_dxt.find("GL_EXT_texture_compression_s3tc") != std::string::npos);
        CHECK(ext_dxt.find("GL_EXT_texture_compression_dxt1") != std::string::npos);
        CHECK(ext_dxt.find("GL_ANGLE_texture_compression_dxt3") != std::string::npos);
        CHECK(ext_dxt.find("GL_ANGLE_texture_compression_dxt5") != std::string::npos);
        CHECK(ext_dxt.find("GL_OES_compressed_ETC1_RGB8_texture") != std::string::npos);
        CHECK(ext_dxt.find("GL_AMD_compressed_ATC_texture") == std::string::npos);
        CHECK(ext_dxt.find("GL_IMG_texture_compression_pvrtc") == std::string::npos);

        const std::string ext_pvr = probe_ext(DepotGpuFormat::Pvrtc);
        CHECK(ext_pvr.find("GL_IMG_texture_compression_pvrtc") != std::string::npos);
        CHECK(ext_pvr.find("GL_OES_compressed_ETC1_RGB8_texture") != std::string::npos);
        CHECK(ext_pvr.find("GL_AMD_compressed_ATC_texture") == std::string::npos);
        CHECK(ext_pvr.find("texture_compression_s3tc") == std::string::npos);

        SDL_DestroyWindow(win);
        SDL_Quit();
    }

    // 2. Verify CPU DXT1 (4-color + punch-through), DXT3, and DXT5 decoders
    {
        std::vector<uint8_t> rgba;
        // DXT1 4-color: c0=0xF800 (red), c1=0x001F (blue), pixels 0..3 -> indices 0,1,2,3
        const uint8_t dxt1_4col[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00};
        CHECK(GlesBridge::DecodeCompressedTextureToRgba(0x83F1u, 4, 4, dxt1_4col, 8, rgba));
        CHECK(rgba.size() == 64);
        CHECK(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);     // idx 0 = red
        CHECK(rgba[4] == 0 && rgba[5] == 0 && rgba[6] == 255 && rgba[7] == 255);     // idx 1 = blue
        CHECK(rgba[8] == 170 && rgba[9] == 0 && rgba[10] == 85 && rgba[11] == 255);  // idx 2 = 2/3 red + 1/3 blue
        CHECK(rgba[12] == 85 && rgba[13] == 0 && rgba[14] == 170 && rgba[15] == 255);// idx 3 = 1/3 red + 2/3 blue

        // DXT1 3-color + punch-through (c0=0x001F <= c1=0xF800): idx 3 = transparent black on RGBA, opaque black on RGB
        const uint8_t dxt1_punch[8] = {0x1F, 0x00, 0x00, 0xF8, 0xE4, 0x00, 0x00, 0x00};
        CHECK(GlesBridge::DecodeCompressedTextureToRgba(0x83F1u, 4, 4, dxt1_punch, 8, rgba));
        CHECK(rgba[12] == 0 && rgba[13] == 0 && rgba[14] == 0 && rgba[15] == 0);
        CHECK(GlesBridge::DecodeCompressedTextureToRgba(0x83F0u, 4, 4, dxt1_punch, 8, rgba));
        CHECK(rgba[12] == 0 && rgba[13] == 0 && rgba[14] == 0 && rgba[15] == 255);

        // DXT3: explicit 4-bit alpha (px0=0x0->0, px1=0x8->136, px2=0xF->255) + 4-color RGB
        uint8_t dxt3[16]{};
        dxt3[0] = 0x80; dxt3[1] = 0x0F;
        std::memcpy(dxt3 + 8, dxt1_4col, 8);
        CHECK(GlesBridge::DecodeCompressedTextureToRgba(0x83F2u, 4, 4, dxt3, 16, rgba));
        CHECK(rgba[3] == 0 && rgba[7] == 136 && rgba[11] == 255 && rgba[0] == 255);

        // DXT5: interpolated alpha (a0=200, a1=60 -> idx0=200, idx1=60, idx2=180)
        uint8_t dxt5[16]{};
        dxt5[0] = 200; dxt5[1] = 60;
        // 3-bit indices for px0=0, px1=1, px2=2: bits = 0 | (1<<3) | (2<<6) = 0x88
        dxt5[2] = 0x88;
        std::memcpy(dxt5 + 8, dxt1_4col, 8);
        CHECK(GlesBridge::DecodeCompressedTextureToRgba(0x83F3u, 4, 4, dxt5, 16, rgba));
        CHECK(rgba[3] == 200 && rgba[7] == 60 && rgba[11] == 180);
    }

    // 3. Verify CPU PVRTC1 4bpp & 2bpp RGB/RGBA decoders (including small mip & punch-through)
    {
        auto write_u32 = [](uint8_t* p, uint32_t v) {
            p[0] = static_cast<uint8_t>(v & 0xFF);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
        };

        // 4bpp 8x8 (4 blocks = 32 bytes): Color A = opaque red (31,0,0), Color B = opaque blue (0,0,31)
        const uint32_t col_red_blue = 0x8000u | (31u << 10) | 0x80000000u | (31u << 16);
        // First 4 pixels in each block: mod codes 0, 3, 1, 2 -> bits = 0 | (3<<2) | (1<<4) | (2<<6) = 0x9C
        uint8_t pvr4[32]{};
        for (int b = 0; b < 4; ++b) {
            write_u32(pvr4 + b * 8, 0x0000009Cu);
            write_u32(pvr4 + b * 8 + 4, col_red_blue);
        }
        std::vector<uint8_t> rgba;
        CHECK(GlesBridge::DecodeCompressedTextureToRgba(0x8C02u, 8, 8, pvr4, 32, rgba));
        CHECK(rgba.size() == 8 * 8 * 4);
        CHECK(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);      // mod 0 -> A (red)
        CHECK(rgba[4] == 0 && rgba[5] == 0 && rgba[6] == 255 && rgba[7] == 255);      // mod 3 -> B (blue)
        CHECK(rgba[8] == 159 && rgba[9] == 0 && rgba[10] == 95 && rgba[11] == 255);   // mod 1 -> 5/8 A + 3/8 B
        CHECK(rgba[12] == 95 && rgba[13] == 0 && rgba[14] == 159 && rgba[15] == 255); // mod 2 -> 3/8 A + 5/8 B

        // 4bpp punch-through mode (bit0=1, code 2 -> alpha=0 on RGBA, alpha=255 on RGB)
        uint8_t pvr4_pt[32]{};
        for (int b = 0; b < 4; ++b) {
            write_u32(pvr4_pt + b * 8, 0x0000009Cu);
            write_u32(pvr4_pt + b * 8 + 4, col_red_blue | 1u);
        }
        CHECK(GlesBridge::DecodeCompressedTextureToRgba(0x8C02u, 4, 4, pvr4_pt, 32, rgba));
        CHECK(rgba.size() == 4 * 4 * 4);
        CHECK(rgba[12] == 127 && rgba[14] == 127 && rgba[15] == 0);
        CHECK(GlesBridge::DecodeCompressedTextureToRgba(0x8C00u, 4, 4, pvr4_pt, 32, rgba));
        CHECK(rgba[15] == 255);

        // 2bpp 16x8 (4 blocks = 32 bytes): mode 0, px0=0 (A red), px1=1 (B blue)
        uint8_t pvr2[32]{};
        for (int b = 0; b < 4; ++b) {
            write_u32(pvr2 + b * 8, 0x00000002u);
            write_u32(pvr2 + b * 8 + 4, col_red_blue);
        }
        CHECK(GlesBridge::DecodeCompressedTextureToRgba(0x8C03u, 16, 8, pvr2, 32, rgba));
        CHECK(rgba.size() == 16 * 8 * 4);
        CHECK(rgba[0] == 255 && rgba[2] == 0 && rgba[3] == 255);
        CHECK(rgba[4] == 0 && rgba[6] == 255 && rgba[7] == 255);
    }

    std::printf("  [PASS] test_multi_gpu_extensions_and_decoders (ATC/ETC1/DXT/PVRTC ext curation + DXT1/3/5 & PVRTC1 4bpp/2bpp decoders)\n");
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    test_angle_and_guest_gl();
    test_fake_jvm_roundtrip();
    test_multi_gpu_extensions_and_decoders();
    std::printf("test_gles_jni ALL PASSED\n");
    return 0;
}
