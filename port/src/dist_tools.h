#pragma once

#include "config.h"

#include <filesystem>
#include <string>
#include <vector>

inline constexpr const char* kExpectedRr3SoSha256 =
    "57edb6314fb1f73916446dccf2aab2223a932ae63ccdf2149cffeddb5aa73051";

enum class DepotGpuFormat {
    Atc,    // Adreno: .atc.dds.z (GL_AMD_compressed_ATC_texture)
    Etc,    // Mali / generic: .etc.dds.z (GL_OES_compressed_ETC1_RGB8_texture only)
    Dxt,    // Xclipse / Tegra: .dxt.dds.z (GL_EXT_texture_compression_s3tc)
    Pvrtc,  // PowerVR: .ptc.pvr.z (GL_IMG_texture_compression_pvrtc)
};

enum class DepotGuiTier {
    Auto,
    Res480x320,
    Res960x640,
    Res2048x1536,
};

struct DepotProfile {
    DepotGpuFormat gpu_format = DepotGpuFormat::Atc;
    DepotGuiTier gui_tier = DepotGuiTier::Auto;
    size_t atc_count = 0;
    size_t etc_count = 0;
    size_t dxt_count = 0;
    size_t pvrtc_count = 0;
    size_t gui_480_count = 0;
    size_t gui_960_count = 0;
    size_t gui_2048_count = 0;
    bool has_apk_res = false;
    std::string cache_build_id;
};

const char* DepotGpuFormatToString(DepotGpuFormat fmt);
const char* DepotGpuDefaultRenderer(DepotGpuFormat fmt);
const char* DepotGuiTierToDirName(DepotGuiTier tier);

// Inspects .depot texture format (Adreno ATC / Mali ETC / Xclipse-Tegra DXT / PowerVR PVRTC),
// installed gui_assets resolution tier (2048x1536 / 960x640 / 480x320), apk/res presence, and cache build.
DepotProfile DetectDepotProfile(const std::filesystem::path& readonly_files_dir,
                                const std::filesystem::path& userdata_dir);

// Ensures Android's unpacked APK resources (res/xR.zip -> apk/res/) exist in readonly_files_dir or userdata_dir.
// Never writes to readonly_files_dir; extracts to userdata_dir/apk/ if missing.
bool EnsureApkResExtracted(const std::filesystem::path& apk_path,
                           const std::filesystem::path& readonly_files_dir,
                           const std::filesystem::path& userdata_dir,
                           std::string& out_status);

struct GameDataStatus {
    bool has_libs = false;
    bool hash_matches = false;
    bool has_depot = false;
    bool has_save_data = false;
    size_t save_file_count = 0;
    DepotProfile depot_profile{};
    std::string rr3_sha256;
    std::string summary;
};

// Computes lowercase hex SHA-256 of a file using Windows BCrypt. Returns empty string on error.
std::string ComputeFileSha256Hex(const std::filesystem::path& path);

// Checks whether extracted ARM64 libraries (with SHA-256) and stock Android .depot assets are ready.
GameDataStatus CheckGameDataStatus(const PcConfig& cfg);

// Extracts lib/arm64-v8a/{libRealRacing3.so, libc++_shared.so, libfmodex.so, [libNimble.so]}
// from a user-provided stock RR3 14.0.1 APK into out_arm64_dir and verifies libRealRacing3.so SHA-256.
bool ExtractArm64LibsFromApk(const std::filesystem::path& apk_path,
                             const std::filesystem::path& out_arm64_dir,
                             std::string& out_status);

// Accepts com.ea.games.r3_row, com.ea.games.r3_row/files, or .depot and normalizes to the .../files folder.
bool NormalizeGameDataDir(const std::filesystem::path& chosen_dir,
                          std::filesystem::path& out_files_dir,
                          std::string& out_status);

// Exports writable user save data (userdata/doc/*) into dest_dir.
bool ExportUserData(const std::filesystem::path& userdata_dir,
                    const std::filesystem::path& dest_dir,
                    std::string& out_status);

// Imports save data from a folder (com.ea.games.r3_row, files/, doc/, or folder of .dat files)
// or a single .dat file into userdata/doc/. Always auto-backs up existing userdata/doc first.
bool ImportUserData(const std::filesystem::path& src_path,
                    const std::filesystem::path& userdata_dir,
                    std::string& out_status);

// Exports the primary save file (character.2.dat) or a full .zip backup of userdata/doc/.
bool ExportSaveFile(const std::filesystem::path& userdata_dir,
                    const std::filesystem::path& dest_file,
                    std::string& out_status);

// Imports one or more save files (.dat/.bin/.pld) and/or .zip save backups into userdata/doc/.
// Always creates a timestamped backup of existing userdata/doc/ before overwriting.
bool ImportSaveFiles(const std::vector<std::filesystem::path>& src_files,
                     const std::filesystem::path& userdata_dir,
                     std::string& out_status);
