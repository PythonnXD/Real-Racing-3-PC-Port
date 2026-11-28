#pragma once

#include <filesystem>
#include <string>

inline constexpr const char* kPortVersion = "1.2.0";
inline constexpr const char* kGithubRepoSlug = "PythonnXD/Real-Racing-3-PC-Port";

enum class WindowMode {
    Windowed,
    Borderless,
    Fullscreen,
};

enum class QualityPreset {
    Low,
    Medium,
    High,
    Ultra,
};

struct WindowGeometry {
    int x = 0;
    int y = 0;
    int w = 1280;
    int h = 720;
};

struct PcConfig {
    int width = 0;  // 0 = auto-detect best fit inside monitor usable bounds
    int height = 0; // 0 = auto-detect best fit inside monitor usable bounds
    WindowMode window_mode = WindowMode::Windowed;
    bool fullscreen = false; // legacy alias (honored when window_mode is not set)
    bool borderless = false; // legacy alias (honored when window_mode is not set)
    float render_scale = 1.0f;
    int anisotropy = 16;
    int msaa_samples = 0;
    bool vsync = true;
    int fps_cap = 0; // 0 = uncapped / monitor vsync
    bool unlock_menu_fps = true;
    QualityPreset quality_preset = QualityPreset::Ultra;
    bool high_quality_preset = true; // legacy alias (true -> Ultra, false -> High)
    bool verbose_logs = false;
    bool android_tv_mode = false;
    float steer_ramp_rate = 4.5f;
    float steer_return_rate = 7.0f;
    std::string key_throttle = "W";
    std::string key_brake = "S";
    std::string key_left = "A";
    std::string key_right = "D";
    std::string key_handbrake = "Space"; // Secondary Brake key
    std::string key_camera = "C";
    std::string key_rear_view = "V";
    std::string key_confirm = "Return";
    std::string key_back = "Escape";
    std::string key_pause = "Escape";
    // Paths (empty = auto-resolved portably relative to executable / dev workspace)
    std::string apk_path;
    std::string readonly_files_dir;
    std::string userdata_dir;
    std::string work_bins_dir;
    // Updater & network settings
    bool auto_check_updates = true;
    bool auto_install_updates = false;
    std::string skipped_update_version;
    bool online_mode = false;
};

WindowMode ParseWindowMode(const std::string& val, WindowMode fallback = WindowMode::Windowed);
const char* WindowModeToString(WindowMode mode);

QualityPreset ParseQualityPreset(const std::string& val, QualityPreset fallback = QualityPreset::Ultra);
const char* QualityPresetToString(QualityPreset preset);
const char* QualityPresetToDisplayName(QualityPreset preset);
const char* QualityPresetToAndroidStem(QualityPreset preset);

// Global lock-free verbose logging flag shared across all runtime subsystems.
void SetVerboseLogging(bool enabled);
bool IsVerboseLogging();

// Portable path resolution (works in both standalone release folder and dev port/build/ tree).
std::filesystem::path GetExecutableDir();
std::filesystem::path ResolveDefaultConfigPath();
void ApplyPortableDefaultPaths(PcConfig& cfg);

// Computes safe client-area geometry so the window (including titlebar & frame borders in Windowed mode)
// never overflows the monitor bounds.
WindowGeometry ComputeSafeWindowGeometry(WindowMode mode,
                                         int req_w, int req_h,
                                         int display_x, int display_y, int display_w, int display_h,
                                         int usable_x, int usable_y, int usable_w, int usable_h,
                                         int border_top, int border_left, int border_bottom, int border_right);

PcConfig LoadConfig(const std::filesystem::path& path);
bool SaveConfig(const std::filesystem::path& path, const PcConfig& cfg);
void SaveDefaultConfigIfMissing(const std::filesystem::path& path, const PcConfig& cfg);
