#include "config.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace {

std::atomic<bool> g_verbose_logging{false};

std::string Trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ParseBool(const std::string& v) {
    std::string low = ToLower(v);
    return low == "1" || low == "true" || low == "yes" || low == "on";
}

bool IsDevBuildDir(const std::filesystem::path& exe_dir) {
    std::error_code ec;
    return std::filesystem::exists(exe_dir / ".." / "CMakeLists.txt", ec);
}

std::filesystem::path ResolveBaseDir() {
    auto exe_dir = GetExecutableDir();
    if (IsDevBuildDir(exe_dir)) {
        return (exe_dir / "..").lexically_normal();
    }
    return exe_dir;
}

} // namespace

void SetVerboseLogging(bool enabled) {
    g_verbose_logging.store(enabled, std::memory_order_relaxed);
}

bool IsVerboseLogging() {
    return g_verbose_logging.load(std::memory_order_relaxed);
}

std::filesystem::path GetExecutableDir() {
    wchar_t buf[32768]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        return std::filesystem::path(buf).parent_path().lexically_normal();
    }
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

std::filesystem::path ResolveDefaultConfigPath() {
    auto exe_dir = GetExecutableDir();
    std::error_code ec;
    auto local_ini = exe_dir / "rr3_pc.ini";
    if (std::filesystem::exists(local_ini, ec)) {
        return local_ini;
    }
    if (IsDevBuildDir(exe_dir)) {
        return (exe_dir / ".." / "rr3_pc.ini").lexically_normal();
    }
    return local_ini;
}

void ApplyPortableDefaultPaths(PcConfig& cfg) {
    const auto base_dir = ResolveBaseDir();
    std::error_code ec;

    if (cfg.userdata_dir.empty()) {
        cfg.userdata_dir = (base_dir / "userdata").lexically_normal().string();
    }
    if (cfg.work_bins_dir.empty()) {
        cfg.work_bins_dir = (base_dir / "work_bins" / "lib" / "arm64-v8a").lexically_normal().string();
    }
    if (cfg.readonly_files_dir.empty()) {
        const std::filesystem::path candidates[] = {
            base_dir / "com.ea.games.r3_row" / "files",
            base_dir / "game_data" / "files",
            base_dir / ".." / "com.ea.games.r3_row" / "files",
        };
        for (const auto& c : candidates) {
            if (std::filesystem::is_directory(c / ".depot", ec)) {
                cfg.readonly_files_dir = c.lexically_normal().string();
                break;
            }
        }
        if (cfg.readonly_files_dir.empty()) {
            cfg.readonly_files_dir = (base_dir / "com.ea.games.r3_row" / "files").lexically_normal().string();
        }
    }
    if (cfg.apk_path.empty()) {
        const std::filesystem::path apk_candidates[] = {
            base_dir / "RR3 14.0.1.apk",
            base_dir / ".." / "RR3 14.0.1.apk",
            std::filesystem::path(cfg.readonly_files_dir).parent_path().parent_path() / "RR3 14.0.1.apk",
        };
        for (const auto& c : apk_candidates) {
            if (std::filesystem::is_regular_file(c, ec)) {
                cfg.apk_path = c.lexically_normal().string();
                break;
            }
        }
        // Fallback: auto-detect any RR3 *.apk (e.g. "RR3 v14.0.1_row.apk") in base/parent/data root.
        if (cfg.apk_path.empty()) {
            const std::filesystem::path search_dirs[] = {
                base_dir,
                base_dir / "..",
                std::filesystem::path(cfg.readonly_files_dir).parent_path().parent_path(),
            };
            std::filesystem::path first_apk;
            std::filesystem::path rr3_apk;
            for (const auto& dir : search_dirs) {
                ec.clear();
                if (!std::filesystem::is_directory(dir, ec)) continue;
                for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (ec) break;
                    if (!entry.is_regular_file(ec)) continue;
                    std::string ext = ToLower(entry.path().extension().string());
                    if (ext != ".apk") continue;
                    std::string name = ToLower(entry.path().filename().string());
                    if (name.find("rr3") != std::string::npos ||
                        name.find("r3_row") != std::string::npos ||
                        name.find("realracing") != std::string::npos) {
                        rr3_apk = entry.path().lexically_normal();
                        break;
                    }
                    if (first_apk.empty()) {
                        first_apk = entry.path().lexically_normal();
                    }
                }
                if (!rr3_apk.empty()) break;
            }
            if (!rr3_apk.empty()) {
                cfg.apk_path = rr3_apk.string();
            } else if (!first_apk.empty()) {
                cfg.apk_path = first_apk.string();
            }
        }
    }
}

WindowMode ParseWindowMode(const std::string& val, WindowMode fallback) {
    std::string low = ToLower(Trim(val));
    if (low == "windowed" || low == "window") return WindowMode::Windowed;
    if (low == "borderless" || low == "borderless_windowed" || low == "borderless-windowed") return WindowMode::Borderless;
    if (low == "fullscreen" || low == "full" || low == "exclusive") return WindowMode::Fullscreen;
    return fallback;
}

const char* WindowModeToString(WindowMode mode) {
    switch (mode) {
        case WindowMode::Windowed:   return "windowed";
        case WindowMode::Borderless: return "borderless";
        case WindowMode::Fullscreen: return "fullscreen";
    }
    return "windowed";
}

QualityPreset ParseQualityPreset(const std::string& val, QualityPreset fallback) {
    std::string low = ToLower(Trim(val));
    if (low == "low" || low == "androidlow") return QualityPreset::Low;
    if (low == "medium" || low == "med" || low == "mid" || low == "androidmid") return QualityPreset::Medium;
    if (low == "high" || low == "androidhigh") return QualityPreset::High;
    if (low == "ultra" || low == "xtrahigh" || low == "extrahigh" || low == "max" || low == "androidxtrahigh") {
        return QualityPreset::Ultra;
    }
    return fallback;
}

const char* QualityPresetToString(QualityPreset preset) {
    switch (preset) {
        case QualityPreset::Low:    return "low";
        case QualityPreset::Medium: return "medium";
        case QualityPreset::High:   return "high";
        case QualityPreset::Ultra:  return "ultra";
    }
    return "ultra";
}

const char* QualityPresetToDisplayName(QualityPreset preset) {
    switch (preset) {
        case QualityPreset::Low:    return "Low";
        case QualityPreset::Medium: return "Medium";
        case QualityPreset::High:   return "High";
        case QualityPreset::Ultra:  return "Ultra";
    }
    return "Ultra";
}

const char* QualityPresetToAndroidStem(QualityPreset preset) {
    switch (preset) {
        case QualityPreset::Low:    return "AndroidLow";
        case QualityPreset::Medium: return "AndroidMid";
        case QualityPreset::High:   return "AndroidHigh";
        case QualityPreset::Ultra:  return "AndroidXtraHigh";
    }
    return "AndroidXtraHigh";
}

WindowGeometry ComputeSafeWindowGeometry(WindowMode mode,
                                         int req_w, int req_h,
                                         int display_x, int display_y, int display_w, int display_h,
                                         int usable_x, int usable_y, int usable_w, int usable_h,
                                         int border_top, int border_left, int border_bottom, int border_right) {
    display_w = std::max(640, display_w);
    display_h = std::max(360, display_h);
    if (usable_w <= 0 || usable_h <= 0) {
        usable_x = display_x;
        usable_y = display_y;
        usable_w = display_w;
        usable_h = display_h;
    }

    if (mode == WindowMode::Borderless || mode == WindowMode::Fullscreen) {
        WindowGeometry g;
        g.x = display_x;
        g.y = display_y;
        g.w = display_w;
        g.h = display_h;
        if (g.w <= g.h) g.w = g.h + 16;
        return g;
    }

    border_top = std::max(0, border_top);
    border_left = std::max(0, border_left);
    border_bottom = std::max(0, border_bottom);
    border_right = std::max(0, border_right);

    int max_w = std::max(640, usable_w - border_left - border_right);
    int max_h = std::max(360, usable_h - border_top - border_bottom);

    int w = req_w;
    int h = req_h;
    if (w <= 0 || h <= 0) {
        // Auto-detect: fit a clean 16:9 landscape window at ~90% of usable screen bounds.
        double avail_w = std::max(640.0, max_w * 0.90);
        double avail_h = std::max(360.0, max_h * 0.90);
        double aspect = 16.0 / 9.0;
        if (avail_w / avail_h > aspect) {
            h = static_cast<int>(std::floor(avail_h));
            w = static_cast<int>(std::floor(h * aspect));
        } else {
            w = static_cast<int>(std::floor(avail_w));
            h = static_cast<int>(std::floor(w / aspect));
        }
    } else if (w > max_w || h > max_h) {
        // Clamp oversized requested dimensions proportionally so nothing extends off-screen.
        double s = std::min(static_cast<double>(max_w) / w, static_cast<double>(max_h) / h);
        w = static_cast<int>(std::floor(w * s));
        h = static_cast<int>(std::floor(h * s));
    }

    // Even pixels, within [640..max_w] x [360..max_h], and always landscape (RR3 requirement).
    w = std::clamp(w & ~1, std::min(640, max_w), max_w);
    h = std::clamp(h & ~1, std::min(360, max_h), max_h);
    if (w <= h) {
        h = std::max(360, (w - 16) & ~1);
    }

    int outer_w = w + border_left + border_right;
    int outer_h = h + border_top + border_bottom;
    int cx = usable_x + std::max(0, (usable_w - outer_w) / 2) + border_left;
    int cy = usable_y + std::max(0, (usable_h - outer_h) / 2) + border_top;
    cx = std::max(cx, usable_x + border_left);
    cy = std::max(cy, usable_y + border_top);

    return WindowGeometry{cx, cy, w, h};
}

PcConfig LoadConfig(const std::filesystem::path& path) {
    PcConfig cfg;
    std::ifstream in(path);
    if (!in.is_open()) {
        ApplyPortableDefaultPaths(cfg);
        return cfg;
    }
    bool explicit_window_mode = false;
    bool explicit_quality = false;
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find_first_of("#;");
        if (hash != std::string::npos) {
            line.erase(hash);
        }
        line = Trim(line);
        if (line.empty() || line.front() == '[') {
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (val.empty()) continue;
        try {
            if (key == "width") {
                int v = std::stoi(val);
                cfg.width = (v <= 0) ? 0 : std::max(640, v);
            } else if (key == "height") {
                int v = std::stoi(val);
                cfg.height = (v <= 0) ? 0 : std::max(360, v);
            } else if (key == "window_mode") {
                cfg.window_mode = ParseWindowMode(val, cfg.window_mode);
                explicit_window_mode = true;
            } else if (key == "fullscreen") {
                cfg.fullscreen = ParseBool(val);
            } else if (key == "borderless") {
                cfg.borderless = ParseBool(val);
            } else if (key == "render_scale") cfg.render_scale = std::clamp(std::stof(val), 0.5f, 2.0f);
            else if (key == "anisotropy") cfg.anisotropy = std::clamp(std::stoi(val), 1, 16);
            else if (key == "msaa_samples") cfg.msaa_samples = std::clamp(std::stoi(val), 0, 8);
            else if (key == "vsync") cfg.vsync = ParseBool(val);
            else if (key == "fps_cap") cfg.fps_cap = std::max(0, std::stoi(val));
            else if (key == "unlock_menu_fps") cfg.unlock_menu_fps = ParseBool(val);
            else if (key == "quality_preset" || key == "quality") {
                cfg.quality_preset = ParseQualityPreset(val, cfg.quality_preset);
                explicit_quality = true;
            } else if (key == "high_quality_preset") {
                cfg.high_quality_preset = ParseBool(val);
            } else if (key == "verbose_logs" || key == "verbose") {
                cfg.verbose_logs = ParseBool(val);
            } else if (key == "android_tv_mode") cfg.android_tv_mode = ParseBool(val);
            else if (key == "steer_ramp_rate") cfg.steer_ramp_rate = std::clamp(std::stof(val), 0.5f, 20.0f);
            else if (key == "steer_return_rate") cfg.steer_return_rate = std::clamp(std::stof(val), 0.5f, 30.0f);
            else if (key == "key_throttle") cfg.key_throttle = val;
            else if (key == "key_brake") cfg.key_brake = val;
            else if (key == "key_left") cfg.key_left = val;
            else if (key == "key_right") cfg.key_right = val;
            else if (key == "key_handbrake") cfg.key_handbrake = val;
            else if (key == "key_camera") cfg.key_camera = val;
            else if (key == "key_rear_view") cfg.key_rear_view = val;
            else if (key == "key_confirm") cfg.key_confirm = val;
            else if (key == "key_back") cfg.key_back = val;
            else if (key == "key_pause") cfg.key_pause = val;
            else if (key == "apk_path") cfg.apk_path = val;
            else if (key == "readonly_files_dir") cfg.readonly_files_dir = val;
            else if (key == "userdata_dir") cfg.userdata_dir = val;
            else if (key == "work_bins_dir") cfg.work_bins_dir = val;
            else if (key == "auto_check_updates") cfg.auto_check_updates = ParseBool(val);
            else if (key == "auto_install_updates") cfg.auto_install_updates = ParseBool(val);
            else if (key == "skipped_update_version") cfg.skipped_update_version = val;
            else if (key == "online_mode") cfg.online_mode = ParseBool(val);
        } catch (const std::exception&) {
            // Keep default value on malformed numeric INI entry
        }
    }
    if (!explicit_window_mode) {
        if (cfg.fullscreen) cfg.window_mode = WindowMode::Fullscreen;
        else if (cfg.borderless) cfg.window_mode = WindowMode::Borderless;
        else cfg.window_mode = WindowMode::Windowed;
    }
    if (!explicit_quality) {
        cfg.quality_preset = cfg.high_quality_preset ? QualityPreset::Ultra : QualityPreset::High;
    }
    cfg.high_quality_preset = (cfg.quality_preset == QualityPreset::Ultra);
    cfg.fullscreen = (cfg.window_mode == WindowMode::Fullscreen);
    cfg.borderless = (cfg.window_mode == WindowMode::Borderless);
    ApplyPortableDefaultPaths(cfg);
    return cfg;
}

bool SaveConfig(const std::filesystem::path& path, const PcConfig& cfg) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out << "[display]\n"
        << "# window_mode: windowed | borderless | fullscreen\n"
        << "window_mode=" << WindowModeToString(cfg.window_mode) << "\n"
        << "# 0 = auto-detect best size fitting your monitor without overflowing\n"
        << "width=" << cfg.width << "\n"
        << "height=" << cfg.height << "\n"
        << "vsync=" << (cfg.vsync ? "true" : "false") << "\n"
        << "fps_cap=" << cfg.fps_cap << "\n"
        << "unlock_menu_fps=" << (cfg.unlock_menu_fps ? "true" : "false") << "\n"
        << "\n[graphics]\n"
        << "# quality_preset: low | medium | high | ultra\n"
        << "quality_preset=" << QualityPresetToString(cfg.quality_preset) << "\n"
        << "render_scale=" << cfg.render_scale << "\n"
        << "anisotropy=" << cfg.anisotropy << "\n"
        << "msaa_samples=" << cfg.msaa_samples << "\n"
        << "\n[logging]\n"
        << "# verbose_logs: true opens a live console window with detailed engine/JNI/GLES logs\n"
        << "verbose_logs=" << (cfg.verbose_logs ? "true" : "false") << "\n"
        << "\n[input]\n"
        << "android_tv_mode=" << (cfg.android_tv_mode ? "true" : "false") << "\n"
        << "steer_ramp_rate=" << cfg.steer_ramp_rate << "\n"
        << "steer_return_rate=" << cfg.steer_return_rate << "\n"
        << "key_throttle=" << cfg.key_throttle << "\n"
        << "key_brake=" << cfg.key_brake << "\n"
        << "key_left=" << cfg.key_left << "\n"
        << "key_right=" << cfg.key_right << "\n"
        << "key_handbrake=" << cfg.key_handbrake << "\n"
        << "key_camera=" << cfg.key_camera << "\n"
        << "key_rear_view=" << cfg.key_rear_view << "\n"
        << "key_confirm=" << cfg.key_confirm << "\n"
        << "key_back=" << cfg.key_back << "\n"
        << "key_pause=" << cfg.key_pause << "\n"
        << "\n[updates]\n"
        << "auto_check_updates=" << (cfg.auto_check_updates ? "true" : "false") << "\n"
        << "auto_install_updates=" << (cfg.auto_install_updates ? "true" : "false") << "\n";
    if (!cfg.skipped_update_version.empty()) {
        out << "skipped_update_version=" << cfg.skipped_update_version << "\n";
    }
    out << "\n[network]\n"
        << "# online_mode: true enables Cloudcell HTTP and sockets (needed for Project Resurrection online)\n"
        << "online_mode=" << (cfg.online_mode ? "true" : "false") << "\n";

    if (!cfg.apk_path.empty() || !cfg.readonly_files_dir.empty() ||
        !cfg.userdata_dir.empty() || !cfg.work_bins_dir.empty()) {
        out << "\n[paths]\n";
        if (!cfg.apk_path.empty()) out << "apk_path=" << cfg.apk_path << "\n";
        if (!cfg.readonly_files_dir.empty()) out << "readonly_files_dir=" << cfg.readonly_files_dir << "\n";
        if (!cfg.userdata_dir.empty()) out << "userdata_dir=" << cfg.userdata_dir << "\n";
        if (!cfg.work_bins_dir.empty()) out << "work_bins_dir=" << cfg.work_bins_dir << "\n";
    }
    return out.good();
}

void SaveDefaultConfigIfMissing(const std::filesystem::path& path, const PcConfig& cfg) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return;
    }
    SaveConfig(path, cfg);
}
