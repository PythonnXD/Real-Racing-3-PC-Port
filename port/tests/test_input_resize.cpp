#include "config.h"
#include "cpu_dynarmic.h"
#include "dist_tools.h"
#include "elf_loader.h"
#include "gles_bridge.h"
#include "input_pc.h"
#include "jni_env.h"
#include "vfs.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define CHECK(expr)                                                                         \
    do {                                                                                    \
        if (!(expr)) {                                                                      \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            std::exit(1);                                                                   \
        }                                                                                   \
    } while (0)

namespace {

void TestKeyboardSmoothSteeringRamp() {
    PcConfig cfg;
    cfg.steer_ramp_rate = 5.0f;   // full lock in 0.2s
    cfg.steer_return_rate = 10.0f; // return in 0.1s

    PcInput in;
    in.Init(cfg);

    // Hold D (steer right) for 100ms -> should be +0.5
    in.SetKeyState(SDLK_d, true);
    in.UpdateFrame(0.05f);
    in.UpdateFrame(0.05f);
    CHECK(std::fabs(in.KeyboardSteer() - 0.5f) < 0.02f);

    // Continue another 150ms -> clamped at +1.0
    in.UpdateFrame(0.05f);
    in.UpdateFrame(0.05f);
    in.UpdateFrame(0.05f);
    CHECK(std::fabs(in.KeyboardSteer() - 1.0f) < 0.01f);

    // Release D for 50ms -> returns at 10.0/s -> 0.5
    in.SetKeyState(SDLK_d, false);
    in.UpdateFrame(0.05f);
    CHECK(std::fabs(in.KeyboardSteer() - 0.5f) < 0.02f);

    // Counter-steer left (A) -> snaps faster across center
    in.SetKeyState(SDLK_a, true);
    in.UpdateFrame(0.05f); // rate = 5 + 10 = 15/s -> step 0.75 -> -0.25
    CHECK(in.KeyboardSteer() < 0.0f);

    // Throttle / Brake mapping
    in.SetKeyState(SDLK_w, true);
    in.SetKeyState(SDLK_s, false);
    in.UpdateFrame(0.016f);
    CHECK(in.KeyboardThrottle() == 1.0f && in.KeyboardBrake() == 0.0f);

    in.SetKeyState(SDLK_w, false);
    in.SetKeyState(SDLK_s, true);
    in.UpdateFrame(0.016f);
    CHECK(in.KeyboardThrottle() == 0.0f && in.KeyboardBrake() == 1.0f);

    // Verify SetButtonValueJNI ABI: (JNIEnv*, jclass, int deviceId, boolean pressed, int buttonOrdinal)
    {
        CpuRuntime cpu;
        ElfLoader loader(cpu);
        FakeJvm jvm(cpu);
        JniClassDef cm;
        cm.name = "com/firemint/realracing/ControllerManager";
        jvm.RegisterJniClass(std::move(cm));

        struct BtnCall { int32_t dev; uint32_t pressed; int32_t ord; };
        std::vector<BtnCall> calls;
        loader.RegisterHostFunction("Java_com_firemint_realracing_ControllerManager_ControllerConnectedJNI",
                                    [](CpuContext&) {});
        loader.RegisterHostFunction("Java_com_firemint_realracing_ControllerManager_SetJoystickValueJNI",
                                    [](CpuContext&) {});
        loader.RegisterHostFunction("Java_com_firemint_realracing_ControllerManager_SetButtonValueJNI",
                                    [&calls](CpuContext& c) {
                                        calls.push_back({static_cast<int32_t>(c.GetW(2)),
                                                         c.GetW(3),
                                                         static_cast<int32_t>(c.GetW(4))});
                                    });
        PcInput kb;
        kb.Init(cfg);
        kb.SetKeyState(SDLK_c, true); // Camera -> Rr3Button::Y (ordinal 3)
        kb.UpdateFrame(0.016f);
        kb.DispatchToGuest(cpu, loader, jvm, 0);
        CHECK(calls.size() == 1);
        CHECK(calls[0].dev == PcInput::kKeyboardDeviceId);
        CHECK(calls[0].pressed == 1u);
        CHECK(calls[0].ord == static_cast<int32_t>(Rr3Button::Y));

        kb.ResetAllInputs();
        kb.DispatchToGuest(cpu, loader, jvm, 0);
        CHECK(calls.size() == 2 && calls[1].pressed == 0u && calls[1].ord == static_cast<int32_t>(Rr3Button::Y));
    }

    // Verify keyboard remapping: Space -> Brake (X + L2 + LTrigger, NOT L1 rear_view),
    // V -> Look Back (L1), C -> Camera (Y), Esc -> Pause (Start) + Android Back keycode 4
    {
        CpuRuntime cpu;
        ElfLoader loader(cpu);
        FakeJvm jvm(cpu);
        JniClassDef cm;
        cm.name = "com/firemint/realracing/ControllerManager";
        jvm.RegisterJniClass(std::move(cm));

        struct BtnCall { int32_t dev; uint32_t pressed; int32_t ord; };
        std::vector<BtnCall> btn_calls;
        std::vector<std::pair<int32_t, bool>> android_keys;
        loader.RegisterHostFunction("Java_com_firemint_realracing_ControllerManager_ControllerConnectedJNI",
                                    [](CpuContext&) {});
        loader.RegisterHostFunction("Java_com_firemint_realracing_ControllerManager_SetJoystickValueJNI",
                                    [](CpuContext&) {});
        loader.RegisterHostFunction("Java_com_firemint_realracing_ControllerManager_SetButtonValueJNI",
                                    [&btn_calls](CpuContext& c) {
                                        btn_calls.push_back({
                                            static_cast<int32_t>(c.GetW(2)),
                                            c.GetW(3),
                                            static_cast<int32_t>(c.GetW(4)),
                                        });
                                    });
        loader.RegisterHostFunction("Java_com_firemint_realracing_MainActivity_onKeyPressed",
                                    [&android_keys](CpuContext& c) {
                                        android_keys.emplace_back(static_cast<int32_t>(c.GetW(2)), true);
                                    });

        PcInput kb;
        kb.Init(cfg);
        // Space -> Brake (X + L2, must NOT trigger L1 rear_view)
        kb.SetKeyState(SDLK_SPACE, true);
        kb.UpdateFrame(0.016f);
        CHECK(kb.KeyboardBrake() == 1.0f);
        kb.DispatchToGuest(cpu, loader, jvm, 1);
        bool saw_x = false, saw_l2 = false, saw_l1 = false;
        for (const auto& b : btn_calls) {
            if (b.pressed && b.ord == static_cast<int32_t>(Rr3Button::X)) saw_x = true;
            if (b.pressed && b.ord == static_cast<int32_t>(Rr3Button::L2)) saw_l2 = true;
            if (b.pressed && b.ord == static_cast<int32_t>(Rr3Button::L1)) saw_l1 = true;
        }
        CHECK(saw_x && saw_l2 && !saw_l1);
        kb.SetKeyState(SDLK_SPACE, false);
        kb.DispatchToGuest(cpu, loader, jvm, 1);
        btn_calls.clear();

        // V -> Look Back (L1 = rear_view), C -> Camera (Y), Esc -> Pause (Start) + Android Back keycode 4
        kb.SetKeyState(SDLK_v, true);
        kb.SetKeyState(SDLK_c, true);
        kb.SetKeyState(SDLK_ESCAPE, true);
        kb.DispatchToGuest(cpu, loader, jvm, 1);
        bool saw_v_rear = false, saw_c_cam = false, saw_esc_pause = false, saw_b = false;
        for (const auto& b : btn_calls) {
            if (b.pressed && b.ord == static_cast<int32_t>(Rr3Button::L1)) saw_v_rear = true;
            if (b.pressed && b.ord == static_cast<int32_t>(Rr3Button::Y)) saw_c_cam = true;
            if (b.pressed && b.ord == static_cast<int32_t>(Rr3Button::Start)) saw_esc_pause = true;
            if (b.pressed && b.ord == static_cast<int32_t>(Rr3Button::B)) saw_b = true;
        }
        CHECK(saw_v_rear && saw_c_cam && saw_esc_pause && !saw_b);
        CHECK(android_keys.size() == 1 && android_keys[0].first == 4 && android_keys[0].second);
    }

    std::printf("  [PASS] test_keyboard_smooth_steering_ramp (incl. Space=Brake, V=LookBack, C=Cam, Esc=Pause)\n");
}

void TestVirtualGamepadProfilesAndDetection() {
    CHECK(PcInput::DetectControllerFamily("Xbox Wireless Controller", SDL_CONTROLLER_TYPE_XBOXONE) == "Xbox/XInput");
    CHECK(PcInput::DetectControllerFamily("PS4 Controller", SDL_CONTROLLER_TYPE_PS4) == "DualShock 4");
    CHECK(PcInput::DetectControllerFamily("DualSense Wireless Controller", SDL_CONTROLLER_TYPE_PS5) == "DualSense");
    CHECK(PcInput::DetectControllerFamily("Nintendo Switch Pro Controller", SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO) == "Switch Pro");

    // Attach a real SDL2 virtual game controller and verify hot-plug + axis + button + disconnect!
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.button_mask = 0xFFFF;
    desc.axis_mask = 0x3F;
    desc.name = "DualSense Virtual Controller";
    int dev_idx = SDL_JoystickAttachVirtualEx(&desc);
    CHECK(dev_idx >= 0);

    SDL_PumpEvents();
    PcConfig cfg;
    PcInput in;
    in.Init(cfg);
    CHECK(in.ConnectedGamepadCount() >= 1);

    auto names = in.ConnectedGamepadNames();
    bool found_ds = false;
    for (const auto& n : names) {
        if (n.find("DualSense") != std::string::npos) found_ds = true;
    }
    CHECK(found_ds);

    // Synthesize controller axis & button SDL events and verify exact remapped RR3 ordinals:
    // RB (RIGHTSHOULDER) -> L1 (rear_view), LB (LEFTSHOULDER) -> R1 (camera), A/RT -> throttle, X/LT -> brake
    CpuRuntime cpu;
    ElfLoader loader(cpu);
    FakeJvm jvm(cpu);
    JniClassDef cm;
    cm.name = "com/firemint/realracing/ControllerManager";
    jvm.RegisterJniClass(std::move(cm));
    std::vector<std::pair<int32_t, int32_t>> pad_btns;
    loader.RegisterHostFunction("Java_com_firemint_realracing_ControllerManager_ControllerConnectedJNI",
                                [](CpuContext&) {});
    loader.RegisterHostFunction("Java_com_firemint_realracing_ControllerManager_SetJoystickValueJNI",
                                [](CpuContext&) {});
    loader.RegisterHostFunction("Java_com_firemint_realracing_ControllerManager_SetButtonValueJNI",
                                [&pad_btns](CpuContext& c) {
                                    if (c.GetW(3)) {
                                        pad_btns.emplace_back(static_cast<int32_t>(c.GetW(2)),
                                                              static_cast<int32_t>(c.GetW(4)));
                                    }
                                });

    SDL_Event ev{};
    ev.type = SDL_CONTROLLERAXISMOTION;
    ev.caxis.which = SDL_JoystickGetDeviceInstanceID(dev_idx);
    ev.caxis.axis = SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
    ev.caxis.value = 32767;
    in.ProcessEvent(ev);

    auto send_pad_btn = [&](Uint8 sdl_btn) {
        SDL_Event be{};
        be.type = SDL_CONTROLLERBUTTONDOWN;
        be.cbutton.which = SDL_JoystickGetDeviceInstanceID(dev_idx);
        be.cbutton.button = sdl_btn;
        in.ProcessEvent(be);
    };
    send_pad_btn(SDL_CONTROLLER_BUTTON_A);
    send_pad_btn(SDL_CONTROLLER_BUTTON_X);
    send_pad_btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER); // RB / PS R1 -> L1 (rear_view)
    send_pad_btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);  // LB / PS L1 -> R1 (camera)
    in.DispatchToGuest(cpu, loader, jvm, 0);

    bool pad_a = false, pad_x = false, pad_rb_rear = false, pad_lb_cam = false;
    for (auto [dev, ord] : pad_btns) {
        if (dev >= 2000) {
            if (ord == static_cast<int32_t>(Rr3Button::A)) pad_a = true;
            if (ord == static_cast<int32_t>(Rr3Button::X)) pad_x = true;
            if (ord == static_cast<int32_t>(Rr3Button::L1)) pad_rb_rear = true;
            if (ord == static_cast<int32_t>(Rr3Button::R1)) pad_lb_cam = true;
        }
    }
    CHECK(pad_a && pad_x && pad_rb_rear && pad_lb_cam);

    // Detach virtual controller and process removal event
    SDL_JoystickID jid = SDL_JoystickGetDeviceInstanceID(dev_idx);
    CHECK(SDL_JoystickDetachVirtual(dev_idx) == 0);
    ev.type = SDL_CONTROLLERDEVICEREMOVED;
    ev.cdevice.which = jid;
    in.ProcessEvent(ev);
    CHECK(in.ConnectedGamepadCount() == 0);

    std::printf("  [PASS] test_virtual_gamepad_profiles_and_detection (Xbox/DS4/DualSense/SwitchPro + SDL virtual hotplug)\n");
}

void TestDynamicResizeAndSupersamplingFbo() {
    SDL_Window* win = SDL_CreateWindow("RR3 Resize Test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                       1280, 720, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    CHECK(win != nullptr);
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    CHECK(SDL_GetWindowWMInfo(win, &wm));

    CpuRuntime cpu;
    GlesBridge gles(cpu);
    PcConfig cfg;
    cfg.width = 1280;
    cfg.height = 720;
    cfg.render_scale = 1.5f; // 1920x1080 supersampled!
    cfg.anisotropy = 16;
    cfg.vsync = false;

    CHECK(gles.Init(wm.info.win.window, 1280, 720, cfg));
    CHECK(gles.RenderWidth() == 1920 && gles.RenderHeight() == 1080);

    // Minimized 0x0 resize must be safely ignored
    gles.Resize(0, 0);
    CHECK(gles.RenderWidth() == 1920 && gles.RenderHeight() == 1080);

    // Resize to 1600x900 -> 2400x1350 supersampled FBO
    gles.Resize(1600, 900);
    CHECK(gles.RenderWidth() == 2400 && gles.RenderHeight() == 1350);
    gles.SwapBuffers();

    std::vector<uint8_t> px;
    int rw = 0, rh = 0;
    CHECK(gles.ReadPixelsRgba(px, rw, rh));
    CHECK(rw == 2400 && rh == 1350);

    gles.Shutdown();

    // Fixed-height dynamic-aspect render FBO mode: keeps UI scale fixed (900p) while eliminating
    // black bars on ANY landscape window shape (1312x738, 1600x829 maximized, 1200x800, 1600x900)
    cfg.render_scale = 1.0f;
    CHECK(gles.Init(wm.info.win.window, 1312, 738, cfg, 900));
    CHECK(gles.IsFixedHeightDynamicAspect());
    CHECK(gles.RenderWidth() == 1600 && gles.RenderHeight() == 900);
    int vx = 0, vy = 0, vw = 0, vh = 0;
    gles.GetPresentRect(vx, vy, vw, vh);
    CHECK(vx == 0 && vy == 0 && vw == 1312 && vh == 738);

    // Fullscreen 1600x900 (same 16:9 aspect) -> no FBO resize needed, zero black bars
    CHECK(!gles.Resize(1600, 900));
    CHECK(gles.RenderWidth() == 1600 && gles.RenderHeight() == 900);
    gles.GetPresentRect(vx, vy, vw, vh);
    CHECK(vx == 0 && vy == 0 && vw == 1600 && vh == 900);

    // Maximized window 1600x829 (1.93:1) -> render_w adapts to 1736x900, ZERO black bars!
    CHECK(gles.Resize(1600, 829));
    CHECK(gles.RenderWidth() == 1736 && gles.RenderHeight() == 900);
    gles.GetPresentRect(vx, vy, vw, vh);
    CHECK(vx == 0 && vy == 0 && vw == 1600 && vh == 829);

    // Narrow 3:2 window 1200x800 -> render_w adapts to 1350x900, ZERO black bars!
    CHECK(gles.Resize(1200, 800));
    CHECK(gles.RenderWidth() == 1350 && gles.RenderHeight() == 900);
    gles.GetPresentRect(vx, vy, vw, vh);
    CHECK(vx == 0 && vy == 0 && vw == 1200 && vh == 800);
    gles.Shutdown();

    SDL_DestroyWindow(win);
    std::printf("  [PASS] test_dynamic_resize_and_supersampling_fbo (fixed-height 900p dynamic aspect: 1600x829->1738x900, 1200x800->1350x900, zero black bars)\n");
}

void TestWindowAutoFitAndModes() {
    // 1. User's 1600x900 screen with 40px taskbar (usable=1600x860) and Windows frame borders (31,8,8,8):
    //    Even if config requests 1920x1080, Windowed mode must clamp strictly inside usable bounds!
    WindowGeometry clamped = ComputeSafeWindowGeometry(
        WindowMode::Windowed, 1920, 1080,
        0, 0, 1600, 900,
        0, 0, 1600, 860,
        31, 8, 8, 8);
    CHECK(clamped.w + 16 <= 1600);
    CHECK(clamped.h + 39 <= 860);
    CHECK(clamped.x - 8 >= 0);
    CHECK(clamped.y - 31 >= 0);
    CHECK(clamped.x + clamped.w + 8 <= 1600);
    CHECK(clamped.y + clamped.h + 8 <= 860);
    CHECK(clamped.w > clamped.h);

    // 2. Auto-detect (0x0) on 1600x900 fits comfortably inside usable bounds at 16:9
    WindowGeometry autog = ComputeSafeWindowGeometry(
        WindowMode::Windowed, 0, 0,
        0, 0, 1600, 900,
        0, 0, 1600, 860,
        31, 8, 8, 8);
    CHECK(autog.w >= 640 && autog.h >= 360);
    CHECK(autog.w + 16 <= 1600 && autog.h + 39 <= 860);
    CHECK(autog.x - 8 >= 0 && autog.y - 31 >= 0);

    // 3. Borderless & Fullscreen fill full display bounds (1600x900 at 0,0)
    WindowGeometry bl = ComputeSafeWindowGeometry(
        WindowMode::Borderless, 0, 0,
        0, 0, 1600, 900,
        0, 0, 1600, 860,
        31, 8, 8, 8);
    CHECK(bl.x == 0 && bl.y == 0 && bl.w == 1600 && bl.h == 900);

    WindowGeometry fs = ComputeSafeWindowGeometry(
        WindowMode::Fullscreen, 1920, 1080,
        0, 0, 1600, 900,
        0, 0, 1600, 860,
        31, 8, 8, 8);
    CHECK(fs.x == 0 && fs.y == 0 && fs.w == 1600 && fs.h == 900);

    // 4. ParseWindowMode covers all three modes + aliases
    CHECK(ParseWindowMode("windowed") == WindowMode::Windowed);
    CHECK(ParseWindowMode("borderless") == WindowMode::Borderless);
    CHECK(ParseWindowMode("borderless_windowed") == WindowMode::Borderless);
    CHECK(ParseWindowMode("fullscreen") == WindowMode::Fullscreen);

    // 5. Verify on real Windows HWND that SDL_SetWindowPosition/Size stays strictly inside usable work area!
    SDL_Rect full_b{0, 0, 1280, 720}, usable_b{0, 0, 1280, 720};
    SDL_GetDisplayBounds(0, &full_b);
    SDL_GetDisplayUsableBounds(0, &usable_b);
    SDL_Window* w = SDL_CreateWindow("RR3 Fit Test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    CHECK(w != nullptr);
    int bt = 31, bl_px = 8, bb = 8, br = 8;
    SDL_GetWindowBordersSize(w, &bt, &bl_px, &bb, &br);
    WindowGeometry real_g = ComputeSafeWindowGeometry(
        WindowMode::Windowed, 3840, 2160,
        full_b.x, full_b.y, full_b.w, full_b.h,
        usable_b.x, usable_b.y, usable_b.w, usable_b.h,
        bt, bl_px, bb, br);
    SDL_SetWindowSize(w, real_g.w, real_g.h);
    SDL_SetWindowPosition(w, real_g.x, real_g.y);
    SDL_ShowWindow(w);
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    CHECK(SDL_GetWindowWMInfo(w, &wm) && wm.info.win.window != nullptr);
    RECT outer{};
    CHECK(GetWindowRect(wm.info.win.window, &outer) != 0);
    // Allow DWM invisible shadow tolerance, never overflowing usable screen
    CHECK(outer.left >= usable_b.x - 10 && outer.top >= usable_b.y - 2);
    CHECK(outer.right <= usable_b.x + usable_b.w + 10 && outer.bottom <= usable_b.y + usable_b.h + 10);

    // 6. Verify Borderless and Fullscreen transitions reach exact native display pixel dimensions
    SDL_SetWindowBordered(w, SDL_FALSE);
    SDL_SetWindowResizable(w, SDL_FALSE);
    SDL_SetWindowSize(w, full_b.w, full_b.h);
    SDL_SetWindowPosition(w, full_b.x, full_b.y);
    SDL_PumpEvents();
    int bw = 0, bh = 0;
    SDL_GetWindowSizeInPixels(w, &bw, &bh);
    CHECK(bw == full_b.w && bh == full_b.h);

    SDL_DisplayMode dm{};
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        SDL_SetWindowDisplayMode(w, &dm);
    }
    if (SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN) != 0) {
        CHECK(SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0);
    }
    SDL_PumpEvents();
    int fw = 0, fh = 0;
    SDL_GetWindowSizeInPixels(w, &fw, &fh);
    CHECK(fw == full_b.w && fh == full_b.h);
    SDL_SetWindowFullscreen(w, 0);
    SDL_DestroyWindow(w);

    std::printf("  [PASS] test_window_autofit_and_modes (1600x900 clamp=%dx%d auto=%dx%d, host outer=%ldx%ld within usable=%dx%d, borderless=%dx%d, fullscreen=%dx%d)\n",
                clamped.w, clamped.h, autog.w, autog.h,
                outer.right - outer.left, outer.bottom - outer.top, usable_b.w, usable_b.h,
                bw, bh, fw, fh);
}

void TestConfigQualityVerboseAndDistTools() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "rr3_pc_dist_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    // 1. Config save + reload round-trip (QualityPreset Low/Med/High/Ultra + Verbose + VSync)
    PcConfig out_cfg;
    out_cfg.window_mode = WindowMode::Borderless;
    out_cfg.vsync = false;
    out_cfg.quality_preset = QualityPreset::Medium;
    out_cfg.verbose_logs = true;
    out_cfg.fps_cap = 144;
    out_cfg.render_scale = 1.25f;
    out_cfg.anisotropy = 8;
    const fs::path ini = tmp / "test_rr3.ini";
    CHECK(SaveConfig(ini, out_cfg));
    PcConfig in_cfg = LoadConfig(ini);
    CHECK(in_cfg.window_mode == WindowMode::Borderless);
    CHECK(!in_cfg.vsync);
    CHECK(in_cfg.quality_preset == QualityPreset::Medium);
    CHECK(in_cfg.verbose_logs);
    CHECK(in_cfg.fps_cap == 144);
    CHECK(std::fabs(in_cfg.render_scale - 1.25f) < 0.01f);
    CHECK(in_cfg.anisotropy == 8);

    // 2. Verify all 4 native Android quality profile files exist in stock .depot
    const fs::path stock_ro = R"(d:\Real Racing 3\com.ea.games.r3_row\files)";
    if (fs::is_directory(stock_ro / ".depot", ec)) {
        LayeredVfs vfs;
        vfs.Initialize(stock_ro, tmp / "vfs_ud");
        for (QualityPreset qp : {QualityPreset::Low, QualityPreset::Medium, QualityPreset::High, QualityPreset::Ultra}) {
            std::string stem = QualityPresetToAndroidStem(qp);
            auto p1 = vfs.ResolveRead("/storage/rr3/.depot/eds/" + stem + ".plist");
            auto p2 = vfs.ResolveRead("/storage/rr3/.depot/feats/" + stem + ".feats");
            CHECK(fs::is_regular_file(p1, ec));
            CHECK(fs::is_regular_file(p2, ec));
        }
    }

    // 3. Export & Import user save data + safety auto-backup verification
    const fs::path ud = tmp / "userdata";
    fs::create_directories(ud / "doc", ec);
    {
        std::ofstream f(ud / "doc" / "character.2.dat", std::ios::binary);
        f << "SAVE_V1_ORIGINAL";
    }
    const fs::path exp_dir = tmp / "exported";
    std::string msg;
    CHECK(ExportUserData(ud, exp_dir, msg));
    CHECK(fs::is_regular_file(exp_dir / "character.2.dat", ec));

    // Modify import source to V2 and import -> must overwrite doc AND create doc_backup_* with V1!
    const fs::path imp_dir = tmp / "incoming" / "files" / "doc";
    fs::create_directories(imp_dir, ec);
    {
        std::ofstream f(imp_dir / "character.2.dat", std::ios::binary);
        f << "SAVE_V2_IMPORTED";
    }
    CHECK(ImportUserData(tmp / "incoming", ud, msg));
    {
        std::ifstream f(ud / "doc" / "character.2.dat", std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(f)), {});
        CHECK(content == "SAVE_V2_IMPORTED");
    }
    bool found_backup = false;
    for (const auto& entry : fs::directory_iterator(ud, ec)) {
        if (entry.is_directory() && entry.path().filename().string().rfind("doc_backup_", 0) == 0) {
            std::ifstream bf(entry.path() / "character.2.dat", std::ios::binary);
            std::string bcontent((std::istreambuf_iterator<char>(bf)), {});
            if (bcontent == "SAVE_V1_ORIGINAL") found_backup = true;
        }
    }
    CHECK(found_backup);

    // 3b. Single-file ExportSaveFile (.dat & .zip) + ImportSaveFiles (.dat & .zip) round-trip
    const fs::path exp_dat = tmp / "character_copy.dat";
    const fs::path exp_zip = tmp / "full_backup.zip";
    CHECK(ExportSaveFile(ud, exp_dat, msg));
    CHECK(fs::is_regular_file(exp_dat, ec));
    CHECK(ExportSaveFile(ud, exp_zip, msg));
    CHECK(fs::is_regular_file(exp_zip, ec));

    const fs::path ud2 = tmp / "userdata_restore";
    CHECK(ImportSaveFiles({exp_zip}, ud2, msg));
    {
        std::ifstream f(ud2 / "doc" / "character.2.dat", std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(f)), {});
        CHECK(content == "SAVE_V2_IMPORTED");
    }

    // 4. APK extractor + BCrypt SHA-256 verification against stock read-only APK
    const fs::path stock_apk = R"(d:\Real Racing 3\RR3 14.0.1.apk)";
    if (fs::is_regular_file(stock_apk, ec)) {
        const fs::path ext_dir = tmp / "extracted_libs";
        CHECK(ExtractArm64LibsFromApk(stock_apk, ext_dir, msg));
        CHECK(ComputeFileSha256Hex(ext_dir / "libRealRacing3.so") == kExpectedRr3SoSha256);
    }

    fs::remove_all(tmp, ec);
    std::printf("  [PASS] test_config_quality_verbose_and_dist_tools (Quality Low..Ultra VFS, Verbose INI, Save Import/Export+Backup, APK SHA-256)\n");
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SDL_SetMainReady();
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) == 0);

    TestKeyboardSmoothSteeringRamp();
    TestVirtualGamepadProfilesAndDetection();
    TestDynamicResizeAndSupersamplingFbo();
    TestWindowAutoFitAndModes();
    TestConfigQualityVerboseAndDistTools();

    SDL_Quit();
    std::printf("test_input_resize ALL PASSED\n");
    return 0;
}
