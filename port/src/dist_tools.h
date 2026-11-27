#pragma once

#include "config.h"

#include <filesystem>
#include <string>
#include <vector>

inline constexpr const char* kExpectedRr3SoSha256 =
    "57edb6314fb1f73916446dccf2aab2223a932ae63ccdf2149cffeddb5aa73051";

struct GameDataStatus {
    bool has_libs = false;
    bool hash_matches = false;
    bool has_depot = false;
    bool has_save_data = false;
    size_t save_file_count = 0;
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
