#pragma once

#include "config.h"
#include "elf_loader.h"
#include "gles_bridge.h"
#include "jni_env.h"
#include "vfs.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class FiremintRuntime {
public:
    FiremintRuntime(CpuRuntime& cpu, ElfLoader& loader, GlesBridge& gles,
                    FakeJvm& jvm, LayeredVfs& vfs, const PcConfig& cfg);
    ~FiremintRuntime();

    // Registers all com/firemint/realracing/* classes (MainActivity, Platform, Font, GlyphVector,
    // TextureInfo, CarLiveryBaker, ControllerManager, AudioStreamManager, Input, MoviePlayer, TextField).
    void RegisterClasses();

    uint64_t MainActivityHandle() const { return main_activity_handle_; }
    uint64_t AssetManagerHandle() const { return asset_manager_handle_; }
    bool ShouldExit() const { return should_exit_; }
    void RequestExit() { should_exit_ = true; }

    // Queue a callback to run on the main/GL thread between frames.
    void PostGlCallback(std::function<void()> cb);
    void PumpGlCallbacks();

private:
    CpuRuntime& cpu_;
    ElfLoader& loader_;
    GlesBridge& gles_;
    FakeJvm& jvm_;
    LayeredVfs& vfs_;
    const PcConfig& cfg_;

    uint64_t main_activity_handle_ = 0;
    uint64_t asset_manager_handle_ = 0;
    bool should_exit_ = false;

    std::mutex cb_mtx_;
    std::vector<std::function<void()>> gl_callbacks_;
};
