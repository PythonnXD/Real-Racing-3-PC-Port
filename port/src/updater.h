#pragma once

#include "config.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

struct ReleaseInfo {
    bool ok = false;
    bool update_available = false;
    std::string current_version;
    std::string latest_version;
    std::string tag_name;
    std::string release_name;
    std::string release_notes;
    std::string html_url;
    std::string asset_name;
    std::string download_url;
    uint64_t asset_size = 0;
    std::string error;
};

// Compares two semver strings (leading 'v'/'V' ignored; e.g. "1.0.0" vs "v1.1.0").
// Returns <0 if a < b, 0 if equal, >0 if a > b.
int CompareSemver(const std::string& a, const std::string& b);

// Validates that a URL uses HTTPS on github.com or *.githubusercontent.com.
bool IsTrustedGithubUrl(const std::string& url);

// Parses GitHub /releases/latest JSON into ReleaseInfo (exposed for unit testing).
ReleaseInfo ParseLatestReleaseJson(const std::string& json_text, const std::string& current_version);

// Queries GitHub Releases API for the latest release of repo_slug over HTTPS (WinHTTP).
ReleaseInfo CheckForUpdates(const std::string& current_version = kPortVersion,
                            const std::string& repo_slug = kGithubRepoSlug);

// Downloads an HTTPS file with progress and cancellation support (follows GitHub CDN redirects).
bool DownloadFileWithProgress(const std::string& url,
                              const std::filesystem::path& dest_path,
                              const std::function<void(uint64_t downloaded, uint64_t total)>& on_progress,
                              std::atomic<bool>* cancel_flag,
                              std::string& out_error);

// Extracts a release zip into staging_dir, validates rr3_pc.exe is present, blocks path traversal,
// and drops rr3_pc.ini from staging if install_dir/rr3_pc.ini already exists (preserving user settings).
bool ExtractUpdateZipToStaging(const std::filesystem::path& zip_path,
                               const std::filesystem::path& staging_dir,
                               const std::filesystem::path& install_dir,
                               std::string& out_error);

// Writes and spawns a detached helper script that waits for the current process to exit,
// copies staged runtime files into install_dir, cleans up temporary files, and relaunches rr3_pc.exe.
bool LaunchUpdateApplyHelper(const std::filesystem::path& staging_dir,
                             const std::filesystem::path& zip_path,
                             const std::filesystem::path& install_dir,
                             std::string& out_error);

// Removes leftover _update_staging, _update_download.zip, and _apply_update.cmd from a previous run.
void CleanupLeftoverUpdateFiles(const std::filesystem::path& install_dir);
