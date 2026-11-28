#include "updater.h"

#include <windows.h>
#include <winhttp.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string StripLeadingV(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.erase(s.begin());
    return s;
}

std::array<int, 3> ParseVersionTriplet(const std::string& v) {
    std::string clean = StripLeadingV(v);
    std::array<int, 3> out{0, 0, 0};
    size_t pos = 0;
    for (int idx = 0; idx < 3 && pos < clean.size(); ++idx) {
        size_t end = pos;
        while (end < clean.size() && std::isdigit(static_cast<unsigned char>(clean[end]))) ++end;
        if (end > pos) {
            try { out[idx] = std::stoi(clean.substr(pos, end - pos)); } catch (...) {}
        }
        if (end >= clean.size() || clean[end] != '.') break;
        pos = end + 1;
    }
    return out;
}

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool InflateRaw(const uint8_t* comp, size_t comp_size, std::vector<uint8_t>& out, size_t expected_size) {
    out.assign(expected_size, 0);
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(comp));
    zs.avail_in = static_cast<uInt>(comp_size);
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = inflate(&zs, Z_FINISH);
    size_t produced = zs.total_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_OK) return false;
    out.resize(produced);
    return true;
}

// ---- Minimal strict JSON scanner for GitHub Releases API ----
void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

bool ParseJsonString(const std::string& s, size_t& i, std::string& out) {
    SkipWs(s, i);
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c == '\\' && i < s.size()) {
            char esc = s[i++];
            switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u':
                    if (i + 4 <= s.size()) {
                        unsigned code = 0;
                        std::string hex = s.substr(i, 4);
                        i += 4;
                        try { code = static_cast<unsigned>(std::stoul(hex, nullptr, 16)); } catch (...) { code = '?'; }
                        if (code < 0x80) {
                            out.push_back(static_cast<char>(code));
                        } else if (code < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                        }
                    }
                    break;
                default:
                    out.push_back(esc);
                    break;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

bool ParseJsonU64(const std::string& s, size_t& i, uint64_t& out) {
    SkipWs(s, i);
    size_t start = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i == start) return false;
    try { out = std::stoull(s.substr(start, i - start)); return true; } catch (...) { return false; }
}

void SkipJsonValue(const std::string& s, size_t& i);

void SkipJsonObjectOrArray(const std::string& s, size_t& i, char open_ch, char close_ch) {
    if (i >= s.size() || s[i] != open_ch) return;
    ++i;
    int depth = 1;
    while (i < s.size() && depth > 0) {
        if (s[i] == '"') {
            std::string dummy;
            if (!ParseJsonString(s, i, dummy)) break;
            continue;
        }
        if (s[i] == open_ch) ++depth;
        else if (s[i] == close_ch) --depth;
        ++i;
    }
}

void SkipJsonValue(const std::string& s, size_t& i) {
    SkipWs(s, i);
    if (i >= s.size()) return;
    if (s[i] == '"') {
        std::string dummy;
        ParseJsonString(s, i, dummy);
    } else if (s[i] == '{') {
        SkipJsonObjectOrArray(s, i, '{', '}');
    } else if (s[i] == '[') {
        SkipJsonObjectOrArray(s, i, '[', ']');
    } else {
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
               !std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
    }
}

struct ParsedAsset {
    std::string name;
    std::string download_url;
    uint64_t size = 0;
};

void ParseAssetsArray(const std::string& s, size_t& i, std::vector<ParsedAsset>& out_assets) {
    SkipWs(s, i);
    if (i >= s.size() || s[i] != '[') return;
    ++i;
    while (i < s.size()) {
        SkipWs(s, i);
        if (i >= s.size() || s[i] == ']') { if (i < s.size()) ++i; break; }
        if (s[i] == ',') { ++i; continue; }
        if (s[i] != '{') { SkipJsonValue(s, i); continue; }
        ++i;
        ParsedAsset a;
        while (i < s.size()) {
            SkipWs(s, i);
            if (i >= s.size() || s[i] == '}') { if (i < s.size()) ++i; break; }
            if (s[i] == ',') { ++i; continue; }
            std::string key;
            if (!ParseJsonString(s, i, key)) { SkipJsonValue(s, i); continue; }
            SkipWs(s, i);
            if (i < s.size() && s[i] == ':') ++i;
            SkipWs(s, i);
            if (key == "name" && i < s.size() && s[i] == '"') {
                ParseJsonString(s, i, a.name);
            } else if (key == "browser_download_url" && i < s.size() && s[i] == '"') {
                ParseJsonString(s, i, a.download_url);
            } else if (key == "size") {
                ParseJsonU64(s, i, a.size);
            } else {
                SkipJsonValue(s, i);
            }
        }
        if (!a.name.empty() && !a.download_url.empty()) {
            out_assets.push_back(std::move(a));
        }
    }
}

std::string CleanReleaseNotes(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    std::istringstream iss(raw);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Strip leading markdown heading markers (e.g. "### ") or blockquote "> "
        size_t p = 0;
        while (p < line.size() && line[p] == '#') ++p;
        if (p > 0 && p < line.size() && line[p] == ' ') {
            line = line.substr(p + 1);
        } else if (line.rfind("> ", 0) == 0) {
            line = line.substr(2);
        }
        // Strip inline backticks and bold markers (**) for clean plain-text rendering
        std::string cleaned;
        cleaned.reserve(line.size());
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '`') continue;
            if (line[i] == '*' && i + 1 < line.size() && line[i + 1] == '*') {
                ++i;
                continue;
            }
            cleaned.push_back(line[i]);
        }
        if (cleaned.rfind("- ", 0) == 0) {
            cleaned = "• " + cleaned.substr(2);
        }
        if (!first) out.push_back('\n');
        out += cleaned;
        first = false;
    }
    while (!out.empty() && (out.front() == '\n' || out.front() == ' ')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

HINTERNET OpenHttpSession(const std::string& user_agent) {
    std::wstring ua = Utf8ToWide(user_agent);
    HINTERNET h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h) {
        h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (h) {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
            protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
    }
    return h;
}

} // namespace

int CompareSemver(const std::string& a, const std::string& b) {
    auto ta = ParseVersionTriplet(a);
    auto tb = ParseVersionTriplet(b);
    for (int i = 0; i < 3; ++i) {
        if (ta[i] < tb[i]) return -1;
        if (ta[i] > tb[i]) return 1;
    }
    return 0;
}

bool IsTrustedGithubUrl(const std::string& url) {
    std::string low = ToLowerAscii(url);
    if (low.rfind("https://", 0) != 0) return false;
    size_t host_start = 8;
    size_t host_end = low.find('/', host_start);
    std::string host = (host_end == std::string::npos) ? low.substr(host_start)
                                                       : low.substr(host_start, host_end - host_start);
    if (host == "github.com" || host == "api.github.com") return true;
    static constexpr const char* kSuffix1 = ".github.com";
    static constexpr const char* kSuffix2 = ".githubusercontent.com";
    if (host.size() > std::strlen(kSuffix1) &&
        host.compare(host.size() - std::strlen(kSuffix1), std::strlen(kSuffix1), kSuffix1) == 0) {
        return true;
    }
    if (host.size() > std::strlen(kSuffix2) &&
        host.compare(host.size() - std::strlen(kSuffix2), std::strlen(kSuffix2), kSuffix2) == 0) {
        return true;
    }
    return false;
}

ReleaseInfo ParseLatestReleaseJson(const std::string& json_text, const std::string& current_version) {
    ReleaseInfo info;
    info.current_version = StripLeadingV(current_version);
    size_t i = 0;
    SkipWs(json_text, i);
    if (i >= json_text.size() || json_text[i] != '{') {
        info.error = "Invalid response from GitHub Releases API.";
        return info;
    }
    ++i;

    std::vector<ParsedAsset> assets;
    bool is_draft = false;
    bool is_prerelease = false;
    while (i < json_text.size()) {
        SkipWs(json_text, i);
        if (i >= json_text.size() || json_text[i] == '}') break;
        if (json_text[i] == ',') { ++i; continue; }
        std::string key;
        if (!ParseJsonString(json_text, i, key)) { SkipJsonValue(json_text, i); continue; }
        SkipWs(json_text, i);
        if (i < json_text.size() && json_text[i] == ':') ++i;
        SkipWs(json_text, i);
        if (key == "tag_name" && i < json_text.size() && json_text[i] == '"') {
            ParseJsonString(json_text, i, info.tag_name);
        } else if (key == "name" && i < json_text.size() && json_text[i] == '"') {
            ParseJsonString(json_text, i, info.release_name);
        } else if (key == "body" && i < json_text.size() && json_text[i] == '"') {
            std::string body;
            ParseJsonString(json_text, i, body);
            info.release_notes = CleanReleaseNotes(body);
        } else if (key == "html_url" && i < json_text.size() && json_text[i] == '"') {
            ParseJsonString(json_text, i, info.html_url);
        } else if (key == "draft") {
            is_draft = (json_text.compare(i, 4, "true") == 0);
            SkipJsonValue(json_text, i);
        } else if (key == "prerelease") {
            is_prerelease = (json_text.compare(i, 4, "true") == 0);
            SkipJsonValue(json_text, i);
        } else if (key == "assets" && i < json_text.size() && json_text[i] == '[') {
            ParseAssetsArray(json_text, i, assets);
        } else {
            SkipJsonValue(json_text, i);
        }
    }

    if (info.tag_name.empty()) {
        info.error = "GitHub release response is missing tag_name.";
        return info;
    }
    if (is_draft || is_prerelease) {
        info.ok = true;
        info.latest_version = StripLeadingV(info.tag_name);
        info.update_available = false;
        return info;
    }

    info.latest_version = StripLeadingV(info.tag_name);
    if (info.release_name.empty()) info.release_name = "v" + info.latest_version;

    // Pick portable Windows .zip asset (prefer RealRacing3-PC-Port*.zip)
    const ParsedAsset* chosen = nullptr;
    for (const auto& a : assets) {
        std::string low = ToLowerAscii(a.name);
        if (low.size() >= 4 && low.compare(low.size() - 4, 4, ".zip") == 0 &&
            IsTrustedGithubUrl(a.download_url)) {
            if (!chosen || low.find("realracing3-pc-port") != std::string::npos) {
                chosen = &a;
                if (low == "realracing3-pc-port.zip") break;
            }
        }
    }
    if (chosen) {
        info.asset_name = chosen->name;
        info.download_url = chosen->download_url;
        info.asset_size = chosen->size;
    }
    if (!info.html_url.empty() && !IsTrustedGithubUrl(info.html_url)) {
        info.html_url.clear();
    }

    info.ok = true;
    info.update_available = (CompareSemver(info.current_version, info.latest_version) < 0) &&
                            !info.download_url.empty();
    return info;
}

ReleaseInfo CheckForUpdates(const std::string& current_version, const std::string& repo_slug) {
    ReleaseInfo info;
    info.current_version = StripLeadingV(current_version);

    HINTERNET h_sess = OpenHttpSession("RealRacing3-PC-Port/" + info.current_version);
    if (!h_sess) {
        info.error = "Could not initialize Windows HTTP client.";
        return info;
    }
    WinHttpSetTimeouts(h_sess, 5000, 5000, 8000, 8000);

    HINTERNET h_conn = WinHttpConnect(h_sess, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!h_conn) {
        WinHttpCloseHandle(h_sess);
        info.error = "Could not connect to GitHub.";
        return info;
    }

    std::wstring path = Utf8ToWide("/repos/" + repo_slug + "/releases/latest");
    HINTERNET h_req = WinHttpOpenRequest(h_conn, L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!h_req) {
        WinHttpCloseHandle(h_conn);
        WinHttpCloseHandle(h_sess);
        info.error = "Could not create update request.";
        return info;
    }

    const wchar_t* hdrs = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    BOOL sent = WinHttpSendRequest(h_req, hdrs, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(h_req, nullptr);
    if (!sent) {
        WinHttpCloseHandle(h_req);
        WinHttpCloseHandle(h_conn);
        WinHttpCloseHandle(h_sess);
        info.error = "Unable to reach GitHub (offline or blocked).";
        return info;
    }

    DWORD status = 0;
    DWORD status_sz = sizeof(status);
    WinHttpQueryHeaders(h_req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WinHttpCloseHandle(h_req);
        WinHttpCloseHandle(h_conn);
        WinHttpCloseHandle(h_sess);
        info.error = "GitHub returned HTTP " + std::to_string(status) + ".";
        return info;
    }

    std::string body;
    body.reserve(16384);
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(h_req, &avail) && avail > 0) {
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(h_req, buf.data(), avail, &read) || read == 0) break;
        body.append(buf.data(), read);
        if (body.size() > 2 * 1024 * 1024) break;
    }

    WinHttpCloseHandle(h_req);
    WinHttpCloseHandle(h_conn);
    WinHttpCloseHandle(h_sess);
    return ParseLatestReleaseJson(body, info.current_version);
}

bool DownloadFileWithProgress(const std::string& url,
                              const std::filesystem::path& dest_path,
                              const std::function<void(uint64_t downloaded, uint64_t total)>& on_progress,
                              std::atomic<bool>* cancel_flag,
                              std::string& out_error) {
    if (!IsTrustedGithubUrl(url)) {
        out_error = "Blocked untrusted download URL.";
        return false;
    }

    std::wstring wurl = Utf8ToWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host_buf[256]{};
    wchar_t path_buf[2048]{};
    wchar_t extra_buf[1024]{};
    uc.lpszHostName = host_buf;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host_buf));
    uc.lpszUrlPath = path_buf;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path_buf));
    uc.lpszExtraInfo = extra_buf;
    uc.dwExtraInfoLength = static_cast<DWORD>(std::size(extra_buf));
    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc) ||
        uc.nScheme != INTERNET_SCHEME_HTTPS) {
        out_error = "Invalid HTTPS download URL.";
        return false;
    }
    std::wstring req_path(path_buf);
    if (extra_buf[0] != L'\0') req_path += extra_buf;

    HINTERNET h_sess = OpenHttpSession(std::string("RealRacing3-PC-Port/") + kPortVersion);
    if (!h_sess) {
        out_error = "Could not initialize Windows HTTP client.";
        return false;
    }
    WinHttpSetTimeouts(h_sess, 10000, 10000, 30000, 30000);

    HINTERNET h_conn = WinHttpConnect(h_sess, host_buf, uc.nPort, 0);
    if (!h_conn) {
        WinHttpCloseHandle(h_sess);
        out_error = "Could not connect to download host.";
        return false;
    }

    HINTERNET h_req = WinHttpOpenRequest(h_conn, L"GET", req_path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!h_req) {
        WinHttpCloseHandle(h_conn);
        WinHttpCloseHandle(h_sess);
        out_error = "Could not open download request.";
        return false;
    }

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(h_req, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    BOOL ok = WinHttpSendRequest(h_req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(h_req, nullptr);
    if (!ok) {
        WinHttpCloseHandle(h_req);
        WinHttpCloseHandle(h_conn);
        WinHttpCloseHandle(h_sess);
        out_error = "Download failed to connect.";
        return false;
    }

    DWORD status = 0;
    DWORD status_sz = sizeof(status);
    WinHttpQueryHeaders(h_req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        WinHttpCloseHandle(h_req);
        WinHttpCloseHandle(h_conn);
        WinHttpCloseHandle(h_sess);
        out_error = "Download failed (HTTP " + std::to_string(status) + ").";
        return false;
    }

    uint64_t total_bytes = 0;
    wchar_t cl_buf[64]{};
    DWORD cl_sz = sizeof(cl_buf);
    if (WinHttpQueryHeaders(h_req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                            cl_buf, &cl_sz, WINHTTP_NO_HEADER_INDEX)) {
        try { total_bytes = std::stoull(cl_buf); } catch (...) {}
    }

    std::error_code ec;
    if (dest_path.has_parent_path()) {
        std::filesystem::create_directories(dest_path.parent_path(), ec);
    }
    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        WinHttpCloseHandle(h_req);
        WinHttpCloseHandle(h_conn);
        WinHttpCloseHandle(h_sess);
        out_error = "Could not write download file to disk.";
        return false;
    }

    uint64_t downloaded = 0;
    std::vector<char> chunk(65536);
    bool cancelled = false;
    while (true) {
        if (cancel_flag && cancel_flag->load(std::memory_order_relaxed)) {
            cancelled = true;
            break;
        }
        DWORD read = 0;
        if (!WinHttpReadData(h_req, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
            out_error = "Network read interrupted during download.";
            out.close();
            std::filesystem::remove(dest_path, ec);
            WinHttpCloseHandle(h_req);
            WinHttpCloseHandle(h_conn);
            WinHttpCloseHandle(h_sess);
            return false;
        }
        if (read == 0) break;
        out.write(chunk.data(), static_cast<std::streamsize>(read));
        if (!out.good()) {
            out_error = "Disk write failed while downloading update.";
            out.close();
            std::filesystem::remove(dest_path, ec);
            WinHttpCloseHandle(h_req);
            WinHttpCloseHandle(h_conn);
            WinHttpCloseHandle(h_sess);
            return false;
        }
        downloaded += read;
        if (on_progress) on_progress(downloaded, total_bytes);
    }

    out.close();
    WinHttpCloseHandle(h_req);
    WinHttpCloseHandle(h_conn);
    WinHttpCloseHandle(h_sess);

    if (cancelled) {
        std::filesystem::remove(dest_path, ec);
        out_error = "Update cancelled.";
        return false;
    }
    if (downloaded == 0) {
        std::filesystem::remove(dest_path, ec);
        out_error = "Downloaded update archive was empty.";
        return false;
    }
    return true;
}

bool ExtractUpdateZipToStaging(const std::filesystem::path& zip_path,
                               const std::filesystem::path& staging_dir,
                               const std::filesystem::path& install_dir,
                               std::string& out_error) {
    std::ifstream in(zip_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        out_error = "Could not open downloaded update archive.";
        return false;
    }
    const int64_t fsz = static_cast<int64_t>(in.tellg());
    if (fsz < 22) {
        out_error = "Downloaded update archive is corrupt.";
        return false;
    }

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
    if (eocd < 0) {
        out_error = "Downloaded file is not a valid ZIP archive.";
        return false;
    }

    const uint16_t count = ReadLe16(tail.data() + eocd + 10);
    const uint32_t cd_sz = ReadLe32(tail.data() + eocd + 12);
    const uint32_t cd_off = ReadLe32(tail.data() + eocd + 16);
    if (static_cast<int64_t>(cd_off) + cd_sz > fsz) {
        out_error = "Corrupt ZIP central directory.";
        return false;
    }

    std::vector<uint8_t> cd(cd_sz);
    in.seekg(cd_off, std::ios::beg);
    in.read(reinterpret_cast<char*>(cd.data()), static_cast<std::streamsize>(cd_sz));

    std::error_code ec;
    std::filesystem::remove_all(staging_dir, ec);
    ec.clear();
    std::filesystem::create_directories(staging_dir, ec);
    if (ec) {
        out_error = "Could not create update staging folder (check folder write permissions).";
        return false;
    }

    const auto canon_root = std::filesystem::weakly_canonical(staging_dir, ec);
    const bool keep_existing_ini = std::filesystem::is_regular_file(install_dir / "rr3_pc.ini", ec);

    size_t extracted = 0;
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
        while (!name.empty() && name.front() == '/') name.erase(name.begin());
        if (name.empty() || name.find("..") != std::string::npos || name.find(':') != std::string::npos) {
            continue;
        }

        // Preserve the user's existing rr3_pc.ini settings and paths!
        std::string low_name = ToLowerAscii(name);
        if (keep_existing_ini && low_name == "rr3_pc.ini") {
            continue;
        }
        // Never touch user save/work_bins directories even if an archive unexpectedly contained them
        if (low_name.rfind("userdata/", 0) == 0 || low_name.rfind("work_bins/", 0) == 0) {
            continue;
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

        const auto dst = (staging_dir / std::filesystem::path(name)).lexically_normal();
        const auto canon_dst = std::filesystem::weakly_canonical(dst, ec);
        if (canon_dst.wstring().rfind(canon_root.wstring(), 0) != 0) continue;

        if (dst.has_parent_path()) std::filesystem::create_directories(dst.parent_path(), ec);
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            out_error = "Failed writing staged update file: " + name;
            return false;
        }
        out.write(reinterpret_cast<const char*>(wptr), static_cast<std::streamsize>(wlen));
        if (!out.good()) {
            out_error = "Disk error writing staged file: " + name;
            return false;
        }
        ++extracted;
    }

    const auto staged_exe = staging_dir / "rr3_pc.exe";
    if (extracted == 0 || !std::filesystem::is_regular_file(staged_exe, ec) ||
        std::filesystem::file_size(staged_exe, ec) < 1024) {
        out_error = "Update archive did not contain a valid rr3_pc.exe.";
        return false;
    }
    return true;
}

bool LaunchUpdateApplyHelper(const std::filesystem::path& staging_dir,
                             const std::filesystem::path& zip_path,
                             const std::filesystem::path& install_dir,
                             std::string& out_error) {
    const auto script_path = install_dir / "_apply_update.cmd";
    const DWORD pid = GetCurrentProcessId();

    std::ofstream bat(script_path, std::ios::binary | std::ios::trunc);
    if (!bat.is_open()) {
        out_error = "Could not write update helper script (folder may be read-only).";
        return false;
    }
    const std::string stage_s = staging_dir.string();
    const std::string dst_s = install_dir.string();
    const std::string zip_s = zip_path.string();
    const std::string exe_s = (install_dir / "rr3_pc.exe").string();

    bat << "@echo off\r\n"
        << "setlocal\r\n"
        << "set \"PID=" << pid << "\"\r\n"
        << "set /a TRIES=0\r\n"
        << ":wait_exit\r\n"
        << "tasklist /FI \"PID eq %PID%\" 2>nul | find /I \"%PID%\" >nul\r\n"
        << "if not errorlevel 1 (\r\n"
        << "  set /a TRIES+=1\r\n"
        << "  if %TRIES% GEQ 60 goto apply_now\r\n"
        << "  ping 127.0.0.1 -n 2 >nul\r\n"
        << "  goto wait_exit\r\n"
        << ")\r\n"
        << ":apply_now\r\n"
        << "set /a COPY_TRIES=0\r\n"
        << ":copy_retry\r\n"
        << "xcopy /E /Y /I /Q \"" << stage_s << "\\*\" \"" << dst_s << "\\\" >nul 2>&1\r\n"
        << "if errorlevel 1 (\r\n"
        << "  set /a COPY_TRIES+=1\r\n"
        << "  if %COPY_TRIES% LSS 10 (\r\n"
        << "    ping 127.0.0.1 -n 2 >nul\r\n"
        << "    goto copy_retry\r\n"
        << "  )\r\n"
        << ")\r\n"
        << "rmdir /S /Q \"" << stage_s << "\" >nul 2>&1\r\n"
        << "del /F /Q \"" << zip_s << "\" >nul 2>&1\r\n"
        << "start \"\" \"" << exe_s << "\"\r\n"
        << "del /F /Q \"%~f0\" >nul 2>&1\r\n";
    bat.close();
    if (!bat.good()) {
        out_error = "Failed writing update helper script.";
        return false;
    }

    std::wstring cmdline = L"cmd.exe /C \"\"" + script_path.wstring() + L"\"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                        install_dir.c_str(), &si, &pi)) {
        out_error = "Could not start update helper process.";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void CleanupLeftoverUpdateFiles(const std::filesystem::path& install_dir) {
    std::error_code ec;
    std::filesystem::remove_all(install_dir / "_update_staging", ec);
    ec.clear();
    std::filesystem::remove(install_dir / "_update_download.zip", ec);
    ec.clear();
    std::filesystem::remove(install_dir / "_apply_update.cmd", ec);
}
