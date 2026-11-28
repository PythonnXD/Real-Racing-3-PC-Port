#pragma once

#include "config.h"

#include <filesystem>

struct LauncherOptions {
    std::filesystem::path config_path;
    int initial_tab = 0;
    std::filesystem::path screenshot_out;
    std::filesystem::path update_popup_screenshot;
    std::string simulate_version;
    bool disable_update_check = false;
};

// Runs the built-in Win32 Launcher & Configurator UI.
// Returns true if the user clicked Play (populating out_cfg); returns false if closed or in screenshot mode.
bool RunLauncherUi(const LauncherOptions& opts, PcConfig& out_cfg);
