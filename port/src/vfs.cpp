#include "vfs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace {
std::string NormalizeSlashes(std::string_view s) {
    std::string out(s);
    std::replace(out.begin(), out.end(), '\\', '/');
    while (out.find("//") != std::string::npos) {
        out.replace(out.find("//"), 2, "/");
    }
    return out;
}
} // namespace

void LayeredVfs::Initialize(const std::filesystem::path& readonly_files_dir,
                            const std::filesystem::path& userdata_dir) {
    readonly_files_dir_ = std::filesystem::weakly_canonical(readonly_files_dir);
    userdata_dir_ = std::filesystem::weakly_canonical(userdata_dir);
    std::filesystem::create_directories(userdata_dir_);
    std::filesystem::create_directories(userdata_dir_ / "cache");
    proc_dir_ = userdata_dir_ / "_proc";
    std::filesystem::create_directories(proc_dir_);

    // Write synthetic /proc/cpuinfo and /proc/meminfo for engine hardware probing.
    {
        std::ofstream f(proc_dir_ / "cpuinfo", std::ios::binary);
        f << "Processor\t: AArch64 Processor rev 0 (aarch64)\n"
          << "processor\t: 0\n"
          << "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
          << "Hardware\t: Qualcomm Technologies, Inc SM8550\n";
    }
    {
        std::ofstream f(proc_dir_ / "meminfo", std::ios::binary);
        f << "MemTotal:        8388608 kB\n"
          << "MemFree:         6291456 kB\n"
          << "MemAvailable:    7340032 kB\n";
    }

    // One-time seed of writable doc/ saves from read-only com.ea.games.r3_row/files/doc/.
    auto src_doc = readonly_files_dir_ / "doc";
    auto dst_doc = userdata_dir_ / "doc";
    if (std::filesystem::exists(src_doc) && !std::filesystem::exists(dst_doc)) {
        std::error_code ec;
        std::filesystem::copy(src_doc, dst_doc,
                              std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::skip_existing,
                              ec);
        if (ec) {
            std::fprintf(stderr, "[VFS] Warning seeding doc/: %s\n", ec.message().c_str());
        }
    }
    std::filesystem::create_directories(dst_doc);
}

bool LayeredVfs::StripToRelative(std::string_view android_path, std::string& out_rel) const {
    std::string p = NormalizeSlashes(android_path);
    // Check /storage/rr3_cache BEFORE /storage/rr3 so cache paths map to userdata/cache/!
    if (p == "/storage/rr3_cache" || p.rfind("/storage/rr3_cache/", 0) == 0) {
        out_rel = "cache/" + (p.size() > 19 ? p.substr(19) : "");
        return true;
    }
    static const char* kPrefixes[] = {
        "/storage/rr3/",
        "/storage/rr3",
        "/sdcard/Android/data/com.ea.games.r3_row/files/",
        "/storage/emulated/0/Android/data/com.ea.games.r3_row/files/",
        "/data/data/com.ea.games.r3_row/files/",
        "/data/user/0/com.ea.games.r3_row/files/",
    };
    for (const char* pref : kPrefixes) {
        size_t len = std::strlen(pref);
        if (p.rfind(pref, 0) == 0) {
            if (p.size() > len && pref[len - 1] != '/' && p[len] != '/') continue;
            out_rel = p.substr(len);
            while (!out_rel.empty() && out_rel.front() == '/') out_rel.erase(out_rel.begin());
            return true;
        }
    }
    if (p.rfind("/data/data/com.ea.games.r3_row/", 0) == 0) {
        out_rel = p.substr(31);
        return true;
    }
    if (p.rfind("/data/user/0/com.ea.games.r3_row/", 0) == 0) {
        out_rel = p.substr(33);
        return true;
    }
    // Relative path (e.g. ".depot/..." or "doc/...")
    if (!p.empty() && p[0] != '/' && !(p.size() >= 2 && p[1] == ':')) {
        out_rel = p;
        return true;
    }
    return false;
}

bool LayeredVfs::IsVirtualPath(std::string_view android_path) const {
    std::string rel;
    return StripToRelative(android_path, rel);
}

std::filesystem::path LayeredVfs::ResolveRead(std::string_view android_path) const {
    std::string p = NormalizeSlashes(android_path);
    if (p == "/proc/cpuinfo") return proc_dir_ / "cpuinfo";
    if (p == "/proc/meminfo") return proc_dir_ / "meminfo";
    if (p == "/storage/rr3/base.apk") {
        std::error_code ec;
        if (!apk_path_.empty() && std::filesystem::exists(apk_path_, ec)) {
            return apk_path_;
        }
        std::filesystem::path apk = readonly_files_dir_.parent_path().parent_path() / "RR3 14.0.1.apk";
        if (std::filesystem::exists(apk, ec)) return apk;
    }

    std::string rel;
    if (StripToRelative(p, rel)) {
        std::error_code ec;
        auto overlay = userdata_dir_ / std::filesystem::path(rel);
        if (std::filesystem::exists(overlay, ec)) {
            return overlay;
        }
        auto ro = readonly_files_dir_ / std::filesystem::path(rel);
        if (std::filesystem::exists(ro, ec)) {
            return ro;
        }
        // Cross-tier GUI asset fallback: if a user's .depot only ships one gui_assets/<W>x<H>/ pack
        // (e.g. Project Resurrection's 2048x1536 pack or a 960x640 backup), fall back to the
        // installed tier so missing textures never return NULL or stall AssetDownloadService.
        static constexpr const char* kGuiPrefixes[] = {
            ".depot/gui_assets/",
            "apk/res/gui_assets/",
        };
        for (const char* gpref : kGuiPrefixes) {
            const size_t glen = std::strlen(gpref);
            if (rel.rfind(gpref, 0) == 0 && rel.size() > glen) {
                const size_t slash = rel.find('/', glen);
                if (slash != std::string::npos && slash + 1 < rel.size()) {
                    const std::string req_tier = rel.substr(glen, slash - glen);
                    if (req_tier == "2048x1536" || req_tier == "960x640" || req_tier == "480x320") {
                        const std::string rest = rel.substr(slash + 1);
                        const char* candidates[] = {
                            preferred_gui_tier_.empty() ? nullptr : preferred_gui_tier_.c_str(),
                            "2048x1536",
                            "960x640",
                            "480x320",
                        };
                        for (const char* cand : candidates) {
                            if (!cand || req_tier == cand) continue;
                            std::string alt_rel = std::string(gpref) + cand + "/" + rest;
                            auto alt_ov = userdata_dir_ / std::filesystem::path(alt_rel);
                            if (std::filesystem::exists(alt_ov, ec)) return alt_ov;
                            auto alt_ro = readonly_files_dir_ / std::filesystem::path(alt_rel);
                            if (std::filesystem::exists(alt_ro, ec)) return alt_ro;
                        }
                    }
                }
                break;
            }
        }
        // Default to overlay path if neither exists yet
        return overlay;
    }
    return std::filesystem::path(android_path);
}

std::vector<std::filesystem::path> LayeredVfs::ResolveReadDirs(std::string_view android_path) const {
    std::vector<std::filesystem::path> dirs;
    std::string p = NormalizeSlashes(android_path);
    std::string rel;
    if (StripToRelative(p, rel)) {
        auto overlay = userdata_dir_ / std::filesystem::path(rel);
        auto ro = readonly_files_dir_ / std::filesystem::path(rel);
        std::error_code ec;
        if (std::filesystem::is_directory(overlay, ec)) dirs.push_back(overlay);
        ec.clear();
        if (std::filesystem::is_directory(ro, ec) && ro != overlay) dirs.push_back(ro);
        return dirs;
    }
    std::filesystem::path direct(android_path);
    std::error_code ec;
    if (std::filesystem::is_directory(direct, ec)) dirs.push_back(direct);
    return dirs;
}

std::filesystem::path LayeredVfs::ResolveWrite(std::string_view android_path) const {
    if (android_path.empty()) return {};
    auto is_within = [](const std::filesystem::path& child, const std::filesystem::path& parent) -> bool {
        std::error_code ec;
        auto c = std::filesystem::weakly_canonical(child, ec);
        if (ec) return false;
        auto p = std::filesystem::weakly_canonical(parent, ec);
        if (ec) return false;
        auto cs = c.wstring();
        auto ps = p.wstring();
        if (cs.size() < ps.size()) return false;
        if (_wcsnicmp(cs.c_str(), ps.c_str(), ps.size()) != 0) return false;
        return cs.size() == ps.size() || cs[ps.size()] == L'\\' || cs[ps.size()] == L'/';
    };

    std::string p = NormalizeSlashes(android_path);
    std::string rel;
    if (StripToRelative(p, rel)) {
        auto candidate = (userdata_dir_ / std::filesystem::path(rel)).lexically_normal();
        // Reject '..' traversal escaping userdata_dir_!
        if (!is_within(candidate, userdata_dir_)) {
            return {};
        }
        if (candidate.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(candidate.parent_path(), ec);
        }
        return candidate;
    }
    // Non-virtual host path: redirect read-only inputs into userdata_dir_, block writes outside userdata_dir_!
    std::filesystem::path direct(android_path);
    if (is_within(direct, userdata_dir_)) {
        if (direct.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(direct.parent_path(), ec);
        }
        return direct;
    }
    if (is_within(direct, readonly_files_dir_)) {
        std::error_code ec;
        auto rel_from_ro = std::filesystem::relative(std::filesystem::weakly_canonical(direct, ec),
                                                     readonly_files_dir_, ec);
        if (!ec && !rel_from_ro.empty()) {
            auto redirected = (userdata_dir_ / rel_from_ro).lexically_normal();
            if (is_within(redirected, userdata_dir_)) {
                if (redirected.has_parent_path()) {
                    std::filesystem::create_directories(redirected.parent_path(), ec);
                }
                return redirected;
            }
        }
    }
    // Strictly refuse writes anywhere outside userdata_dir_ (protects com.ea.games.r3_row and RR3 14.0.1.apk).
    return {};
}
