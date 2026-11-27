#include "audio_opensl.h"
#include "bionic_libc.h"
#include "config.h"
#include "configurator.h"
#include "cpu_dynarmic.h"
#include "dist_tools.h"
#include "elf_loader.h"
#include "gles_bridge.h"
#include "input_pc.h"
#include "java_firemint.h"
#include "java_offline_stubs.h"
#include "jni_env.h"
#include "vfs.h"
#include "../assets/rr3_icon.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <windows.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

static uint64_t g_rr3_bias = 0;

void OnInvalidCrtParam(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned int line, uintptr_t) {
    std::fprintf(stderr, "[CRT INVALID PARAM] last_svc='%s' LR=0x%llx (rr3+0x%llx) func=%ls expr=%ls (%ls:%u)\n",
                 CpuRuntime::LastSvcName(),
                 static_cast<unsigned long long>(CpuRuntime::LastSvcLR()),
                 static_cast<unsigned long long>(CpuRuntime::LastSvcLR() - g_rr3_bias),
                 func ? func : L"(null)", expr ? expr : L"(null)",
                 file ? file : L"(null)", line);
    std::fflush(stderr);
}

void OnTerminate() {
    std::fprintf(stderr, "[TERMINATE] last_svc='%s' LR=0x%llx (rr3+0x%llx)",
                 CpuRuntime::LastSvcName(),
                 static_cast<unsigned long long>(CpuRuntime::LastSvcLR()),
                 static_cast<unsigned long long>(CpuRuntime::LastSvcLR() - g_rr3_bias));
    try {
        if (auto ep = std::current_exception()) std::rethrow_exception(ep);
    } catch (const std::exception& e) {
        std::fprintf(stderr, " exception='%s'", e.what());
    } catch (...) {
        std::fprintf(stderr, " exception=(unknown)");
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    std::_Exit(99);
}

LONG WINAPI OnVehException(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == STATUS_STACK_BUFFER_OVERRUN || code == EXCEPTION_STACK_OVERFLOW) {
        std::fprintf(stderr, "[WIN32 EX 0x%08lx] rip=%p last_svc='%s' LR=0x%llx (rr3+0x%llx)\n",
                     code, ep->ExceptionRecord->ExceptionAddress,
                     CpuRuntime::LastSvcName(),
                     static_cast<unsigned long long>(CpuRuntime::LastSvcLR()),
                     static_cast<unsigned long long>(CpuRuntime::LastSvcLR() - g_rr3_bias));
        std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void WriteU32BE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

void WritePngChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len) {
    WriteU32BE(out, static_cast<uint32_t>(len));
    size_t type_pos = out.size();
    out.insert(out.end(), type, type + 4);
    if (data && len) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, out.data() + type_pos, static_cast<uInt>(4 + len));
    WriteU32BE(out, static_cast<uint32_t>(crc));
}

bool SaveRgbaPng(const std::filesystem::path& path, const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return false;
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 4));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgba + static_cast<size_t>(y) * w * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 4);
    }
    uLongf comp_len = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> comp(comp_len);
    if (compress2(comp.data(), &comp_len, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED) != Z_OK) {
        return false;
    }
    comp.resize(comp_len);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    WriteU32BE(ihdr, static_cast<uint32_t>(w));
    WriteU32BE(ihdr, static_cast<uint32_t>(h));
    ihdr.push_back(8);
    ihdr.push_back(6);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    WritePngChunk(png, "IHDR", ihdr.data(), ihdr.size());
    WritePngChunk(png, "IDAT", comp.data(), comp.size());
    WritePngChunk(png, "IEND", nullptr, 0);

    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return out.good();
}

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }
    }
    SetProcessDPIAware();
}

SDL_Keycode ParseKeyOrDefault(const std::string& name, SDL_Keycode fallback) {
    if (name.empty()) return fallback;
    SDL_Keycode k = SDL_GetKeyFromName(name.c_str());
    return k != SDLK_UNKNOWN ? k : fallback;
}

KeyBindings BuildKeyBindings(const PcConfig& cfg) {
    KeyBindings b;
    b.throttle = ParseKeyOrDefault(cfg.key_throttle, SDLK_w);
    b.brake = ParseKeyOrDefault(cfg.key_brake, SDLK_s);
    b.steer_left = ParseKeyOrDefault(cfg.key_left, SDLK_a);
    b.steer_right = ParseKeyOrDefault(cfg.key_right, SDLK_d);
    b.handbrake = ParseKeyOrDefault(cfg.key_handbrake, SDLK_SPACE);
    b.camera = ParseKeyOrDefault(cfg.key_camera, SDLK_c);
    b.rear_view = ParseKeyOrDefault(cfg.key_rear_view, SDLK_v);
    b.confirm = ParseKeyOrDefault(cfg.key_confirm, SDLK_RETURN);
    b.back = ParseKeyOrDefault(cfg.key_back, SDLK_ESCAPE);
    b.pause = ParseKeyOrDefault(cfg.key_pause, SDLK_ESCAPE);
    return b;
}

struct TimedDump { double sec = 0.0; std::string path; bool done = false; };
struct TimedTap  { double sec = 0.0; float nx = 0.5f; float ny = 0.5f; int state = 0; };
struct TimedKey  { double start_sec = 0.0; double end_sec = 0.0; SDL_Keycode key = SDLK_UNKNOWN; int state = 0; };

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    _set_invalid_parameter_handler(OnInvalidCrtParam);
    std::set_terminate(OnTerminate);
    AddVectoredExceptionHandler(1, OnVehException);
    EnableDpiAwareness();

    std::vector<std::pair<int, std::string>> dump_frames;
    std::vector<TimedDump> timed_dumps;
    std::vector<TimedTap> timed_taps;
    std::vector<TimedKey> timed_keys;
    int max_frames = -1;
    double max_seconds = -1.0;
    int resize_frame = -1, resize_w = 0, resize_h = 0;
    double resize_sec = -1.0;
    bool resize_sec_done = false;
    std::string renderer_override = "Adreno (TM) 740";
    std::filesystem::path config_path = ResolveDefaultConfigPath();
    bool cli_no_vsync = false;
    bool cli_profile_slow = false;
    bool cli_has_verbose = false;
    bool cli_verbose = false;
    bool cli_has_quality = false;
    QualityPreset cli_quality = QualityPreset::Ultra;
    bool cli_config_ui = false;
    bool cli_play = false;
    int cli_tab = 0;
    std::filesystem::path cli_screenshot;
    float cli_render_scale = 0.0f;
    bool cli_has_mode = false;
    WindowMode cli_mode = WindowMode::Windowed;
    int cli_width = -1, cli_height = -1;
    double switch_mode_sec = 0.0;
    WindowMode switch_mode_target = WindowMode::Fullscreen;
    bool switch_mode_done = false;
    bool cli_dump_backbuffer = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dump-frame" && i + 2 < argc) {
            int f = std::atoi(argv[++i]);
            std::string p = argv[++i];
            dump_frames.emplace_back(f, std::move(p));
        } else if (arg == "--dump-at-sec" && i + 2 < argc) {
            TimedDump td;
            td.sec = std::atof(argv[++i]);
            td.path = argv[++i];
            timed_dumps.push_back(std::move(td));
        } else if (arg == "--tap-at-sec" && i + 3 < argc) {
            TimedTap tt;
            tt.sec = std::atof(argv[++i]);
            tt.nx = static_cast<float>(std::atof(argv[++i]));
            tt.ny = static_cast<float>(std::atof(argv[++i]));
            timed_taps.push_back(tt);
        } else if (arg == "--key-at-sec" && i + 3 < argc) {
            TimedKey tk;
            tk.start_sec = std::atof(argv[++i]);
            tk.key = SDL_GetKeyFromName(argv[++i]);
            tk.end_sec = tk.start_sec + std::atof(argv[++i]);
            timed_keys.push_back(tk);
        } else if (arg == "--max-frames" && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
        } else if (arg == "--max-seconds" && i + 1 < argc) {
            max_seconds = std::atof(argv[++i]);
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--config-ui") {
            cli_config_ui = true;
        } else if (arg == "--play") {
            cli_play = true;
        } else if (arg == "--tab" && i + 1 < argc) {
            cli_tab = std::atoi(argv[++i]);
        } else if (arg == "--screenshot" && i + 1 < argc) {
            cli_screenshot = argv[++i];
        } else if (arg == "--verbose") {
            cli_has_verbose = true;
            cli_verbose = true;
        } else if (arg == "--quiet") {
            cli_has_verbose = true;
            cli_verbose = false;
        } else if (arg == "--quality" && i + 1 < argc) {
            cli_has_quality = true;
            cli_quality = ParseQualityPreset(argv[++i], QualityPreset::Ultra);
        } else if (arg == "--renderer" && i + 1 < argc) {
            renderer_override = argv[++i];
        } else if (arg == "--no-vsync") {
            cli_no_vsync = true;
        } else if (arg == "--profile-slow") {
            cli_profile_slow = true;
        } else if (arg == "--windowed") {
            cli_has_mode = true;
            cli_mode = WindowMode::Windowed;
        } else if (arg == "--borderless") {
            cli_has_mode = true;
            cli_mode = WindowMode::Borderless;
        } else if (arg == "--fullscreen") {
            cli_has_mode = true;
            cli_mode = WindowMode::Fullscreen;
        } else if (arg == "--window-mode" && i + 1 < argc) {
            cli_has_mode = true;
            cli_mode = ParseWindowMode(argv[++i], WindowMode::Windowed);
        } else if (arg == "--width" && i + 1 < argc) {
            cli_width = std::atoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            cli_height = std::atoi(argv[++i]);
        } else if (arg == "--render-scale" && i + 1 < argc) {
            cli_render_scale = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--resize-at" && i + 3 < argc) {
            resize_frame = std::atoi(argv[++i]);
            resize_w = std::atoi(argv[++i]);
            resize_h = std::atoi(argv[++i]);
        } else if (arg == "--resize-at-sec" && i + 3 < argc) {
            resize_sec = std::atof(argv[++i]);
            resize_w = std::atoi(argv[++i]);
            resize_h = std::atoi(argv[++i]);
        } else if (arg == "--switch-mode-at-sec" && i + 2 < argc) {
            switch_mode_sec = std::atof(argv[++i]);
            switch_mode_target = ParseWindowMode(argv[++i], WindowMode::Fullscreen);
        } else if (arg == "--dump-backbuffer") {
            cli_dump_backbuffer = true;
        }
    }

    PcConfig cfg = LoadConfig(config_path);
    SaveDefaultConfigIfMissing(config_path, cfg);

    // Interactive double-click launches the built-in Launcher & Configurator UI;
    // passing --play or any direct game/test CLI flag jumps straight into the game.
    const bool direct_game_launch =
        cli_play || cli_has_mode || cli_no_vsync || cli_has_verbose || cli_has_quality ||
        cli_render_scale > 0.0f || cli_width >= 0 || cli_height >= 0 ||
        max_frames >= 0 || max_seconds >= 0.0 || !dump_frames.empty() || !timed_dumps.empty();
    const bool open_launcher = cli_config_ui || !cli_screenshot.empty() || !direct_game_launch;

    if (open_launcher) {
        LauncherOptions opts;
        opts.config_path = config_path;
        opts.initial_tab = cli_tab;
        opts.screenshot_out = cli_screenshot;
        if (!RunLauncherUi(opts, cfg)) {
            return 0;
        }
    } else {
        GameDataStatus data_st = CheckGameDataStatus(cfg);
        if (!data_st.has_libs || !data_st.has_depot) {
            std::fprintf(stderr, "[RR3] %s\n", data_st.summary.c_str());
            if (max_frames < 0 && max_seconds < 0.0) {
                LauncherOptions opts;
                opts.config_path = config_path;
                opts.initial_tab = 3; // Files & saves
                if (!RunLauncherUi(opts, cfg)) {
                    return 0;
                }
            } else {
                return 1;
            }
        }
    }

    if (cli_no_vsync) cfg.vsync = false;
    if (cli_has_verbose) cfg.verbose_logs = cli_verbose;
    if (cli_has_quality) cfg.quality_preset = cli_quality;
    if (cli_render_scale > 0.0f) cfg.render_scale = std::clamp(cli_render_scale, 0.5f, 2.0f);
    if (cli_has_mode) cfg.window_mode = cli_mode;
    if (cli_width >= 0) cfg.width = cli_width;
    if (cli_height >= 0) cfg.height = cli_height;
    cfg.high_quality_preset = (cfg.quality_preset == QualityPreset::Ultra);
    cfg.fullscreen = (cfg.window_mode == WindowMode::Fullscreen);
    cfg.borderless = (cfg.window_mode == WindowMode::Borderless);

    SetVerboseLogging(cfg.verbose_logs);

    // If launched with Verbose Logs enabled (no redirected stdout pipe/file),
    // allocate a live console window so the user can watch engine/JNI/GLES logs in real time.
    if (cfg.verbose_logs) {
        HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD ftype = (h_out && h_out != INVALID_HANDLE_VALUE) ? GetFileType(h_out) : FILE_TYPE_UNKNOWN;
        if (ftype == FILE_TYPE_UNKNOWN && !GetConsoleWindow()) {
            if (AllocConsole()) {
                SetConsoleTitleW(L"Real Racing 3 PC — Verbose Logs");
                FILE* fp = nullptr;
                freopen_s(&fp, "CONOUT$", "w", stdout);
                freopen_s(&fp, "CONOUT$", "w", stderr);
                setvbuf(stdout, nullptr, _IONBF, 0);
                setvbuf(stderr, nullptr, _IONBF, 0);
            }
        }
    }

    SDL_SetMainReady();
    // Enable Steam/HIDAPI drivers for DualShock 4, DualSense, Switch Pro, and XInput controllers!
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "[RR3] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Create initially hidden so we can measure real OS window borders, clamp inside monitor
    // usable bounds (never overflowing a 1600x900 or smaller display), and apply the exact mode!
    SDL_Window* win = SDL_CreateWindow("Real Racing 3 PC",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1280, 720,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    if (!win) {
        std::fprintf(stderr, "[RR3] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    WindowMode active_mode = cfg.window_mode;
    int saved_win_w = cfg.width;
    int saved_win_h = cfg.height;

    auto apply_window_mode = [&](WindowMode target_mode) {
        int disp_idx = SDL_GetWindowDisplayIndex(win);
        if (disp_idx < 0) disp_idx = 0;

        SDL_Rect full_bounds{0, 0, 1280, 720};
        SDL_Rect usable_bounds{0, 0, 1280, 720};
        SDL_GetDisplayBounds(disp_idx, &full_bounds);
        if (SDL_GetDisplayUsableBounds(disp_idx, &usable_bounds) != 0) {
            usable_bounds = full_bounds;
        }

        // Measure Win32 frame/titlebar borders while in bordered windowed state
        int bt = 31, bl = 8, bb = 8, br = 8;
        if (target_mode == WindowMode::Windowed) {
            SDL_SetWindowFullscreen(win, 0);
            SDL_SetWindowBordered(win, SDL_TRUE);
            SDL_SetWindowResizable(win, SDL_TRUE);
            int q_bt = 0, q_bl = 0, q_bb = 0, q_br = 0;
            if (SDL_GetWindowBordersSize(win, &q_bt, &q_bl, &q_bb, &q_br) == 0 &&
                (q_bt > 0 || q_bl > 0 || q_bb > 0 || q_br > 0)) {
                bt = q_bt; bl = q_bl; bb = q_bb; br = q_br;
            }
        }

        WindowGeometry geom = ComputeSafeWindowGeometry(
            target_mode, saved_win_w, saved_win_h,
            full_bounds.x, full_bounds.y, full_bounds.w, full_bounds.h,
            usable_bounds.x, usable_bounds.y, usable_bounds.w, usable_bounds.h,
            bt, bl, bb, br);

        const Uint32 flags = SDL_GetWindowFlags(win);
        if (target_mode == WindowMode::Windowed) {
            if (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) {
                SDL_RestoreWindow(win);
            }
            SDL_SetWindowSize(win, geom.w, geom.h);
            SDL_SetWindowPosition(win, geom.x, geom.y);
            if (flags & SDL_WINDOW_HIDDEN) {
                SDL_ShowWindow(win);
            }
        } else if (target_mode == WindowMode::Borderless) {
            SDL_SetWindowFullscreen(win, 0);
            if (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) {
                SDL_RestoreWindow(win);
            }
            SDL_SetWindowBordered(win, SDL_FALSE);
            SDL_SetWindowResizable(win, SDL_FALSE);
            SDL_SetWindowSize(win, geom.w, geom.h);
            SDL_SetWindowPosition(win, geom.x, geom.y);
            if (flags & SDL_WINDOW_HIDDEN) {
                SDL_ShowWindow(win);
            }
        } else { // WindowMode::Fullscreen
            SDL_SetWindowBordered(win, SDL_FALSE);
            SDL_SetWindowSize(win, geom.w, geom.h);
            SDL_SetWindowPosition(win, geom.x, geom.y);
            if (flags & SDL_WINDOW_HIDDEN) {
                SDL_ShowWindow(win);
            }
            SDL_RaiseWindow(win);
            SDL_DisplayMode desktop_mode{};
            if (SDL_GetDesktopDisplayMode(disp_idx, &desktop_mode) == 0) {
                SDL_SetWindowDisplayMode(win, &desktop_mode);
            }
            if (SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN) != 0) {
                SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }

        SDL_PumpEvents();
        active_mode = target_mode;
        int actual_w = geom.w, actual_h = geom.h;
        SDL_GetWindowSizeInPixels(win, &actual_w, &actual_h);
        std::fprintf(stdout,
                     "[RR3] Display #%d: full=%dx%d usable=%dx%d -> mode=%s client=%dx%d at (%d,%d)\n",
                     disp_idx, full_bounds.w, full_bounds.h, usable_bounds.w, usable_bounds.h,
                     WindowModeToString(active_mode), actual_w, actual_h, geom.x, geom.y);
    };

    apply_window_mode(active_mode);
    SDL_RaiseWindow(win);

    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(win, &wm) || !wm.info.win.window) {
        std::fprintf(stderr, "[RR3] SDL_GetWindowWMInfo failed\n");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    // Apply embedded multi-resolution Real Racing 3 app icon to window titlebar, taskbar, and Alt+Tab.
    {
        HINSTANCE hinst = GetModuleHandleW(nullptr);
        if (HICON h_big = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APP_ICON),
                                                        IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR))) {
            SendMessageW(wm.info.win.window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(h_big));
        }
        if (HICON h_small = static_cast<HICON>(LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APP_ICON),
                                                          IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR))) {
            SendMessageW(wm.info.win.window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(h_small));
        }
    }

    int cur_w = 1280, cur_h = 720;
    SDL_GetWindowSizeInPixels(win, &cur_w, &cur_h);
    if (cur_w <= cur_h) cur_w = cur_h + 16;

    // Keep RR3's UI/render height fixed at the monitor's native height (scaled by render_scale)
    // while dynamically adjusting render width to match the window's exact aspect ratio:
    // eliminates black bars on any window shape/maximize/fullscreen without scaling UI size!
    int init_disp = SDL_GetWindowDisplayIndex(win);
    if (init_disp < 0) init_disp = 0;
    SDL_Rect native_disp{0, 0, cur_w, cur_h};
    if (SDL_GetDisplayBounds(init_disp, &native_disp) != 0 || native_disp.w <= 0 || native_disp.h <= 0) {
        native_disp = {0, 0, cur_w, cur_h};
    }
    int fixed_render_h = std::max(360, native_disp.h);

    // ---- Core subsystems ----
    LayeredVfs vfs;
    vfs.Initialize(cfg.readonly_files_dir, cfg.userdata_dir);
    vfs.SetQualityPreset(cfg.quality_preset);
    if (!cfg.apk_path.empty()) {
        vfs.SetApkPath(cfg.apk_path);
    }

    CpuRuntime cpu;
    ElfLoader loader(cpu);
    BionicRuntime bionic(cpu, loader, vfs);
    bionic.RegisterAll();
    bionic.RegisterSdkStubs();

    OpenSlBridge audio(cpu);
    audio.Init();
    bionic.SetCustomDlResolver([&audio](const std::string& lib, const std::string& sym) {
        return audio.ResolveSymbol(lib, sym);
    });

    GlesBridge gles(cpu);
    if (!gles.Init(wm.info.win.window, cur_w, cur_h, cfg, fixed_render_h)) {
        std::fprintf(stderr, "[RR3] GlesBridge::Init failed\n");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    if (!renderer_override.empty()) {
        gles.SetRendererOverride(renderer_override);
    }
    gles.RegisterSymbols(loader);

    FakeJvm jvm(cpu);
    FiremintRuntime firemint(cpu, loader, gles, jvm, vfs, cfg);
    firemint.RegisterClasses();
    RegisterOfflineStubs(loader, jvm, firemint, cfg);

    PcInput input;
    input.Init(cfg);
    input.SetBindings(BuildKeyBindings(cfg));

    const std::filesystem::path bin_dir(cfg.work_bins_dir);
    LoadedModule* mod_cpp = loader.LoadModule(bin_dir / "libc++_shared.so");
    LoadedModule* mod_fmod = loader.LoadModule(bin_dir / "libfmodex.so");
    LoadedModule* mod_rr3 = loader.LoadModule(bin_dir / "libRealRacing3.so");
    if (!mod_cpp || !mod_fmod || !mod_rr3) {
        std::fprintf(stderr, "[RR3] Failed to load guest ARM64 libraries from %s\n", cfg.work_bins_dir.c_str());
        return 1;
    }
    g_rr3_bias = mod_rr3->load_bias;

    const bool known_rr3_14_0_1 =
        (mod_rr3->sha256_hex == "57edb6314fb1f73916446dccf2aab2223a932ae63ccdf2149cffeddb5aa73051");
    if (cfg.unlock_menu_fps && known_rr3_14_0_1) {
        bionic.SetSkipMenuUsleep(true);
        bionic.SetMenuUsleepRange(mod_rr3->load_bias + 0x012af0d0ULL, mod_rr3->load_bias + 0x012af130ULL);
    }
    if (known_rr3_14_0_1) {
        // Hide the red gamepad pointing glove (party_play/pp_red_cursor.png) when only the
        // virtual PC Keyboard Controller is connected (count==1). Once a real gamepad plugs in
        // (count>=2), the glove appears automatically; unplugging hides it again.
        // 0xdf981c: cmp w0, #1 (0x7100041f) -> cmp w0, #2 (0x7100081f)
        auto* cursor_cmp = reinterpret_cast<uint32_t*>(mod_rr3->load_bias + 0xdf981cULL);
        if (*cursor_cmp == 0x7100041fu) {
            *cursor_cmp = 0x7100081fu;
        }
        // Enable RR3's built-in Display::SetScreen (0x1476f78) + GuiLayout::SetDimensions (0x11c07a4)
        // on live window aspect-ratio changes so fullscreen buffers & HUD edges resize cleanly with
        // zero black bars! (0x12ae40c: cbnz x8, #0x12ae700 (0xb50017a8) -> nop (0xd503201f))
        auto* resize_skip = reinterpret_cast<uint32_t*>(mod_rr3->load_bias + 0x12ae40cULL);
        if (*resize_skip == 0xb50017a8u) {
            *resize_skip = 0xd503201fu;
        }
        // Native RR3 quality tier selection (CGlobalAndroid::ApplyPlatformSettings(this, tier)):
        // 0=AndroidLow, 1=AndroidMid, 2=AndroidMid2, 3=AndroidHigh, 4=AndroidXtraHigh.
        // Patching callers 0x12ad664 and 0x12adbd4 (mov w1, w0 -> mov w1, #<tier>) loads the real
        // .plist and .feats natively without triggering RR3's internal settings ID mismatch error.
        uint32_t tier = 4u;
        switch (cfg.quality_preset) {
            case QualityPreset::Low:    tier = 0u; break;
            case QualityPreset::Medium: tier = 1u; break;
            case QualityPreset::High:   tier = 3u; break;
            case QualityPreset::Ultra:  tier = 4u; break;
        }
        const uint32_t mov_w1_tier = 0x52800001u | (tier << 5);
        for (uint64_t patch_off : {0x12ad664ULL, 0x12adbd4ULL}) {
            auto* insn = reinterpret_cast<uint32_t*>(mod_rr3->load_bias + patch_off);
            if (*insn == 0x2a0003e1u) {
                *insn = mov_w1_tier;
            }
        }
    }

    std::vector<std::string> unresolved;
    loader.ApplyRelocations(&unresolved);
    std::fprintf(stdout, "[RR3] Loaded libRealRacing3.so (bias=0x%llx, sha256=%.16s..., unresolved=%zu, quality=%s, verbose=%s)\n",
                 static_cast<unsigned long long>(mod_rr3->load_bias),
                 mod_rr3->sha256_hex.c_str(), unresolved.size(),
                 QualityPresetToDisplayName(cfg.quality_preset),
                 cfg.verbose_logs ? "ON" : "OFF");

    loader.RunInitArrays();

    auto req_export = [&](const char* name) -> uint64_t {
        uint64_t addr = loader.FindExport(name);
        if (!addr) std::fprintf(stderr, "[RR3] Missing required export: %s\n", name);
        return addr;
    };

    const uint64_t fn_set_tv = req_export("Java_com_firemint_realracing_MainActivity_SetRunningOnAndroidTv");
    const uint64_t fn_on_create = req_export("Java_com_firemint_realracing_MainActivity_onCreateJNI");
    const uint64_t fn_on_start = req_export("Java_com_firemint_realracing_MainActivity_onStartJNI");
    const uint64_t fn_on_resume = req_export("Java_com_firemint_realracing_MainActivity_onResumeJNI");
    const uint64_t fn_on_focus = req_export("Java_com_firemint_realracing_MainActivity_onWindowFocusChangedJNI");
    const uint64_t fn_view_created = req_export("Java_com_firemint_realracing_MainActivity_onViewCreatedJNI");
    const uint64_t fn_view_changed = req_export("Java_com_firemint_realracing_MainActivity_onViewChangedJNI");
    const uint64_t fn_view_render = req_export("Java_com_firemint_realracing_MainActivity_onViewRenderJNI");
    const uint64_t fn_touch_begin = req_export("Java_com_firemint_realracing_MainActivity_onTouchBeginJNI");
    const uint64_t fn_touch_move = req_export("Java_com_firemint_realracing_MainActivity_onTouchMoveJNI");
    const uint64_t fn_touch_end = req_export("Java_com_firemint_realracing_MainActivity_onTouchEndJNI");

    if (!fn_on_create || !fn_view_created || !fn_view_changed || !fn_view_render) {
        return 1;
    }

    const uint64_t env_ptr = jvm.GetJniEnvGuestPtr();
    const uint64_t act = firemint.MainActivityHandle();

    std::fprintf(stdout, "[RR3] Booting MainActivity lifecycle (%dx%d, render=%dx%d, vsync=%s, aniso=%dx)...\n",
                 cur_w, cur_h, gles.RenderWidth(), gles.RenderHeight(),
                 cfg.vsync ? "ON" : "OFF", cfg.anisotropy);

    // Clamp initial boot view size to <=1920x1080 so AssetDownloadService matches installed local
    // .depot asset packs (prevents 535MB download prompt on 1440p/4K monitors or render_scale > 1.0).
    // Once asset validation completes (frame 12), promote to full render_w x render_h!
    int boot_w = gles.RenderWidth();
    int boot_h = gles.RenderHeight();
    bool need_post_boot_promote = false;
    if (boot_w > 1920 || boot_h > 1080) {
        double s = std::min(1920.0 / boot_w, 1080.0 / boot_h);
        boot_w = std::max(640, static_cast<int>(boot_w * s + 0.5) & ~1);
        boot_h = std::max(360, static_cast<int>(boot_h * s + 0.5) & ~1);
        if (boot_w <= boot_h) boot_w = boot_h + 16;
        need_post_boot_promote = true;
    }

    if (fn_set_tv) cpu.CallGuest(fn_set_tv, {env_ptr, act, cfg.android_tv_mode ? 1ULL : 0ULL});
    cpu.CallGuest(fn_on_create, {env_ptr, act});
    if (fn_on_start) cpu.CallGuest(fn_on_start, {env_ptr, act});
    if (fn_on_resume) cpu.CallGuest(fn_on_resume, {env_ptr, act});
    if (fn_on_focus) cpu.CallGuest(fn_on_focus, {env_ptr, act, 1ULL});
    cpu.CallGuest(fn_view_created, {env_ptr, act});
    cpu.CallGuest(fn_view_changed, {
        env_ptr, act,
        static_cast<uint64_t>(boot_w),
        static_cast<uint64_t>(boot_h),
        2ULL, 0ULL
    });

    std::fprintf(stdout, "[RR3] Entering render loop...\n");
    bool running = true;
    bool mouse_down = false;
    int frame = 0;
    const auto run_start = std::chrono::steady_clock::now();
    auto last_frame_t = run_start;
    auto fps_t0 = run_start;
    int fps_frames = 0;
    std::vector<double> steady_frame_ms; // frame durations after initial load (frame > 60)

    bool pending_post_resize_sync = false;
    uint64_t pre_resize_scr = 0;
    uint8_t pre_resize_hdr = 0;
    int pending_resize_frames = 0;

    auto do_resize = [&](int target_w, int target_h, const char* tag) {
        if (target_w <= 0 || target_h <= 0) return;
        cur_w = target_w;
        cur_h = target_h;
        bool render_changed = gles.Resize(cur_w, cur_h);
        std::fprintf(stdout, "[RR3] %s -> window=%dx%d (render=%dx%d, aspectChanged=%s)\n",
                     tag, cur_w, cur_h, gles.RenderWidth(), gles.RenderHeight(),
                     render_changed ? "YES" : "NO");
        if (render_changed) {
            if (known_rr3_14_0_1) {
                auto* pp_scr = *reinterpret_cast<uint64_t**>(mod_rr3->load_bias + 0x1f6df40ULL);
                pre_resize_scr = pp_scr ? *pp_scr : 0;
                pre_resize_hdr = pre_resize_scr ? *reinterpret_cast<uint8_t*>(pre_resize_scr + 0x78ULL) : 0;
                pending_post_resize_sync = true;
                pending_resize_frames = 0;
            }
            cpu.CallGuest(fn_view_changed, {
                env_ptr, act,
                static_cast<uint64_t>(gles.RenderWidth()),
                static_cast<uint64_t>(gles.RenderHeight()),
                2ULL, 0ULL
            });
        }
    };

    auto map_mouse_to_render = [&](int mx, int my, float& out_x, float& out_y) {
        int lw = cur_w, lh = cur_h;
        SDL_GetWindowSize(win, &lw, &lh);
        int pw = gles.WindowWidth(), ph = gles.WindowHeight();
        float px = static_cast<float>(mx) * pw / std::max(1, lw);
        float py = static_cast<float>(my) * ph / std::max(1, lh);
        int vx = 0, vy = 0, vw = pw, vh = ph;
        gles.GetPresentRect(vx, vy, vw, vh);
        // Convert top-down pixel coords against present rect (note vy is GL bottom-up; symmetrical for centered rect)
        float nx = std::clamp((px - static_cast<float>(vx)) / std::max(1, vw), 0.0f, 1.0f);
        float ny = std::clamp((py - static_cast<float>(vy)) / std::max(1, vh), 0.0f, 1.0f);
        out_x = nx * gles.RenderWidth();
        out_y = ny * gles.RenderHeight();
    };

    while (running && !firemint.ShouldExit()) {
        auto frame_start = std::chrono::steady_clock::now();
        double dt_sec = std::chrono::duration<double>(frame_start - last_frame_t).count();
        double elapsed_total = std::chrono::duration<double>(frame_start - run_start).count();
        last_frame_t = frame_start;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_WINDOWEVENT &&
                       (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                        e.window.event == SDL_WINDOWEVENT_RESIZED)) {
                int nw = 0, nh = 0;
                SDL_GetWindowSizeInPixels(win, &nw, &nh);
                if (nw > 0 && nh > 0 && (nw != cur_w || nh != cur_h) && !need_post_boot_promote) {
                    do_resize(nw, nh, "Window resized");
                }
            } else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                mouse_down = false;
                input.ResetAllInputs();
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && fn_touch_begin) {
                mouse_down = true;
                float sx = 0.0f, sy = 0.0f;
                map_mouse_to_render(e.button.x, e.button.y, sx, sy);
                cpu.CallGuestWithF32(fn_touch_begin, {env_ptr, act, 0ULL}, {sx, sy});
            } else if (e.type == SDL_MOUSEMOTION && mouse_down && fn_touch_move) {
                float sx = 0.0f, sy = 0.0f;
                map_mouse_to_render(e.motion.x, e.motion.y, sx, sy);
                cpu.CallGuestWithF32(fn_touch_move, {env_ptr, act, 0ULL}, {sx, sy});
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && mouse_down && fn_touch_end) {
                mouse_down = false;
                float sx = 0.0f, sy = 0.0f;
                map_mouse_to_render(e.button.x, e.button.y, sx, sy);
                cpu.CallGuestWithF32(fn_touch_end, {env_ptr, act, 0ULL, 1ULL}, {sx, sy});
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat &&
                       (e.key.keysym.sym == SDLK_F11 ||
                        (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))) {
                // F11 / Alt+Enter: toggle Fullscreen <-> Windowed
                if (active_mode == WindowMode::Windowed) {
                    SDL_GetWindowSize(win, &saved_win_w, &saved_win_h);
                    apply_window_mode(WindowMode::Fullscreen);
                } else {
                    apply_window_mode(WindowMode::Windowed);
                }
                int nw = 0, nh = 0;
                SDL_GetWindowSizeInPixels(win, &nw, &nh);
                if (nw > 0 && nh > 0 && (nw != cur_w || nh != cur_h)) {
                    do_resize(nw, nh, "Mode toggled");
                }
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F10) {
                // F10: cycle Windowed -> Borderless -> Fullscreen -> Windowed
                if (active_mode == WindowMode::Windowed) {
                    SDL_GetWindowSize(win, &saved_win_w, &saved_win_h);
                    apply_window_mode(WindowMode::Borderless);
                } else if (active_mode == WindowMode::Borderless) {
                    apply_window_mode(WindowMode::Fullscreen);
                } else {
                    apply_window_mode(WindowMode::Windowed);
                }
                int nw = 0, nh = 0;
                SDL_GetWindowSizeInPixels(win, &nw, &nh);
                if (nw > 0 && nh > 0 && (nw != cur_w || nh != cur_h)) {
                    do_resize(nw, nh, "Mode cycled");
                }
            } else {
                input.ProcessEvent(e);
            }
        }

        // Scripted test taps / keys / resizes (for automated verification)
        for (auto& tt : timed_taps) {
            float tx = tt.nx * gles.RenderWidth();
            float ty = tt.ny * gles.RenderHeight();
            if (tt.state == 0 && elapsed_total >= tt.sec && fn_touch_begin) {
                std::fprintf(stdout, "[RR3] Auto-tap DOWN at %.1fs (%.0f, %.0f)\n", elapsed_total, tx, ty);
                cpu.CallGuestWithF32(fn_touch_begin, {env_ptr, act, 0ULL}, {tx, ty});
                tt.state = 1;
            } else if (tt.state == 1 && elapsed_total >= tt.sec + 0.08 && fn_touch_end) {
                cpu.CallGuestWithF32(fn_touch_end, {env_ptr, act, 0ULL, 1ULL}, {tx, ty});
                tt.state = 2;
            }
        }
        for (auto& tk : timed_keys) {
            if (tk.state == 0 && elapsed_total >= tk.start_sec) {
                input.SetKeyState(tk.key, true);
                tk.state = 1;
            } else if (tk.state == 1 && elapsed_total >= tk.end_sec) {
                input.SetKeyState(tk.key, false);
                tk.state = 2;
            }
        }
        if (switch_mode_sec > 0.0 && !switch_mode_done && elapsed_total >= switch_mode_sec) {
            switch_mode_done = true;
            if (active_mode == WindowMode::Windowed) {
                SDL_GetWindowSize(win, &saved_win_w, &saved_win_h);
            }
            apply_window_mode(switch_mode_target);
            int nw = 0, nh = 0;
            SDL_GetWindowSizeInPixels(win, &nw, &nh);
            if (nw > 0 && nh > 0) {
                do_resize(nw, nh, "Automated mode switch");
            }
        }
        if ((resize_frame > 0 && frame == resize_frame) ||
            (resize_sec > 0.0 && !resize_sec_done && elapsed_total >= resize_sec)) {
            resize_sec_done = true;
            if (resize_w > 0 && resize_h > 0) {
                SDL_SetWindowSize(win, resize_w, resize_h);
                int nw = 0, nh = 0;
                SDL_GetWindowSizeInPixels(win, &nw, &nh);
                do_resize(nw > 0 ? nw : resize_w, nh > 0 ? nh : resize_h, "Automated resize");
            }
        }

        // RR3 v14.0.1 (SHA-256 verified): EA servers enabled "enable_optimized_logic" (0x1f894f8)
        // to cache expired LTS event lookups; offline mode leaves it 0, causing 6.8M/frame string scans.
        static const bool kKnownRr3_14_0_1 =
            (mod_rr3->sha256_hex == "57edb6314fb1f73916446dccf2aab2223a932ae63ccdf2149cffeddb5aa73051");
        if (kKnownRr3_14_0_1) {
            auto* opt_flag = reinterpret_cast<uint8_t*>(mod_rr3->load_bias + 0x1f894f8ULL);
            if (*opt_flag == 0) *opt_flag = 1;
        }

        // Promote supersampled / >1080p render resolution after AssetDownloadService finishes (frame 12)
        if (need_post_boot_promote && frame == 12) {
            need_post_boot_promote = false;
            std::fprintf(stdout, "[RR3] Post-boot promote -> render=%dx%d\n", gles.RenderWidth(), gles.RenderHeight());
            cpu.CallGuest(fn_view_changed, {
                env_ptr, act,
                static_cast<uint64_t>(gles.RenderWidth()),
                static_cast<uint64_t>(gles.RenderHeight()),
                2ULL, 0ULL
            });
        }

        // Update keyboard steering ramp and dispatch all keyboard/gamepad inputs to guest
        input.UpdateFrame(static_cast<float>(dt_sec));
        if (frame >= 5) {
            input.DispatchToGuest(cpu, loader, jvm, act);
        }

        firemint.PumpGlCallbacks();

        if (cli_profile_slow) {
            cpu.SetSvcProfileEnabled(true);
            cpu.ResetSvcProfile();
        }
        gles.ResetFrameCounters();
        cpu.CallGuest(fn_view_render, {env_ptr, act, 2ULL, 0ULL});
        if (pending_post_resize_sync && known_rr3_14_0_1) {
            ++pending_resize_frames;
            auto* pp_scr = *reinterpret_cast<uint64_t**>(mod_rr3->load_bias + 0x1f6df40ULL);
            uint64_t cur_scr = pp_scr ? *pp_scr : 0;
            // onViewChangedJNI executes Display::SetScreen asynchronously during fn_view_render.
            // Wait until Display::SetScreen has replaced mtScreenGL with the new render width,
            // then sync graphics viewport, FBO cache, HDR flag, and drop any stale pause-blur RT.
            int scr_w = cur_scr ? *reinterpret_cast<int32_t*>(cur_scr + 8ULL) : 0;
            if ((cur_scr && scr_w == gles.RenderWidth()) || pending_resize_frames >= 600) {
                pending_post_resize_sync = false;
                if (cur_scr) {
                    if (pre_resize_hdr) {
                        *reinterpret_cast<uint8_t*>(cur_scr + 0x78ULL) = pre_resize_hdr;
                    }
                    // Free any pause-blur RT captured during the transitional resize frame
                    // so MainMenuManager::Render re-captures cleanly at the new viewport on the next frame.
                    uint64_t scr_vt = *reinterpret_cast<uint64_t*>(cur_scr);
                    uint64_t fn_clear_blur = scr_vt ? *reinterpret_cast<uint64_t*>(scr_vt + 0xa0ULL) : 0;
                    uint64_t blur_rt = *reinterpret_cast<uint64_t*>(cur_scr + 0xd0ULL);
                    if (blur_rt && fn_clear_blur) {
                        cpu.CallGuest(fn_clear_blur, {cur_scr});
                    }
                }
                auto* pp_gfx = *reinterpret_cast<uint64_t**>(mod_rr3->load_bias + 0x1f6df50ULL);
                uint64_t g_gfx = pp_gfx ? *pp_gfx : 0;
                if (g_gfx) {
                    *reinterpret_cast<uint64_t*>(g_gfx + 0x5a0ULL) = 0;
                    *reinterpret_cast<uint64_t*>(g_gfx + 0x5a8ULL) = 0;
                    *reinterpret_cast<uint32_t*>(g_gfx + 0x5b0ULL) = 0;
                    uint64_t vtable = *reinterpret_cast<uint64_t*>(g_gfx);
                    uint64_t fn_set_vp = vtable ? *reinterpret_cast<uint64_t*>(vtable + 0xf8ULL) : 0;
                    if (fn_set_vp) {
                        cpu.CallGuest(fn_set_vp, {
                            g_gfx, 0ULL, 0ULL,
                            static_cast<uint64_t>(gles.RenderWidth()),
                            static_cast<uint64_t>(gles.RenderHeight())
                        });
                    }
                }
            }
        }
        ++frame;
        ++fps_frames;

        auto dump_now = [&](const std::string& p) {
            std::vector<uint8_t> rgba;
            int dw = 0, dh = 0;
            if (gles.ReadPixelsRgba(rgba, dw, dh, cli_dump_backbuffer)) {
                bool ok = SaveRgbaPng(p, rgba.data(), dw, dh);
                std::fprintf(stdout, "[RR3] Dumped frame %d @%.1fs (%dx%d, drawCalls=%llu) -> %s (%s)\n",
                             frame, elapsed_total, dw, dh,
                             static_cast<unsigned long long>(gles.DrawCallCount()),
                             p.c_str(), ok ? "OK" : "FAILED");
            }
        };
        for (const auto& [df, dp] : dump_frames) {
            if (df == frame && !dp.empty()) dump_now(dp);
        }
        for (auto& td : timed_dumps) {
            if (!td.done && elapsed_total >= td.sec) {
                td.done = true;
                dump_now(td.path);
            }
        }

        gles.SwapBuffers();

        auto frame_end = std::chrono::steady_clock::now();
        double frame_ms = std::chrono::duration<double, std::milli>(frame_end - frame_start).count();
        if (cli_profile_slow && frame_ms >= 200.0 && frame >= 20) {
            cpu.DumpTopSvcs(frame, frame_ms, 8);
        }
        if (cfg.fps_cap > 0) {
            double target_ms = 1000.0 / cfg.fps_cap;
            if (frame_ms < target_ms) {
                double rem = target_ms - frame_ms;
                if (rem > 2.0) std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(rem - 1.0)));
                while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start).count() < target_ms) {
                    std::this_thread::yield();
                }
                frame_end = std::chrono::steady_clock::now();
                frame_ms = std::chrono::duration<double, std::milli>(frame_end - frame_start).count();
            }
        }
        if (frame > 60 && frame_ms < 250.0) {
            steady_frame_ms.push_back(frame_ms);
        }

        double elapsed = std::chrono::duration<double>(frame_end - fps_t0).count();
        if (elapsed >= 1.0) {
            double fps = fps_frames / elapsed;
            char title[192];
            std::snprintf(title, sizeof(title),
                          "Real Racing 3 PC [%s] | %dx%d | %.1f FPS | Pads:%zu | F10:Mode F11:FS",
                          WindowModeToString(active_mode),
                          gles.RenderWidth(), gles.RenderHeight(), fps, input.ConnectedGamepadCount());
            SDL_SetWindowTitle(win, title);
            if (IsVerboseLogging()) {
                std::fprintf(stdout, "[RR3] t=%.1fs frame=%d fps=%.1f drawCalls=%llu\n",
                             elapsed_total, frame, fps, static_cast<unsigned long long>(gles.DrawCallCount()));
            }
            fps_t0 = frame_end;
            fps_frames = 0;
        }

        if ((max_frames > 0 && frame >= max_frames) ||
            (max_seconds > 0.0 && elapsed_total >= max_seconds)) {
            std::fprintf(stdout, "[RR3] Reached stop limit (frame=%d, t=%.1fs), exiting cleanly.\n",
                         frame, elapsed_total);
            break;
        }
    }

    if (!steady_frame_ms.empty()) {
        std::vector<double> sorted = steady_frame_ms;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;
        for (double v : sorted) sum += v;
        double avg_ms = sum / sorted.size();
        double p99_ms = sorted[static_cast<size_t>(sorted.size() * 0.99)];
        std::fprintf(stdout, "[RR3] Benchmark (%zu steady frames): avg=%.1f FPS (%.2f ms), 1%% low=%.1f FPS (%.2f ms), best=%.2f ms\n",
                     sorted.size(), 1000.0 / avg_ms, avg_ms, 1000.0 / p99_ms, p99_ms, sorted.front());
    }
    std::fprintf(stdout, "[RR3] Audio summary: OpenSL framesPlayed=%llu enqueues=%llu (%dHz %dch)\n",
                 static_cast<unsigned long long>(audio.FramesPlayed()),
                 static_cast<unsigned long long>(audio.EnqueueCount()),
                 audio.SampleRate(), audio.Channels());

    input.Shutdown();
    audio.Shutdown();
    gles.Shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}
