#include "dist_tools.h"

#include <windows.h>
#include <bcrypt.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

namespace {

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

std::string MakeTimestampSuffix() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_val{};
    localtime_s(&tm_val, &tt);
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
                  tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
    return buf;
}

bool InflateRaw(const uint8_t* comp, size_t comp_size, std::vector<uint8_t>& out, size_t expected_size) {
    out.assign(expected_size, 0);
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        return false;
    }
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(comp));
    zs.avail_in = static_cast<uInt>(comp_size);
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());

    int rc = inflate(&zs, Z_FINISH);
    size_t produced = zs.total_out;
    inflateEnd(&zs);

    if (rc != Z_STREAM_END && rc != Z_OK) {
        return false;
    }
    out.resize(produced);
    return true;
}

bool CountFilesInDir(const std::filesystem::path& dir, size_t& out_count, uintmax_t& out_bytes) {
    out_count = 0;
    out_bytes = 0;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            ++out_count;
            out_bytes += entry.file_size(ec);
        }
    }
    return true;
}

size_t CountRegularFilesCapped(const std::filesystem::path& dir, size_t cap) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return 0;
    size_t n = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) { ec.clear(); continue; }
        if (entry.is_regular_file(ec)) {
            ++n;
            if (n >= cap) break;
        }
    }
    return n;
}

bool EndsWithLower(const std::string& lower_s, const char* suffix) {
    size_t slen = std::strlen(suffix);
    return lower_s.size() >= slen && lower_s.compare(lower_s.size() - slen, slen, suffix) == 0;
}

bool FindZipEocd(const uint8_t* data, size_t len, int64_t& out_eocd) {
    out_eocd = -1;
    if (len < 22) return false;
    for (int64_t i = static_cast<int64_t>(len) - 22; i >= 0; --i) {
        if (ReadLe32(data + i) == 0x06054b50u) {
            uint16_t comment_len = ReadLe16(data + i + 20);
            if (i + 22 + comment_len == static_cast<int64_t>(len)) {
                out_eocd = i;
                return true;
            }
        }
    }
    return false;
}

bool ExtractZipBufferToDir(const uint8_t* zip_data,
                           size_t zip_size,
                           const std::filesystem::path& dest_root,
                           size_t& out_extracted,
                           std::string& out_err) {
    out_extracted = 0;
    const size_t tail_len = std::min<size_t>(zip_size, 65557);
    const size_t tail_off = zip_size - tail_len;
    int64_t rel_eocd = -1;
    if (!FindZipEocd(zip_data + tail_off, tail_len, rel_eocd)) {
        out_err = "Inner resource ZIP missing End of Central Directory.";
        return false;
    }
    const size_t eocd = tail_off + static_cast<size_t>(rel_eocd);
    const uint16_t total_entries = ReadLe16(zip_data + eocd + 10);
    const uint32_t cd_size = ReadLe32(zip_data + eocd + 12);
    const uint32_t cd_offset = ReadLe32(zip_data + eocd + 16);
    if (static_cast<uint64_t>(cd_offset) + cd_size > zip_size) {
        out_err = "Inner resource ZIP Central Directory out of bounds.";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dest_root, ec);
    const auto canon_root = std::filesystem::weakly_canonical(dest_root, ec);
    const uint8_t* cd = zip_data + cd_offset;
    size_t pos = 0;

    for (uint16_t idx = 0; idx < total_entries && pos + 46 <= cd_size; ++idx) {
        if (ReadLe32(cd + pos) != 0x02014b50u) break;
        const uint16_t method = ReadLe16(cd + pos + 10);
        const uint32_t comp_sz = ReadLe32(cd + pos + 20);
        const uint32_t uncomp_sz = ReadLe32(cd + pos + 24);
        const uint16_t name_len = ReadLe16(cd + pos + 28);
        const uint16_t extra_len = ReadLe16(cd + pos + 30);
        const uint16_t comment_len = ReadLe16(cd + pos + 32);
        const uint32_t lfh_off = ReadLe32(cd + pos + 42);
        if (pos + 46 + name_len > cd_size) break;

        std::string name(reinterpret_cast<const char*>(cd + pos + 46), name_len);
        pos += 46 + name_len + extra_len + comment_len;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty() || name.back() == '/') continue;
        while (!name.empty() && name.front() == '/') name.erase(name.begin());
        if (name.empty() || name.find("..") != std::string::npos || name.find(':') != std::string::npos) {
            continue;
        }
        if (name.rfind("res/", 0) != 0) {
            name = "res/" + name;
        }

        if (static_cast<uint64_t>(lfh_off) + 30 > zip_size) continue;
        const uint8_t* lfh = zip_data + lfh_off;
        if (ReadLe32(lfh) != 0x04034b50u) continue;
        const uint64_t data_off = static_cast<uint64_t>(lfh_off) + 30 + ReadLe16(lfh + 26) + ReadLe16(lfh + 28);
        if (data_off + comp_sz > zip_size) continue;

        const uint8_t* comp_ptr = zip_data + data_off;
        std::vector<uint8_t> decoded;
        const uint8_t* wptr = nullptr;
        size_t wlen = 0;
        if (method == 0) {
            wptr = comp_ptr;
            wlen = comp_sz;
        } else if (method == 8) {
            if (!InflateRaw(comp_ptr, comp_sz, decoded, uncomp_sz)) continue;
            wptr = decoded.data();
            wlen = decoded.size();
        } else {
            continue;
        }

        const auto dst = (dest_root / std::filesystem::path(name)).lexically_normal();
        const auto canon_dst = std::filesystem::weakly_canonical(dst, ec);
        if (canon_dst.wstring().rfind(canon_root.wstring(), 0) != 0) continue;

        if (dst.has_parent_path()) std::filesystem::create_directories(dst.parent_path(), ec);
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) continue;
        out.write(reinterpret_cast<const char*>(wptr), static_cast<std::streamsize>(wlen));
        if (out.good()) ++out_extracted;
    }

    if (out_extracted == 0) {
        out_err = "Inner resource ZIP contained no extractable files.";
        return false;
    }
    return true;
}

} // namespace

const char* DepotGpuFormatToString(DepotGpuFormat fmt) {
    switch (fmt) {
        case DepotGpuFormat::Atc:   return "Adreno (ATC)";
        case DepotGpuFormat::Etc:   return "Mali (ETC1)";
        case DepotGpuFormat::Dxt:   return "Xclipse/Tegra (DXT)";
        case DepotGpuFormat::Pvrtc: return "PowerVR (PVRTC)";
    }
    return "Adreno (ATC)";
}

const char* DepotGpuDefaultRenderer(DepotGpuFormat fmt) {
    switch (fmt) {
        case DepotGpuFormat::Atc:   return "Adreno (TM) 740";
        case DepotGpuFormat::Etc:   return "Mali-G715";
        case DepotGpuFormat::Dxt:   return "Samsung Xclipse 940";
        case DepotGpuFormat::Pvrtc: return "PowerVR Rogue GE8320";
    }
    return "Adreno (TM) 740";
}

const char* DepotGuiTierToDirName(DepotGuiTier tier) {
    switch (tier) {
        case DepotGuiTier::Res480x320:   return "480x320";
        case DepotGuiTier::Res960x640:   return "960x640";
        case DepotGuiTier::Res2048x1536: return "2048x1536";
        case DepotGuiTier::Auto:         return "auto";
    }
    return "auto";
}

DepotProfile DetectDepotProfile(const std::filesystem::path& readonly_files_dir,
                                const std::filesystem::path& userdata_dir) {
    DepotProfile p;
    std::error_code ec;
    const auto depot = readonly_files_dir / ".depot";

    p.has_apk_res =
        std::filesystem::is_regular_file(readonly_files_dir / "apk" / "res" / "asset_list_base.txt", ec) ||
        (!userdata_dir.empty() &&
         std::filesystem::is_regular_file(userdata_dir / "apk" / "res" / "asset_list_base.txt", ec));

    // Read build number from asset_list_base.txtCache.txt (8-byte header 0xa4f6f39d + version, then ASCII build line)
    for (const auto& root : {userdata_dir, readonly_files_dir}) {
        if (root.empty()) continue;
        const auto cache_file = root / ".depot" / "asset_list_base.txtCache.txt";
        if (std::filesystem::is_regular_file(cache_file, ec)) {
            std::ifstream cf(cache_file, std::ios::binary);
            uint8_t hdr[64]{};
            cf.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
            std::streamsize got = cf.gcount();
            if (got >= 10 && ReadLe32(hdr) == 0xa4f6f39du) {
                std::string bid;
                for (std::streamsize i = 8; i < got; ++i) {
                    char ch = static_cast<char>(hdr[i]);
                    if (ch == '\r' || ch == '\n') break;
                    if (ch >= 32 && ch < 127) bid.push_back(ch);
                }
                p.cache_build_id = bid;
            }
            break;
        }
    }

    if (!std::filesystem::is_directory(depot, ec)) {
        return p;
    }

    // 1. Detect installed gui_assets resolution tier (2048x1536 vs 960x640 vs 480x320)
    const auto gui_root = depot / "gui_assets";
    p.gui_2048_count = CountRegularFilesCapped(gui_root / "2048x1536", 500);
    p.gui_960_count  = CountRegularFilesCapped(gui_root / "960x640", 500);
    p.gui_480_count  = CountRegularFilesCapped(gui_root / "480x320", 500);
    if (!userdata_dir.empty()) {
        const auto ud_gui = userdata_dir / ".depot" / "gui_assets";
        p.gui_2048_count += CountRegularFilesCapped(ud_gui / "2048x1536", 500);
        p.gui_960_count  += CountRegularFilesCapped(ud_gui / "960x640", 500);
        p.gui_480_count  += CountRegularFilesCapped(ud_gui / "480x320", 500);
    }

    size_t best_gui = std::max({p.gui_2048_count, p.gui_960_count, p.gui_480_count});
    if (best_gui > 0) {
        if (p.gui_2048_count >= p.gui_960_count && p.gui_2048_count >= p.gui_480_count) {
            p.gui_tier = DepotGuiTier::Res2048x1536;
        } else if (p.gui_960_count >= p.gui_480_count) {
            p.gui_tier = DepotGuiTier::Res960x640;
        } else {
            p.gui_tier = DepotGuiTier::Res480x320;
        }
    }

    // 2. Bounded scan for GPU-compressed texture extensions across high-signal folders
    size_t matched = 0;
    size_t visited = 0;
    auto scan_dir = [&](const std::filesystem::path& dir) {
        if (matched >= 300 || visited >= 4000) return;
        std::error_code lec;
        if (!std::filesystem::is_directory(dir, lec)) return;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, lec)) {
            if (lec) { lec.clear(); continue; }
            if (!entry.is_regular_file(lec)) continue;
            ++visited;
            std::string name = entry.path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (EndsWithLower(name, ".atc.dds.z") || EndsWithLower(name, ".atc.dds")) {
                ++p.atc_count; ++matched;
            } else if (EndsWithLower(name, ".etc.dds.z") || EndsWithLower(name, ".etc.dds")) {
                ++p.etc_count; ++matched;
            } else if (EndsWithLower(name, ".dxt.dds.z") || EndsWithLower(name, ".dxt.dds")) {
                ++p.dxt_count; ++matched;
            } else if (EndsWithLower(name, ".ptc.pvr.z") || EndsWithLower(name, ".ptc.pvr")) {
                ++p.pvrtc_count; ++matched;
            }
            if (matched >= 300 || visited >= 4000) break;
        }
    };

    scan_dir(depot / "gui_assets");
    scan_dir(depot / "vehicles");
    scan_dir(depot / "Tracks");
    scan_dir(depot / "ui");
    if (matched == 0) {
        scan_dir(depot);
    }

    size_t best_gpu = std::max({p.atc_count, p.etc_count, p.dxt_count, p.pvrtc_count});
    if (best_gpu > 0) {
        if (p.atc_count == best_gpu)        p.gpu_format = DepotGpuFormat::Atc;
        else if (p.etc_count == best_gpu)   p.gpu_format = DepotGpuFormat::Etc;
        else if (p.dxt_count == best_gpu)   p.gpu_format = DepotGpuFormat::Dxt;
        else                                p.gpu_format = DepotGpuFormat::Pvrtc;
    }
    return p;
}

bool EnsureApkResExtracted(const std::filesystem::path& apk_path,
                           const std::filesystem::path& readonly_files_dir,
                           const std::filesystem::path& userdata_dir,
                           std::string& out_status) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(readonly_files_dir / "apk" / "res" / "asset_list_base.txt", ec)) {
        out_status = "APK res/ already present in game data folder.";
        return true;
    }
    if (!userdata_dir.empty() &&
        std::filesystem::is_regular_file(userdata_dir / "apk" / "res" / "asset_list_base.txt", ec)) {
        out_status = "APK res/ already unpacked in userdata/apk/res.";
        return true;
    }
    if (apk_path.empty() || !std::filesystem::is_regular_file(apk_path, ec)) {
        out_status = "Missing files/apk/res/ and no APK found to extract res/xR.zip.";
        return false;
    }
    if (userdata_dir.empty()) {
        out_status = "No writable userdata_dir available for APK res/ extraction.";
        return false;
    }

    std::ifstream in(apk_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        out_status = "Could not open APK for res/ extraction.";
        return false;
    }
    const int64_t file_size = static_cast<int64_t>(in.tellg());
    if (file_size < 22) {
        out_status = "Invalid APK size.";
        return false;
    }

    const size_t search_len = static_cast<size_t>(std::min<int64_t>(file_size, 65557));
    std::vector<uint8_t> tail(search_len);
    in.seekg(file_size - static_cast<int64_t>(search_len), std::ios::beg);
    in.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(search_len));

    int64_t eocd_pos = -1;
    if (!FindZipEocd(tail.data(), tail.size(), eocd_pos)) {
        out_status = "Invalid APK: ZIP EOCD not found.";
        return false;
    }

    const uint16_t total_entries = ReadLe16(tail.data() + eocd_pos + 10);
    const uint32_t cd_size = ReadLe32(tail.data() + eocd_pos + 12);
    const uint32_t cd_offset = ReadLe32(tail.data() + eocd_pos + 16);
    if (static_cast<int64_t>(cd_offset) + static_cast<int64_t>(cd_size) > file_size) {
        out_status = "Invalid APK: Central Directory out of bounds.";
        return false;
    }

    std::vector<uint8_t> cd(cd_size);
    in.seekg(cd_offset, std::ios::beg);
    in.read(reinterpret_cast<char*>(cd.data()), static_cast<std::streamsize>(cd_size));
    if (!in.good()) {
        out_status = "Failed reading APK Central Directory.";
        return false;
    }

    // Locate embedded inner resource zip (res/xR.zip in RR3 v14.0.1, or fallback res/*.zip >= 1 MB)
    struct InnerZipHit {
        std::string name;
        uint16_t method = 0;
        uint32_t comp_sz = 0;
        uint32_t uncomp_sz = 0;
        uint32_t lfh_off = 0;
    };
    InnerZipHit hit;
    bool found_exact = false;
    size_t pos = 0;
    for (uint16_t idx = 0; idx < total_entries && pos + 46 <= cd.size(); ++idx) {
        if (ReadLe32(cd.data() + pos) != 0x02014b50u) break;
        const uint16_t method = ReadLe16(cd.data() + pos + 10);
        const uint32_t comp_sz = ReadLe32(cd.data() + pos + 20);
        const uint32_t uncomp_sz = ReadLe32(cd.data() + pos + 24);
        const uint16_t name_len = ReadLe16(cd.data() + pos + 28);
        const uint16_t extra_len = ReadLe16(cd.data() + pos + 30);
        const uint16_t comment_len = ReadLe16(cd.data() + pos + 32);
        const uint32_t lfh_off = ReadLe32(cd.data() + pos + 42);
        if (pos + 46 + name_len > cd.size()) break;

        std::string entry_name(reinterpret_cast<const char*>(cd.data() + pos + 46), name_len);
        pos += 46 + name_len + extra_len + comment_len;

        if (entry_name == "res/xR.zip") {
            hit = {entry_name, method, comp_sz, uncomp_sz, lfh_off};
            found_exact = true;
            break;
        }
        if (!found_exact && entry_name.rfind("res/", 0) == 0 &&
            entry_name.size() > 8 && entry_name.compare(entry_name.size() - 4, 4, ".zip") == 0 &&
            uncomp_sz >= 1000000u && uncomp_sz > hit.uncomp_sz) {
            hit = {entry_name, method, comp_sz, uncomp_sz, lfh_off};
        }
    }

    if (hit.name.empty()) {
        out_status = "No embedded resource zip (res/xR.zip) found in APK.";
        return false;
    }

    uint8_t lfh[30]{};
    in.seekg(hit.lfh_off, std::ios::beg);
    in.read(reinterpret_cast<char*>(lfh), sizeof(lfh));
    if (!in.good() || ReadLe32(lfh) != 0x04034b50u) {
        out_status = "Corrupt ZIP header for " + hit.name;
        return false;
    }
    const int64_t data_off = static_cast<int64_t>(hit.lfh_off) + 30 + ReadLe16(lfh + 26) + ReadLe16(lfh + 28);
    if (data_off + hit.comp_sz > file_size) {
        out_status = "Corrupt ZIP bounds for " + hit.name;
        return false;
    }

    std::vector<uint8_t> raw(hit.comp_sz);
    in.seekg(data_off, std::ios::beg);
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(hit.comp_sz));
    if (!in.good()) {
        out_status = "Failed reading " + hit.name + " from APK.";
        return false;
    }

    std::vector<uint8_t> inner_zip;
    const uint8_t* zip_ptr = nullptr;
    size_t zip_len = 0;
    if (hit.method == 0) {
        zip_ptr = raw.data();
        zip_len = raw.size();
    } else if (hit.method == 8) {
        if (!InflateRaw(raw.data(), raw.size(), inner_zip, hit.uncomp_sz)) {
            out_status = "Failed inflating " + hit.name;
            return false;
        }
        zip_ptr = inner_zip.data();
        zip_len = inner_zip.size();
    } else {
        out_status = "Unsupported ZIP compression method for " + hit.name;
        return false;
    }

    size_t extracted_count = 0;
    std::string err;
    const auto apk_out = userdata_dir / "apk";
    if (!ExtractZipBufferToDir(zip_ptr, zip_len, apk_out, extracted_count, err)) {
        out_status = err;
        return false;
    }

    out_status = "Unpacked " + std::to_string(extracted_count) + " APK resource files from " +
                 hit.name + " into userdata/apk/res.";
    return true;
}

std::string ComputeFileSha256Hex(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    DWORD obj_len = 0, cb = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &cb, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    std::vector<uint8_t> hash_obj(obj_len);
    if (BCryptCreateHash(alg, &hash, hash_obj.data(), obj_len, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    std::vector<char> buf(256 * 1024);
    while (in.good()) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = in.gcount();
        if (got > 0) {
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(got), 0) != 0) {
                BCryptDestroyHash(hash);
                BCryptCloseAlgorithmProvider(alg, 0);
                return {};
            }
        }
    }

    std::array<uint8_t, 32> digest{};
    bool ok = (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : digest) {
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

bool IsCompatibleRr3_14_0_1So(const std::filesystem::path& so_path) {
    std::ifstream in(so_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    const int64_t fsz = static_cast<int64_t>(in.tellg());
    if (fsz < 0x1300000) return false;

    uint8_t ehdr[64]{};
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(ehdr), sizeof(ehdr));
    if (!in.good() || ehdr[0] != 0x7F || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F' || ehdr[4] != 2) {
        return false;
    }
    uint64_t e_phoff = 0;
    std::memcpy(&e_phoff, ehdr + 32, 8);
    const uint16_t e_phentsize = ReadLe16(ehdr + 54);
    const uint16_t e_phnum = ReadLe16(ehdr + 56);
    if (e_phentsize < 56 || e_phnum == 0 || e_phnum > 64) return false;

    struct LoadSeg { uint64_t vstart, vend, foff; };
    std::vector<LoadSeg> segs;
    for (uint16_t i = 0; i < e_phnum; ++i) {
        uint8_t ph[56]{};
        in.seekg(static_cast<std::streamoff>(e_phoff + static_cast<uint64_t>(i) * e_phentsize), std::ios::beg);
        in.read(reinterpret_cast<char*>(ph), sizeof(ph));
        if (!in.good()) return false;
        if (ReadLe32(ph) == 1u) { // PT_LOAD
            uint64_t p_off = 0, p_va = 0, p_fsz = 0;
            std::memcpy(&p_off, ph + 8, 8);
            std::memcpy(&p_va, ph + 16, 8);
            std::memcpy(&p_fsz, ph + 32, 8);
            segs.push_back({p_va, p_va + p_fsz, p_off});
        }
    }

    auto read_va_u32 = [&](uint64_t va, uint32_t& out) -> bool {
        for (const auto& s : segs) {
            if (va >= s.vstart && va + 4 <= s.vend) {
                uint64_t fo = s.foff + (va - s.vstart);
                if (static_cast<int64_t>(fo + 4) > fsz) return false;
                uint8_t buf[4]{};
                in.seekg(static_cast<std::streamoff>(fo), std::ios::beg);
                in.read(reinterpret_cast<char*>(buf), 4);
                if (!in.good()) return false;
                out = ReadLe32(buf);
                return true;
            }
        }
        return false;
    };

    static constexpr std::pair<uint64_t, uint32_t> kExpectedInsns[] = {
        {0x0df981cULL, 0x7100041fu}, // cmp w0, #1 (gamepad cursor)
        {0x12ae40cULL, 0xb50017a8u}, // cbnz x8, ... (live resize)
        {0x12ad664ULL, 0x2a0003e1u}, // mov w1, w0 (quality tier caller 1)
        {0x12adbd4ULL, 0x2a0003e1u}, // mov w1, w0 (quality tier caller 2)
        {0x12af0d0ULL, 0x52800a49u}, // menu usleep anchor
    };
    for (const auto& [va, expect] : kExpectedInsns) {
        uint32_t got = 0;
        if (!read_va_u32(va, got) || got != expect) return false;
    }
    return true;
}

GameDataStatus CheckGameDataStatus(const PcConfig& cfg) {
    GameDataStatus st;
    std::error_code ec;

    const std::filesystem::path bins(cfg.work_bins_dir);
    const auto p_rr3 = bins / "libRealRacing3.so";
    const auto p_cpp = bins / "libc++_shared.so";
    const auto p_fmod = bins / "libfmodex.so";
    st.has_libs = std::filesystem::is_regular_file(p_rr3, ec) &&
                  std::filesystem::is_regular_file(p_cpp, ec) &&
                  std::filesystem::is_regular_file(p_fmod, ec);
    if (st.has_libs) {
        st.rr3_sha256 = ComputeFileSha256Hex(p_rr3);
        st.hash_matches = (st.rr3_sha256 == kExpectedRr3SoSha256);
        st.is_patched_14_0_1 = !st.hash_matches && IsCompatibleRr3_14_0_1So(p_rr3);
    }

    const std::filesystem::path ro(cfg.readonly_files_dir);
    st.has_depot = std::filesystem::is_directory(ro / ".depot", ec);
    if (st.has_depot) {
        st.depot_profile = DetectDepotProfile(ro, std::filesystem::path(cfg.userdata_dir));
    }

    const std::filesystem::path doc = std::filesystem::path(cfg.userdata_dir) / "doc";
    uintmax_t bytes = 0;
    CountFilesInDir(doc, st.save_file_count, bytes);
    st.has_save_data = (st.save_file_count > 0);

    std::ostringstream oss;
    if (st.has_libs && (st.hash_matches || st.is_patched_14_0_1) && st.has_depot) {
        oss << "Ready to play (RR3 v14.0.1"
            << (st.is_patched_14_0_1 ? " modded" : "")
            << " · " << DepotGpuFormatToString(st.depot_profile.gpu_format)
            << " · UI " << DepotGuiTierToDirName(st.depot_profile.gui_tier) << ")";
    } else if (!st.has_libs && !st.has_depot) {
        oss << "Setup required: select your Real Racing 3 APK to extract libraries and select com.ea.games.r3_row data folder.";
    } else if (!st.has_libs) {
        oss << "Missing ARM64 libraries: browse for Real Racing 3 APK and click Extract.";
    } else if (!st.hash_matches) {
        oss << "Warning: libRealRacing3.so SHA-256 differs from v14.0.1 (" << st.rr3_sha256.substr(0, 12) << "...).";
        if (!st.has_depot) oss << " Also missing .depot game data folder.";
    } else {
        oss << "Libraries OK, but .depot folder was not found in Game Data path.";
    }
    st.summary = oss.str();
    return st;
}

bool ExtractArm64LibsFromApk(const std::filesystem::path& apk_path,
                             const std::filesystem::path& out_arm64_dir,
                             std::string& out_status) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(apk_path, ec)) {
        out_status = "APK file not found: " + apk_path.string();
        return false;
    }

    std::ifstream in(apk_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        out_status = "Could not open APK file for reading.";
        return false;
    }
    const int64_t file_size = static_cast<int64_t>(in.tellg());
    if (file_size < 22) {
        out_status = "File is too small to be a valid APK/ZIP archive.";
        return false;
    }

    // Locate ZIP End of Central Directory record (0x06054b50) within last 66KB
    const size_t search_len = static_cast<size_t>(std::min<int64_t>(file_size, 65557));
    std::vector<uint8_t> tail(search_len);
    in.seekg(file_size - static_cast<int64_t>(search_len), std::ios::beg);
    in.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(search_len));

    int64_t eocd_pos = -1;
    for (int64_t i = static_cast<int64_t>(search_len) - 22; i >= 0; --i) {
        if (ReadLe32(tail.data() + i) == 0x06054b50u) {
            uint16_t comment_len = ReadLe16(tail.data() + i + 20);
            if (i + 22 + comment_len == static_cast<int64_t>(search_len)) {
                eocd_pos = i;
                break;
            }
        }
    }
    if (eocd_pos < 0) {
        out_status = "Invalid APK: ZIP Central Directory header not found.";
        return false;
    }

    const uint16_t total_entries = ReadLe16(tail.data() + eocd_pos + 10);
    const uint32_t cd_size = ReadLe32(tail.data() + eocd_pos + 12);
    const uint32_t cd_offset = ReadLe32(tail.data() + eocd_pos + 16);
    if (static_cast<int64_t>(cd_offset) + static_cast<int64_t>(cd_size) > file_size) {
        out_status = "Invalid APK: Central Directory extends past EOF.";
        return false;
    }

    std::vector<uint8_t> cd(cd_size);
    in.seekg(cd_offset, std::ios::beg);
    in.read(reinterpret_cast<char*>(cd.data()), static_cast<std::streamsize>(cd_size));
    if (!in.good()) {
        out_status = "Failed reading ZIP Central Directory.";
        return false;
    }

    const std::set<std::string> wanted = {
        "lib/arm64-v8a/libRealRacing3.so",
        "lib/arm64-v8a/libc++_shared.so",
        "lib/arm64-v8a/libfmodex.so",
        "lib/arm64-v8a/libNimble.so",
    };

    std::filesystem::create_directories(out_arm64_dir, ec);
    if (ec) {
        out_status = "Failed creating output directory: " + out_arm64_dir.string();
        return false;
    }

    std::set<std::string> extracted;
    size_t pos = 0;
    for (uint16_t idx = 0; idx < total_entries && pos + 46 <= cd.size(); ++idx) {
        if (ReadLe32(cd.data() + pos) != 0x02014b50u) break;
        const uint16_t method = ReadLe16(cd.data() + pos + 10);
        const uint32_t comp_size = ReadLe32(cd.data() + pos + 20);
        const uint32_t uncomp_size = ReadLe32(cd.data() + pos + 24);
        const uint16_t name_len = ReadLe16(cd.data() + pos + 28);
        const uint16_t extra_len = ReadLe16(cd.data() + pos + 30);
        const uint16_t comment_len = ReadLe16(cd.data() + pos + 32);
        const uint32_t local_offset = ReadLe32(cd.data() + pos + 42);

        if (pos + 46 + name_len > cd.size()) break;
        std::string entry_name(reinterpret_cast<const char*>(cd.data() + pos + 46), name_len);
        pos += 46 + name_len + extra_len + comment_len;

        if (!wanted.count(entry_name)) continue;

        uint8_t lfh[30]{};
        in.seekg(local_offset, std::ios::beg);
        in.read(reinterpret_cast<char*>(lfh), sizeof(lfh));
        if (!in.good() || ReadLe32(lfh) != 0x04034b50u) {
            out_status = "Corrupt ZIP local file header for " + entry_name;
            return false;
        }
        const uint16_t lfh_name_len = ReadLe16(lfh + 26);
        const uint16_t lfh_extra_len = ReadLe16(lfh + 28);
        const int64_t data_offset = static_cast<int64_t>(local_offset) + 30 + lfh_name_len + lfh_extra_len;
        if (data_offset + comp_size > file_size) {
            out_status = "Corrupt ZIP payload bounds for " + entry_name;
            return false;
        }

        std::vector<uint8_t> raw(comp_size);
        in.seekg(data_offset, std::ios::beg);
        in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(comp_size));
        if (!in.good()) {
            out_status = "Failed reading compressed bytes for " + entry_name;
            return false;
        }

        std::vector<uint8_t> decoded;
        const uint8_t* write_ptr = nullptr;
        size_t write_len = 0;
        if (method == 0) {
            write_ptr = raw.data();
            write_len = raw.size();
        } else if (method == 8) {
            if (!InflateRaw(raw.data(), raw.size(), decoded, uncomp_size)) {
                out_status = "zlib inflate failed for " + entry_name;
                return false;
            }
            write_ptr = decoded.data();
            write_len = decoded.size();
        } else {
            out_status = "Unsupported ZIP compression method (" + std::to_string(method) + ") for " + entry_name;
            return false;
        }

        const auto leaf = std::filesystem::path(entry_name).filename();
        const auto dst = out_arm64_dir / leaf;
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            out_status = "Could not write " + dst.string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(write_ptr), static_cast<std::streamsize>(write_len));
        if (!out.good()) {
            out_status = "Disk write error while extracting " + leaf.string();
            return false;
        }
        extracted.insert(leaf.string());
    }

    if (!extracted.count("libRealRacing3.so") ||
        !extracted.count("libc++_shared.so") ||
        !extracted.count("libfmodex.so")) {
        out_status = "APK is missing required ARM64 libraries (lib/arm64-v8a/libRealRacing3.so, libc++_shared.so, libfmodex.so).";
        return false;
    }

    const std::string sha = ComputeFileSha256Hex(out_arm64_dir / "libRealRacing3.so");
    if (sha == kExpectedRr3SoSha256) {
        out_status = "Extracted " + std::to_string(extracted.size()) +
                     " ARM64 libraries. Verified RR3 v14.0.1 SHA-256 (" + sha.substr(0, 12) + "...).";
    } else if (IsCompatibleRr3_14_0_1So(out_arm64_dir / "libRealRacing3.so")) {
        out_status = "Extracted " + std::to_string(extracted.size()) +
                     " ARM64 libraries. Detected compatible patched RR3 v14.0.1 build (" + sha.substr(0, 12) + "...).";
    } else {
        out_status = "Extracted " + std::to_string(extracted.size()) +
                     " ARM64 libraries, but libRealRacing3.so SHA-256 (" + sha.substr(0, 12) +
                     "...) does not match tested v14.0.1.";
    }
    return true;
}

bool NormalizeGameDataDir(const std::filesystem::path& chosen_dir,
                          std::filesystem::path& out_files_dir,
                          std::string& out_status) {
    std::error_code ec;
    if (!std::filesystem::is_directory(chosen_dir, ec)) {
        out_status = "Selected folder does not exist.";
        return false;
    }
    if (std::filesystem::is_directory(chosen_dir / ".depot", ec)) {
        out_files_dir = chosen_dir.lexically_normal();
        out_status = "Valid RR3 game data folder (.depot found).";
        return true;
    }
    if (std::filesystem::is_directory(chosen_dir / "files" / ".depot", ec)) {
        out_files_dir = (chosen_dir / "files").lexically_normal();
        out_status = "Valid RR3 game data folder (normalized to files/).";
        return true;
    }
    if (chosen_dir.filename() == ".depot" && chosen_dir.has_parent_path()) {
        out_files_dir = chosen_dir.parent_path().lexically_normal();
        out_status = "Valid RR3 game data folder (normalized from .depot).";
        return true;
    }
    out_status = "Selected folder does not contain a .depot subfolder. Choose com.ea.games.r3_row or its files/ folder.";
    return false;
}

bool ExportUserData(const std::filesystem::path& userdata_dir,
                    const std::filesystem::path& dest_dir,
                    std::string& out_status) {
    std::error_code ec;
    const auto src_doc = userdata_dir / "doc";
    if (!std::filesystem::is_directory(src_doc, ec)) {
        out_status = "No user save data folder found at " + src_doc.string();
        return false;
    }
    size_t count = 0;
    uintmax_t bytes = 0;
    CountFilesInDir(src_doc, count, bytes);
    if (count == 0) {
        out_status = "User save data folder is empty (nothing to export yet).";
        return false;
    }

    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        out_status = "Could not create export destination: " + dest_dir.string();
        return false;
    }

    std::filesystem::copy(src_doc, dest_dir,
                          std::filesystem::copy_options::recursive |
                          std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
        out_status = "Export failed: " + ec.message();
        return false;
    }

    char buf[256]{};
    std::snprintf(buf, sizeof(buf), "Exported %zu save file(s) (%.1f KB) to %s",
                  count, static_cast<double>(bytes) / 1024.0, dest_dir.string().c_str());
    out_status = buf;
    return true;
}

bool ImportUserData(const std::filesystem::path& src_path,
                    const std::filesystem::path& userdata_dir,
                    std::string& out_status) {
    std::error_code ec;
    if (!std::filesystem::exists(src_path, ec)) {
        out_status = "Import source not found: " + src_path.string();
        return false;
    }

    const auto dst_doc = userdata_dir / "doc";
    std::filesystem::path resolved_src = src_path;
    bool single_file = std::filesystem::is_regular_file(src_path, ec);

    if (!single_file) {
        if (std::filesystem::is_directory(src_path / "files" / "doc", ec)) {
            resolved_src = src_path / "files" / "doc";
        } else if (std::filesystem::is_directory(src_path / "doc", ec)) {
            resolved_src = src_path / "doc";
        }
        size_t src_count = 0;
        uintmax_t src_bytes = 0;
        CountFilesInDir(resolved_src, src_count, src_bytes);
        if (src_count == 0) {
            out_status = "Selected folder contains no save files to import.";
            return false;
        }
        // Prevent self-copy corruption
        auto canon_src = std::filesystem::weakly_canonical(resolved_src, ec);
        auto canon_dst = std::filesystem::weakly_canonical(dst_doc, ec);
        if (canon_src == canon_dst) {
            out_status = "Source folder is already the active userdata/doc folder.";
            return false;
        }
    }

    // Auto-backup existing save data before overwriting
    std::string backup_note;
    size_t existing_count = 0;
    uintmax_t existing_bytes = 0;
    if (CountFilesInDir(dst_doc, existing_count, existing_bytes) && existing_count > 0) {
        auto backup_dir = userdata_dir / ("doc_backup_" + MakeTimestampSuffix());
        std::filesystem::create_directories(backup_dir, ec);
        std::filesystem::copy(dst_doc, backup_dir,
                              std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                              ec);
        if (ec) {
            out_status = "Aborted import: failed to create safety backup (" + ec.message() + ").";
            return false;
        }
        backup_note = " Auto-backup saved to " + backup_dir.filename().string() + ".";
    }

    std::filesystem::create_directories(dst_doc, ec);
    if (ec) {
        out_status = "Failed creating userdata/doc directory: " + ec.message();
        return false;
    }

    size_t imported_files = 0;
    if (single_file) {
        std::filesystem::copy_file(resolved_src, dst_doc / resolved_src.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            out_status = "Failed importing file: " + ec.message();
            return false;
        }
        imported_files = 1;
    } else {
        uintmax_t imported_bytes = 0;
        CountFilesInDir(resolved_src, imported_files, imported_bytes);
        std::filesystem::copy(resolved_src, dst_doc,
                              std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                              ec);
        if (ec) {
            out_status = "Import failed: " + ec.message();
            return false;
        }
    }

    out_status = "Imported " + std::to_string(imported_files) + " save file(s) into userdata/doc." + backup_note;
    return true;
}

namespace {

void WriteLe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void WriteLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

bool DeflateRaw(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    out.resize(deflateBound(&zs, static_cast<uLong>(in.size())) + 16);
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&zs, Z_FINISH);
    size_t produced = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) return false;
    out.resize(produced);
    return true;
}

bool HasLowerExt(const std::filesystem::path& p, const char* ext) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ext;
}

bool ExtractSaveZipToDoc(const std::filesystem::path& zip_path,
                         const std::filesystem::path& dst_doc,
                         size_t& out_extracted,
                         std::string& out_err) {
    std::ifstream in(zip_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) { out_err = "Could not open backup zip."; return false; }
    const int64_t fsz = static_cast<int64_t>(in.tellg());
    if (fsz < 22) { out_err = "Invalid zip file."; return false; }

    const size_t search_len = static_cast<size_t>(std::min<int64_t>(fsz, 65557));
    std::vector<uint8_t> tail(search_len);
    in.seekg(fsz - static_cast<int64_t>(search_len), std::ios::beg);
    in.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(search_len));

    int64_t eocd = -1;
    for (int64_t i = static_cast<int64_t>(search_len) - 22; i >= 0; --i) {
        if (ReadLe32(tail.data() + i) == 0x06054b50u &&
            i + 22 + ReadLe16(tail.data() + i + 20) == static_cast<int64_t>(search_len)) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) { out_err = "Invalid zip archive (missing EOCD)."; return false; }

    const uint16_t count = ReadLe16(tail.data() + eocd + 10);
    const uint32_t cd_sz = ReadLe32(tail.data() + eocd + 12);
    const uint32_t cd_off = ReadLe32(tail.data() + eocd + 16);
    if (static_cast<int64_t>(cd_off) + cd_sz > fsz) { out_err = "Corrupt zip directory."; return false; }

    std::vector<uint8_t> cd(cd_sz);
    in.seekg(cd_off, std::ios::beg);
    in.read(reinterpret_cast<char*>(cd.data()), static_cast<std::streamsize>(cd_sz));

    std::error_code ec;
    const auto canon_root = std::filesystem::weakly_canonical(dst_doc, ec);
    size_t pos = 0;
    for (uint16_t idx = 0; idx < count && pos + 46 <= cd.size(); ++idx) {
        if (ReadLe32(cd.data() + pos) != 0x02014b50u) break;
        const uint16_t method = ReadLe16(cd.data() + pos + 10);
        const uint32_t comp_sz = ReadLe32(cd.data() + pos + 20);
        const uint32_t uncomp_sz = ReadLe32(cd.data() + pos + 24);
        const uint16_t name_len = ReadLe16(cd.data() + pos + 28);
        const uint16_t extra_len = ReadLe16(cd.data() + pos + 30);
        const uint16_t comment_len = ReadLe16(cd.data() + pos + 32);
        const uint32_t lfh_off = ReadLe32(cd.data() + pos + 42);
        if (pos + 46 + name_len > cd.size()) break;

        std::string name(reinterpret_cast<const char*>(cd.data() + pos + 46), name_len);
        pos += 46 + name_len + extra_len + comment_len;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty() || name.back() == '/') continue;

        // Strip common archive prefixes like files/doc/ or doc/
        if (name.rfind("files/doc/", 0) == 0) name = name.substr(10);
        else if (name.rfind("doc/", 0) == 0) name = name.substr(4);
        while (!name.empty() && name.front() == '/') name.erase(name.begin());
        if (name.empty() || name.find("..") != std::string::npos || name.find(':') != std::string::npos) {
            continue; // Reject unsafe traversal paths
        }

        uint8_t lfh[30]{};
        in.seekg(lfh_off, std::ios::beg);
        in.read(reinterpret_cast<char*>(lfh), sizeof(lfh));
        if (!in.good() || ReadLe32(lfh) != 0x04034b50u) continue;
        const int64_t data_off = static_cast<int64_t>(lfh_off) + 30 + ReadLe16(lfh + 26) + ReadLe16(lfh + 28);
        if (data_off + comp_sz > fsz) continue;

        std::vector<uint8_t> raw(comp_sz);
        in.seekg(data_off, std::ios::beg);
        in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(comp_sz));

        std::vector<uint8_t> decoded;
        const uint8_t* wptr = nullptr;
        size_t wlen = 0;
        if (method == 0) {
            wptr = raw.data();
            wlen = raw.size();
        } else if (method == 8) {
            if (!InflateRaw(raw.data(), raw.size(), decoded, uncomp_sz)) continue;
            wptr = decoded.data();
            wlen = decoded.size();
        } else {
            continue;
        }

        const auto dst = (dst_doc / std::filesystem::path(name)).lexically_normal();
        const auto canon_dst = std::filesystem::weakly_canonical(dst, ec);
        if (canon_dst.wstring().rfind(canon_root.wstring(), 0) != 0) continue;

        if (dst.has_parent_path()) std::filesystem::create_directories(dst.parent_path(), ec);
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) continue;
        out.write(reinterpret_cast<const char*>(wptr), static_cast<std::streamsize>(wlen));
        if (out.good()) ++out_extracted;
    }
    if (out_extracted == 0) {
        out_err = "Zip archive contained no valid save files.";
        return false;
    }
    return true;
}

} // namespace

bool ExportSaveFile(const std::filesystem::path& userdata_dir,
                    const std::filesystem::path& dest_file,
                    std::string& out_status) {
    std::error_code ec;
    const auto src_doc = userdata_dir / "doc";
    if (!std::filesystem::is_directory(src_doc, ec)) {
        out_status = "No save data folder found at " + src_doc.string();
        return false;
    }
    if (dest_file.has_parent_path()) {
        std::filesystem::create_directories(dest_file.parent_path(), ec);
    }

    if (!HasLowerExt(dest_file, ".zip")) {
        const auto primary_save = src_doc / "character.2.dat";
        if (!std::filesystem::is_regular_file(primary_save, ec)) {
            out_status = "No character.2.dat save file found yet.";
            return false;
        }
        std::filesystem::copy_file(primary_save, dest_file,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            out_status = "Failed exporting save: " + ec.message();
            return false;
        }
        auto sz = std::filesystem::file_size(dest_file, ec);
        char buf[256]{};
        std::snprintf(buf, sizeof(buf), "Exported character.2.dat (%.1f KB) to %s",
                      static_cast<double>(sz) / 1024.0, dest_file.string().c_str());
        out_status = buf;
        return true;
    }

    // Export full userdata/doc/ as a standard portable .zip archive
    struct CdEntry {
        std::string name;
        uint32_t crc;
        uint32_t comp_sz;
        uint32_t uncomp_sz;
        uint16_t method;
        uint32_t lfh_off;
    };
    std::vector<uint8_t> zip;
    std::vector<CdEntry> entries;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(src_doc, ec)) {
        if (ec || !entry.is_regular_file(ec)) continue;
        auto rel = std::filesystem::relative(entry.path(), src_doc, ec).generic_string();
        if (ec || rel.empty()) continue;

        std::ifstream f(entry.path(), std::ios::binary);
        if (!f.is_open()) continue;
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), {});
        uint32_t crc = static_cast<uint32_t>(crc32(0L, raw.data(), static_cast<uInt>(raw.size())));

        std::vector<uint8_t> comp;
        uint16_t method = 8;
        const std::vector<uint8_t>* payload = &comp;
        if (!DeflateRaw(raw, comp) || comp.size() >= raw.size()) {
            method = 0;
            payload = &raw;
        }

        const uint32_t lfh_off = static_cast<uint32_t>(zip.size());
        WriteLe32(zip, 0x04034b50u);
        WriteLe16(zip, 20);
        WriteLe16(zip, 0);
        WriteLe16(zip, method);
        WriteLe16(zip, 0);
        WriteLe16(zip, 0);
        WriteLe32(zip, crc);
        WriteLe32(zip, static_cast<uint32_t>(payload->size()));
        WriteLe32(zip, static_cast<uint32_t>(raw.size()));
        WriteLe16(zip, static_cast<uint16_t>(rel.size()));
        WriteLe16(zip, 0);
        zip.insert(zip.end(), rel.begin(), rel.end());
        zip.insert(zip.end(), payload->begin(), payload->end());

        entries.push_back({rel, crc, static_cast<uint32_t>(payload->size()),
                           static_cast<uint32_t>(raw.size()), method, lfh_off});
    }

    if (entries.empty()) {
        out_status = "No save files found to export.";
        return false;
    }

    const uint32_t cd_off = static_cast<uint32_t>(zip.size());
    for (const auto& e : entries) {
        WriteLe32(zip, 0x02014b50u);
        WriteLe16(zip, 20);
        WriteLe16(zip, 20);
        WriteLe16(zip, 0);
        WriteLe16(zip, e.method);
        WriteLe16(zip, 0);
        WriteLe16(zip, 0);
        WriteLe32(zip, e.crc);
        WriteLe32(zip, e.comp_sz);
        WriteLe32(zip, e.uncomp_sz);
        WriteLe16(zip, static_cast<uint16_t>(e.name.size()));
        WriteLe16(zip, 0);
        WriteLe16(zip, 0);
        WriteLe16(zip, 0);
        WriteLe16(zip, 0);
        WriteLe32(zip, 0);
        WriteLe32(zip, e.lfh_off);
        zip.insert(zip.end(), e.name.begin(), e.name.end());
    }
    const uint32_t cd_sz = static_cast<uint32_t>(zip.size()) - cd_off;

    WriteLe32(zip, 0x06054b50u);
    WriteLe16(zip, 0);
    WriteLe16(zip, 0);
    WriteLe16(zip, static_cast<uint16_t>(entries.size()));
    WriteLe16(zip, static_cast<uint16_t>(entries.size()));
    WriteLe32(zip, cd_sz);
    WriteLe32(zip, cd_off);
    WriteLe16(zip, 0);

    std::ofstream out(dest_file, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        out_status = "Could not write " + dest_file.string();
        return false;
    }
    out.write(reinterpret_cast<const char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
    if (!out.good()) {
        out_status = "Disk error writing backup zip.";
        return false;
    }

    char buf[256]{};
    std::snprintf(buf, sizeof(buf), "Exported %zu save file(s) (%.1f KB) to %s",
                  entries.size(), static_cast<double>(zip.size()) / 1024.0, dest_file.string().c_str());
    out_status = buf;
    return true;
}

bool ImportSaveFiles(const std::vector<std::filesystem::path>& src_files,
                     const std::filesystem::path& userdata_dir,
                     std::string& out_status) {
    if (src_files.empty()) {
        out_status = "No save files selected.";
        return false;
    }
    std::error_code ec;
    const auto dst_doc = userdata_dir / "doc";

    // Auto-backup existing save folder first
    std::string backup_note;
    size_t existing_count = 0;
    uintmax_t existing_bytes = 0;
    if (CountFilesInDir(dst_doc, existing_count, existing_bytes) && existing_count > 0) {
        auto backup_dir = userdata_dir / ("doc_backup_" + MakeTimestampSuffix());
        std::filesystem::create_directories(backup_dir, ec);
        std::filesystem::copy(dst_doc, backup_dir,
                              std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            out_status = "Aborted import: could not create backup (" + ec.message() + ").";
            return false;
        }
        backup_note = " Backup saved to " + backup_dir.filename().string() + ".";
    }

    std::filesystem::create_directories(dst_doc, ec);
    size_t imported = 0;
    for (const auto& src : src_files) {
        if (!std::filesystem::is_regular_file(src, ec)) continue;
        if (HasLowerExt(src, ".zip")) {
            size_t n = 0;
            std::string err;
            if (!ExtractSaveZipToDoc(src, dst_doc, n, err)) {
                out_status = err;
                return false;
            }
            imported += n;
        } else {
            auto dst = dst_doc / src.filename();
            auto c_src = std::filesystem::weakly_canonical(src, ec);
            auto c_dst = std::filesystem::weakly_canonical(dst, ec);
            if (c_src != c_dst) {
                std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    out_status = "Failed copying " + src.filename().string() + ": " + ec.message();
                    return false;
                }
            }
            ++imported;
        }
    }
    if (imported == 0) {
        out_status = "No valid save files were imported.";
        return false;
    }
    out_status = "Imported " + std::to_string(imported) + " save file(s)." + backup_note;
    return true;
}
