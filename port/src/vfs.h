#pragma once

#include "config.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class LayeredVfs {
public:
    static constexpr const char* kAndroidFilesRoot = "/storage/rr3";
    static constexpr const char* kAndroidCacheRoot = "/storage/rr3_cache";

    void Initialize(const std::filesystem::path& readonly_files_dir,
                    const std::filesystem::path& userdata_dir);

    // Maps an Android/POSIX path to a host path for reading (checks userdata overlay first,
    // then readonly_files_dir). Non-virtual paths (e.g. /proc/cpuinfo) return synthetic paths.
    std::filesystem::path ResolveRead(std::string_view android_path) const;

    // Returns all existing host directories corresponding to an Android/POSIX directory path
    // (userdata overlay first, then readonly_files_dir) without creating any directories.
    std::vector<std::filesystem::path> ResolveReadDirs(std::string_view android_path) const;

    // Maps an Android/POSIX path strictly into userdata_dir (never returns readonly_files_dir).
    // Returns empty path if path is outside writable roots.
    std::filesystem::path ResolveWrite(std::string_view android_path) const;

    void SetQualityPreset(QualityPreset preset) { quality_preset_ = preset; }
    QualityPreset GetQualityPreset() const { return quality_preset_; }
    void SetHighQualityPreset(bool enabled) {
        quality_preset_ = enabled ? QualityPreset::Ultra : QualityPreset::High;
    }
    void SetApkPath(const std::filesystem::path& apk_path) { apk_path_ = apk_path; }
    bool IsVirtualPath(std::string_view android_path) const;
    const std::filesystem::path& ReadonlyFilesDir() const { return readonly_files_dir_; }
    const std::filesystem::path& UserdataDir() const { return userdata_dir_; }

private:
    bool StripToRelative(std::string_view android_path, std::string& out_rel) const;

    std::filesystem::path readonly_files_dir_;
    std::filesystem::path userdata_dir_;
    std::filesystem::path proc_dir_;
    std::filesystem::path apk_path_;
    QualityPreset quality_preset_ = QualityPreset::Ultra;
};
