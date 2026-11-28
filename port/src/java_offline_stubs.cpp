#include "java_offline_stubs.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::wstring Utf8ToWideStr(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

std::string WideToUtf8Str(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

std::string ToLowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void OpenBrowserUrlIfHttp(const std::string& url) {
    std::string low = ToLowerStr(url);
    if (low.rfind("https://", 0) == 0 || low.rfind("http://", 0) == 0) {
        std::fprintf(stdout, "[ONLINE] Opening URL in browser: %s\n", url.c_str());
        ShellExecuteW(nullptr, L"open", Utf8ToWideStr(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

} // namespace

std::string GetOrCreateDeviceUid(const PcConfig& cfg) {
    if (!cfg.online_mode) return "rr3pc-offline-0001";
    std::filesystem::path uid_file = std::filesystem::path(cfg.userdata_dir) / "device_uid.txt";
    std::error_code ec;
    if (std::filesystem::is_regular_file(uid_file, ec)) {
        std::ifstream in(uid_file);
        std::string line;
        if (std::getline(in, line) && line.size() >= 8) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
                line.pop_back();
            }
            if (line.size() >= 8) return line;
        }
    }
    uint8_t rnd[16]{};
    BCryptGenRandom(nullptr, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* kHex = "0123456789abcdef";
    std::string uid = "rr3pc-";
    for (uint8_t b : rnd) {
        uid.push_back(kHex[(b >> 4) & 0xF]);
        uid.push_back(kHex[b & 0xF]);
    }
    if (uid_file.has_parent_path()) std::filesystem::create_directories(uid_file.parent_path(), ec);
    std::ofstream out(uid_file, std::ios::trunc);
    out << uid << "\n";
    return uid;
}

namespace {

std::string LoadOrCreateOnlineDeviceUid(const PcConfig& cfg) {
    return GetOrCreateDeviceUid(cfg);
}

// ---- Backed Java Collection states for HttpRequest_headerCallback (Map<String, List<String>>) ----
struct JavaIterState {
    std::vector<uint64_t> items;
    size_t pos = 0;
};

struct JavaHeaderMapState {
    std::vector<std::pair<std::string, std::vector<std::string>>> entries;
};

struct CloudcellHttpReqState {
    std::string user_agent;
    std::string method = "GET";
    std::string url;
    std::vector<uint8_t> body;
    int read_capacity = 65536;
    int64_t native_ptr = 0;
    bool fail_on_error_status = false;
    double server_time = 0.0;
    bool ssl_check = false;
    bool add_default_headers = false;
    int timeout_ms = 30000;
    std::vector<std::pair<std::string, std::string>> headers;
    std::atomic<bool> closed{false};
    std::atomic<bool> posted{false};
};

struct CloudcellHttpResult {
    bool network_ok = false;
    int http_status = -1;
    int content_length = 0;
    std::vector<std::pair<std::string, std::vector<std::string>>> resp_headers;
    std::vector<uint8_t> resp_body;
};

void ParseRawHttpHeaders(const std::string& raw,
                         std::vector<std::pair<std::string, std::vector<std::string>>>& out_headers,
                         int& out_content_len) {
    out_headers.clear();
    out_content_len = 0;
    std::istringstream iss(raw);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) { first = false; continue; } // skip HTTP/1.1 200 OK status line
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        if (ToLowerStr(k) == "content-length") {
            try { out_content_len = std::stoi(v); } catch (...) {}
        }
        bool merged = false;
        std::string lk = ToLowerStr(k);
        for (auto& kv : out_headers) {
            if (ToLowerStr(kv.first) == lk) {
                kv.second.push_back(v);
                merged = true;
                break;
            }
        }
        if (!merged) {
            out_headers.push_back({k, {v}});
        }
    }
}

CloudcellHttpResult ExecuteWinHttpRequest(const std::shared_ptr<CloudcellHttpReqState>& req) {
    CloudcellHttpResult res;
    std::wstring wurl = Utf8ToWideStr(req->url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host_buf[256]{};
    wchar_t path_buf[4096]{};
    wchar_t extra_buf[4096]{};
    uc.lpszHostName = host_buf;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host_buf));
    uc.lpszUrlPath = path_buf;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path_buf));
    uc.lpszExtraInfo = extra_buf;
    uc.dwExtraInfoLength = static_cast<DWORD>(std::size(extra_buf));
    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) {
        return res;
    }
    const bool is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    if (!is_https && uc.nScheme != INTERNET_SCHEME_HTTP) {
        return res;
    }
    std::wstring full_path = (path_buf[0] != L'\0') ? std::wstring(path_buf) : L"/";
    if (extra_buf[0] != L'\0') full_path += extra_buf;

    std::string ua = req->user_agent.empty()
                         ? "RealRacing3/14.0.1 (Linux; Android 12; SM-S918B)"
                         : req->user_agent;
    std::wstring wua = Utf8ToWideStr(ua);
    HINTERNET h_sess = WinHttpOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h_sess) {
        h_sess = WinHttpOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!h_sess) return res;

    int t_ms = std::clamp(req->timeout_ms, 5000, 60000);
    WinHttpSetTimeouts(h_sess, 10000, 10000, t_ms, t_ms);
    if (is_https) {
        DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(h_sess, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos))) {
            protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(h_sess, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
        }
    }

    HINTERNET h_conn = WinHttpConnect(h_sess, host_buf, uc.nPort, 0);
    if (!h_conn) {
        WinHttpCloseHandle(h_sess);
        return res;
    }

    std::wstring wmethod = Utf8ToWideStr(req->method.empty() ? "GET" : req->method);
    HINTERNET h_req = WinHttpOpenRequest(h_conn, wmethod.c_str(), full_path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         is_https ? WINHTTP_FLAG_SECURE : 0);
    if (!h_req) {
        WinHttpCloseHandle(h_conn);
        WinHttpCloseHandle(h_sess);
        return res;
    }

    // Match Android CloudcellTrustManager + ALLOW_ALL_HOSTNAME_VERIFIER (required for private servers
    // such as Project Resurrection's servers.devel60.net:62002 which uses a self-signed certificate).
    if (is_https) {
        DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(h_req, WINHTTP_OPTION_SECURITY_FLAGS, &sec_flags, sizeof(sec_flags));
    }
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(h_req, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    bool has_ct = false;
    std::wstring hdr_block;
    for (const auto& [k, v] : req->headers) {
        if (ToLowerStr(k) == "content-type") has_ct = true;
        if (ToLowerStr(k) == "user-agent") continue;
        hdr_block += Utf8ToWideStr(k + ": " + v + "\r\n");
    }
    if (!req->body.empty() && req->add_default_headers && !has_ct) {
        hdr_block += L"Content-Type: application/x-www-form-urlencoded\r\n";
    }

    const void* body_ptr = req->body.empty() ? WINHTTP_NO_REQUEST_DATA : req->body.data();
    const DWORD body_len = static_cast<DWORD>(req->body.size());
    BOOL ok = WinHttpSendRequest(h_req,
                                 hdr_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr_block.c_str(),
                                 hdr_block.empty() ? 0 : static_cast<DWORD>(-1L),
                                 const_cast<void*>(body_ptr), body_len, body_len, 0);
    if (ok) ok = WinHttpReceiveResponse(h_req, nullptr);
    if (!ok || req->closed.load(std::memory_order_acquire)) {
        WinHttpCloseHandle(h_req);
        WinHttpCloseHandle(h_conn);
        WinHttpCloseHandle(h_sess);
        return res;
    }

    DWORD status = 0;
    DWORD status_sz = sizeof(status);
    WinHttpQueryHeaders(h_req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX);
    res.http_status = static_cast<int>(status);
    res.network_ok = true;

    DWORD raw_hdr_sz = 0;
    WinHttpQueryHeaders(h_req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &raw_hdr_sz, WINHTTP_NO_HEADER_INDEX);
    if (raw_hdr_sz > 0) {
        std::wstring whdrs(raw_hdr_sz / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(h_req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                whdrs.data(), &raw_hdr_sz, WINHTTP_NO_HEADER_INDEX)) {
            ParseRawHttpHeaders(WideToUtf8Str(whdrs), res.resp_headers, res.content_length);
        }
    }

    std::vector<uint8_t> chunk(65536);
    while (!req->closed.load(std::memory_order_acquire)) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(h_req, &avail) || avail == 0) break;
        DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(chunk.size()));
        DWORD got = 0;
        if (!WinHttpReadData(h_req, chunk.data(), to_read, &got) || got == 0) break;
        res.resp_body.insert(res.resp_body.end(), chunk.begin(), chunk.begin() + got);
        if (res.resp_body.size() > 64 * 1024 * 1024) break; // safety cap
    }
    if (res.content_length == 0 && !res.resp_body.empty()) {
        res.content_length = static_cast<int>(res.resp_body.size());
    }

    WinHttpCloseHandle(h_req);
    WinHttpCloseHandle(h_conn);
    WinHttpCloseHandle(h_sess);
    return res;
}

} // namespace

void RegisterOfflineStubs(ElfLoader& loader, FakeJvm& jvm, FiremintRuntime& firemint, const PcConfig& cfg) {
    const bool online = cfg.online_mode;
    const std::string device_uid = LoadOrCreateOnlineDeviceUid(cfg);

    // ---- Native C++ Nimble bridge exports imported by libRealRacing3.so ----
    loader.RegisterHostFunction("_ZN2EA6Nimble6getEnvEv", [&jvm](CpuContext& ctx) {
        ctx.SetX(0, jvm.GetJniEnvGuestPtr());
    });
    loader.RegisterHostFunction("_ZN2EA6Nimble9findClassEPKc", [&jvm](CpuContext& ctx) {
        const char* raw = reinterpret_cast<const char*>(static_cast<uintptr_t>(ctx.GetX(0)));
        if (!raw || !*raw) { ctx.SetX(0, 0); return; }
        uint64_t h = jvm.FindClassHandle(raw);
        if (!h) {
            if (IsVerboseLogging()) {
                std::fprintf(stderr, "[OFFLINE] Auto-stubbing Nimble class: %s\n", raw);
            }
            h = jvm.RegisterJniClass(JniClassDef{raw});
        }
        ctx.SetX(0, h);
    });

    // ---- Android & Java Collection utility classes ----
    {
        JniClassDef d;
        d.name = "java/io/File";
        d.methods["<init>:(Ljava/lang/String;)V"] = [](FakeJvm& j, uint64_t self, const auto& a) {
            if (!a.empty()) j.SetInstanceField(self, "path", a[0]);
            return JniVal::Void();
        };
        d.methods["getAbsolutePath"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto v = j.GetInstanceField(self, "path");
            return v.AsObj() ? v : JniVal::Obj(j.NewStringUtf("/storage/rr3"));
        };
        d.methods["getPath"] = d.methods["getAbsolutePath"];
        d.methods["exists"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/lang/ClassLoader";
        d.methods["loadClass"] = [](FakeJvm& j, uint64_t, const auto& a) {
            if (a.empty()) return JniVal::Obj(0);
            std::string nm = j.GetStringUtf(a[0].AsObj());
            uint64_t h = j.FindClassHandle(nm);
            if (!h && !nm.empty()) h = j.RegisterJniClass(JniClassDef{nm});
            return JniVal::Obj(h);
        };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/util/Iterator";
        d.methods["hasNext"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto o = j.GetJniObject(self);
            if (o && o->native_state.has_value()) {
                auto it = std::any_cast<std::shared_ptr<JavaIterState>>(o->native_state);
                return JniVal::Bool(it && it->pos < it->items.size());
            }
            return JniVal::Bool(false);
        };
        d.methods["next"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto o = j.GetJniObject(self);
            if (o && o->native_state.has_value()) {
                auto it = std::any_cast<std::shared_ptr<JavaIterState>>(o->native_state);
                if (it && it->pos < it->items.size()) {
                    return JniVal::Obj(it->items[it->pos++]);
                }
            }
            return JniVal::Obj(0);
        };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/util/Set";
        d.methods["iterator"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto st = std::make_shared<JavaIterState>();
            auto o = j.GetJniObject(self);
            if (o && o->native_state.has_value()) {
                st->items = std::any_cast<std::vector<uint64_t>>(o->native_state);
            }
            return JniVal::Obj(j.NewObjectInstance("java/util/Iterator", st));
        };
        d.methods["size"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto o = j.GetJniObject(self);
            if (o && o->native_state.has_value()) {
                const auto& items = std::any_cast<const std::vector<uint64_t>&>(o->native_state);
                return JniVal::Int(static_cast<int32_t>(items.size()));
            }
            return JniVal::Int(0);
        };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/util/List";
        d.methods["size"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto o = j.GetJniObject(self);
            if (o && o->native_state.has_value()) {
                const auto& items = std::any_cast<const std::vector<uint64_t>&>(o->native_state);
                return JniVal::Int(static_cast<int32_t>(items.size()));
            }
            return JniVal::Int(0);
        };
        d.methods["get"] = [](FakeJvm& j, uint64_t self, const auto& a) {
            auto o = j.GetJniObject(self);
            if (o && o->native_state.has_value() && !a.empty()) {
                const auto& items = std::any_cast<const std::vector<uint64_t>&>(o->native_state);
                int idx = a[0].AsInt();
                if (idx >= 0 && static_cast<size_t>(idx) < items.size()) {
                    return JniVal::Obj(items[static_cast<size_t>(idx)]);
                }
            }
            return JniVal::Obj(0);
        };
        d.methods["iterator"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto st = std::make_shared<JavaIterState>();
            auto o = j.GetJniObject(self);
            if (o && o->native_state.has_value()) {
                st->items = std::any_cast<std::vector<uint64_t>>(o->native_state);
            }
            return JniVal::Obj(j.NewObjectInstance("java/util/Iterator", st));
        };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "java/util/Map";
        d.methods["keySet"] = [](FakeJvm& j, uint64_t self, const auto&) {
            std::vector<uint64_t> keys;
            auto o = j.GetJniObject(self);
            if (o && o->native_state.has_value()) {
                auto mp = std::any_cast<std::shared_ptr<JavaHeaderMapState>>(o->native_state);
                if (mp) {
                    for (const auto& kv : mp->entries) {
                        keys.push_back(j.NewStringUtf(kv.first));
                    }
                }
            }
            return JniVal::Obj(j.NewObjectInstance("java/util/Set", std::move(keys)));
        };
        d.methods["get"] = [](FakeJvm& j, uint64_t self, const auto& a) {
            if (a.empty()) return JniVal::Obj(0);
            std::string req_key = ToLowerStr(j.GetStringUtf(a[0].AsObj()));
            auto o = j.GetJniObject(self);
            if (o && o->native_state.has_value()) {
                auto mp = std::any_cast<std::shared_ptr<JavaHeaderMapState>>(o->native_state);
                if (mp) {
                    for (const auto& kv : mp->entries) {
                        if (ToLowerStr(kv.first) == req_key) {
                            std::vector<uint64_t> vals;
                            for (const auto& s : kv.second) vals.push_back(j.NewStringUtf(s));
                            return JniVal::Obj(j.NewObjectInstance("java/util/List", std::move(vals)));
                        }
                    }
                }
            }
            return JniVal::Obj(0);
        };
        d.methods["size"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto o = j.GetJniObject(self);
            if (o && o->native_state.has_value()) {
                auto mp = std::any_cast<std::shared_ptr<JavaHeaderMapState>>(o->native_state);
                if (mp) return JniVal::Int(static_cast<int32_t>(mp->entries.size()));
            }
            return JniVal::Int(0);
        };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "android/net/Uri";
        d.static_methods["parse"] = [](FakeJvm& j, uint64_t, const auto& a) {
            uint64_t h = j.NewObjectInstance("android/net/Uri");
            if (!a.empty()) j.SetInstanceField(h, "uri", a[0]);
            return JniVal::Obj(h);
        };
        d.methods["toString"] = [](FakeJvm& j, uint64_t self, const auto&) {
            auto v = j.GetInstanceField(self, "uri");
            return v.AsObj() ? v : JniVal::Obj(j.NewStringUtf(""));
        };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "android/content/Intent";
        d.methods["setData"] = [](FakeJvm& j, uint64_t self, const auto& a) {
            if (!a.empty()) j.SetInstanceField(self, "data_uri", a[0]);
            return JniVal::Obj(self);
        };
        d.methods["setAction"] = [](FakeJvm&, uint64_t self, const auto&) { return JniVal::Obj(self); };
        d.methods["getStringExtra"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(0); };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "android/content/Context";
        d.methods["getPackageName"] = [](FakeJvm& j, uint64_t, const auto&) {
            return JniVal::Obj(j.NewStringUtf("com.ea.games.r3_row"));
        };
        d.methods["getApplicationContext"] = [](FakeJvm&, uint64_t self, const auto&) {
            return JniVal::Obj(self);
        };
        d.methods["getFilesDir"] = [](FakeJvm& j, uint64_t, const auto&) {
            uint64_t f = j.NewObjectInstance("java/io/File");
            j.SetInstanceField(f, "path", JniVal::Obj(j.NewStringUtf("/storage/rr3")));
            return JniVal::Obj(f);
        };
        d.methods["getExternalFilesDir"] = d.methods["getFilesDir"];
        d.methods["getAssets"] = [&firemint](FakeJvm&, uint64_t, const auto&) {
            return JniVal::Obj(firemint.AssetManagerHandle());
        };
        d.methods["getClassLoader"] = [](FakeJvm& j, uint64_t, const auto&) {
            return JniVal::Obj(j.NewObjectInstance("java/lang/ClassLoader"));
        };
        jvm.RegisterJniClass(std::move(d));
    }
    jvm.RegisterJniClass(JniClassDef{"android/app/Activity", "android/content/Context"});
    jvm.RegisterJniClass(JniClassDef{"android/content/res/AssetManager"});
    jvm.RegisterJniClass(JniClassDef{"com/firemonkeys/cloudcellapi/CC_Activity", "android/app/Activity"});

    // ---- NetworkStatusMonitor (CONNECTION_TYPE_NONE=1, CONNECTION_TYPE_WIFI=2) ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/NetworkStatusMonitor";
        d.static_fields["CONNECTION_TYPE_NONE"] = JniVal::Int(1);
        d.static_fields["CONNECTION_TYPE_WIFI"] = JniVal::Int(2);
        d.static_fields["CONNECTION_TYPE_ETHERNET"] = JniVal::Int(6);
        d.static_methods["getNetworkConnectivity"] = [online](FakeJvm&, uint64_t, const auto&) {
            return JniVal::Int(online ? 2 : 1);
        };
        jvm.RegisterJniClass(std::move(d));
    }

    // ---- CC_Component ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/CC_Component";
        uint64_t cc_inst = jvm.NewObjectInstance("com/firemonkeys/cloudcellapi/CC_Component");
        jvm.PinHandle(cc_inst);
        d.static_methods["GetInstance"] = [cc_inst](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(cc_inst); };
        d.static_methods["GetActivity"] = [&firemint](FakeJvm&, uint64_t, const auto&) {
            return JniVal::Obj(firemint.MainActivityHandle());
        };
        d.static_methods["getNetworkConnectivity"] = [online](FakeJvm&, uint64_t, const auto&) {
            return JniVal::Int(online ? 2 : 1);
        };
        d.static_methods["getIsUsingImmersiveMode"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["setIsUsingImmersiveMode"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["canCallSuspendResume"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["startDownloadService"] = [online](FakeJvm&, uint64_t, const auto&) {
            if (!online) {
                std::fprintf(stderr,
                             "[OFFLINE] RR3 requested asset download (suppressed in offline mode). "
                             "If stuck on 'Downloading assets', check that your com.ea.games.r3_row/.depot "
                             "matches your APK build, or enable Online mode for private servers.\n");
            } else if (IsVerboseLogging()) {
                std::fprintf(stdout, "[ONLINE] CC_Component.startDownloadService() active\n");
            }
            return JniVal::Void();
        };
        d.static_methods["stopDownloadService"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["setServiceStarted"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["openSettings"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["staticStartActivity"] = [online](FakeJvm& j, uint64_t, const auto& a) {
            if (online && !a.empty()) {
                auto uri_v = j.GetInstanceField(a[0].AsObj(), "data_uri");
                if (uri_v.AsObj()) {
                    auto ustr_v = j.GetInstanceField(uri_v.AsObj(), "uri");
                    if (ustr_v.AsObj()) OpenBrowserUrlIfHttp(j.GetStringUtf(ustr_v.AsObj()));
                }
            }
            return JniVal::Void();
        };
        d.static_methods["getStaticSystemService"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(0); };
        d.static_methods["RequestMissingPermissions"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["runOnGLThreadonCloudell"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm.RegisterJniClass(std::move(d));
    }

    // ---- GetInfo ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/util/GetInfo";
        bool tv = cfg.android_tv_mode;
        d.static_methods["Initialize"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["IsInitialized"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["GetApiLevel"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(31); };
        d.static_methods["GetAdvertisingEnabled"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["GetAdvertisingID"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.static_methods["GetImei"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.static_methods["GetIsTablet"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["GetIsRunningOnTv"] = [tv](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(tv); };
        d.static_methods["GetDeviceCode"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("dm3q")); };
        d.static_methods["GetDeviceName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("samsung SM-S918B")); };
        d.static_methods["GetDeviceManufacturer"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("samsung")); };
        d.static_methods["GetDeviceModel"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("SM-S918B")); };
        d.static_methods["GetDeviceBrand"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("samsung")); };
        d.static_methods["GetDeviceFirmwareVersion"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("12")); };
        d.static_methods["GetBuildVersion"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("14.0.1")); };
        d.static_methods["GetDeviceMacAddress"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("00:00:00:00:00:00")); };
        d.static_methods["GetPackageName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("com.ea.games.r3_row")); };
        d.static_methods["GetIsAmazonDevice"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["GetIsChinaBuild"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["GetDeviceUID"] = [device_uid](FakeJvm& j, uint64_t, const auto&) {
            return JniVal::Obj(j.NewStringUtf(device_uid));
        };
        d.static_methods["GetLocalIp"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("127.0.0.1")); };
        d.static_methods["GetDeviceCountry"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("USA")); };
        d.static_methods["GetDeviceLanguage"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("en")); };
        d.static_methods["GetVolume"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Int(15); };
        d.static_methods["DoesPackageExist"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["IsPackageEnabled"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["GetRequestedPermissions"] = [](FakeJvm& j, uint64_t, const auto&) {
            return JniVal::Obj(j.NewObjectArray(0, "java/lang/String"));
        };
        d.static_methods["getAppInstallTime"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Long(1700000000000LL); };
        d.static_methods["getExternalStorageDir"] = [](FakeJvm& j, uint64_t, const auto&) {
            return JniVal::Obj(j.NewStringUtf("/storage/rr3"));
        };
        d.static_methods["getLegacyExternalStorageDirPath"] = d.static_methods["getExternalStorageDir"];
        d.static_methods["ShouldUseLegacyExternalFilesDir"] = [](FakeJvm& j, uint64_t, const auto&) {
            uint64_t b = j.NewObjectInstance("java/lang/Boolean");
            j.SetInstanceField(b, "value", JniVal::Bool(false));
            return JniVal::Obj(b);
        };
        jvm.RegisterJniClass(std::move(d));
    }

    // ---- Cloudcell HttpRequest (offline: blocks all requests; online: real async WinHTTP!) ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/HttpRequest";
        d.create_native_state = []() -> std::any { return std::make_shared<CloudcellHttpReqState>(); };
        auto get_req = [](FakeJvm& j, uint64_t self) -> std::shared_ptr<CloudcellHttpReqState> {
            auto o = j.GetJniObject(self);
            if (!o) return nullptr;
            if (!o->native_state.has_value()) {
                o->native_state = std::make_shared<CloudcellHttpReqState>();
            }
            return std::any_cast<std::shared_ptr<CloudcellHttpReqState>>(o->native_state);
        };

        // HttpRequest.init(String ua, String method, String url, byte[] data, int readCap,
        //                  long cbPtr, boolean failOnErr, double serverTime, boolean sslCheck,
        //                  boolean addDefaultHeaders, int timeoutSec)
        d.methods["init"] = [online, get_req](FakeJvm& j, uint64_t self, const auto& a) {
            auto st = get_req(j, self);
            if (!st) return JniVal::Void();
            st->user_agent = a.size() > 0 ? j.GetStringUtf(a[0].AsObj()) : "";
            st->method = a.size() > 1 ? j.GetStringUtf(a[1].AsObj()) : "GET";
            st->url = a.size() > 2 ? j.GetStringUtf(a[2].AsObj()) : "";
            st->body.clear();
            if (a.size() > 3 && a[3].AsObj()) {
                if (auto bo = j.GetJniObject(a[3].AsObj())) st->body = bo->bytes;
            }
            st->read_capacity = (a.size() > 4 && a[4].AsInt() > 0) ? a[4].AsInt() : 65536;
            st->native_ptr = a.size() > 5 ? a[5].AsLong() : 0;
            st->fail_on_error_status = a.size() > 6 ? a[6].AsBool() : false;
            st->server_time = a.size() > 7 ? a[7].AsDouble() : 0.0;
            st->ssl_check = a.size() > 8 ? a[8].AsBool() : false;
            st->add_default_headers = a.size() > 9 ? a[9].AsBool() : false;
            int to_sec = a.size() > 10 ? a[10].AsInt() : 30;
            st->timeout_ms = (to_sec > 0) ? to_sec * 1000 : 30000;
            st->headers.clear();
            st->closed.store(false, std::memory_order_release);
            st->posted.store(false, std::memory_order_release);
            if (IsVerboseLogging()) {
                std::fprintf(stdout, "[%s] HttpRequest.init %s %s (body=%zuB)\n",
                             online ? "ONLINE" : "OFFLINE",
                             st->method.c_str(), st->url.c_str(), st->body.size());
            }
            return JniVal::Void();
        };
        d.methods["addHeader"] = [get_req](FakeJvm& j, uint64_t self, const auto& a) {
            auto st = get_req(j, self);
            if (st && a.size() >= 2) {
                st->headers.emplace_back(j.GetStringUtf(a[0].AsObj()), j.GetStringUtf(a[1].AsObj()));
            }
            return JniVal::Void();
        };
        d.methods["post"] = [online, get_req, &loader, &firemint](FakeJvm& j, uint64_t self, const auto&) {
            auto st = get_req(j, self);
            if (!st || st->native_ptr == 0 || st->closed.load(std::memory_order_acquire)) {
                return JniVal::Void();
            }
            if (st->posted.exchange(true)) return JniVal::Void();

            const uint64_t err_cb = loader.FindExport("Java_com_firemonkeys_cloudcellapi_HttpRequest_errorCallback");
            const uint64_t hdr_cb = loader.FindExport("Java_com_firemonkeys_cloudcellapi_HttpRequest_headerCallback");
            const uint64_t data_cb = loader.FindExport("Java_com_firemonkeys_cloudcellapi_HttpRequest_dataCallback");
            const uint64_t done_cb = loader.FindExport("Java_com_firemonkeys_cloudcellapi_HttpRequest_completeCallback");

            if (!online) {
                if (err_cb) {
                    j.PinHandle(self);
                    firemint.PostGlCallback([&j, err_cb, self, st]() {
                        if (!st->closed.load(std::memory_order_acquire)) {
                            j.Cpu().CallGuest(err_cb, {
                                j.GetJniEnvGuestPtr(), self,
                                static_cast<uint64_t>(st->native_ptr), static_cast<uint64_t>(-1LL)
                            });
                        }
                    });
                }
                return JniVal::Void();
            }

            j.PinHandle(self);
            std::thread([&j, &firemint, self, st, err_cb, hdr_cb, data_cb, done_cb]() {
                auto result = std::make_shared<CloudcellHttpResult>(ExecuteWinHttpRequest(st));
                if (IsVerboseLogging()) {
                    std::fprintf(stdout, "[ONLINE] HTTP %s %s -> status=%d (%zu bytes, ok=%d)\n",
                                 st->method.c_str(), st->url.c_str(), result->http_status,
                                 result->resp_body.size(), result->network_ok ? 1 : 0);
                }
                firemint.PostGlCallback([&j, self, st, result, err_cb, hdr_cb, data_cb, done_cb]() {
                    if (st->closed.load(std::memory_order_acquire)) return;
                    const uint64_t env = j.GetJniEnvGuestPtr();
                    const uint64_t nptr = static_cast<uint64_t>(st->native_ptr);
                    if (!result->network_ok) {
                        if (err_cb) j.Cpu().CallGuest(err_cb, {env, self, nptr, static_cast<uint64_t>(-1LL)});
                        return;
                    }
                    if (hdr_cb) {
                        auto hmap = std::make_shared<JavaHeaderMapState>();
                        hmap->entries = result->resp_headers;
                        uint64_t map_obj = j.NewObjectInstance("java/util/Map", hmap);
                        j.Cpu().CallGuest(hdr_cb, {
                            env, self, nptr,
                            static_cast<uint64_t>(static_cast<uint32_t>(result->content_length)),
                            map_obj
                        });
                    }
                    if (st->closed.load(std::memory_order_acquire)) return;
                    if (st->fail_on_error_status && result->http_status >= 400) {
                        if (err_cb) {
                            j.Cpu().CallGuest(err_cb, {env, self, nptr, static_cast<uint64_t>(result->http_status)});
                        }
                        return;
                    }
                    if (data_cb && !result->resp_body.empty()) {
                        const size_t cap = static_cast<size_t>(std::max(4096, st->read_capacity));
                        for (size_t off = 0; off < result->resp_body.size(); off += cap) {
                            if (st->closed.load(std::memory_order_acquire)) return;
                            const size_t n = std::min(cap, result->resp_body.size() - off);
                            uint64_t jba = j.NewByteArray(result->resp_body.data() + off, n);
                            j.Cpu().CallGuest(data_cb, {env, self, nptr, jba, static_cast<uint64_t>(n)});
                        }
                    }
                    if (st->closed.load(std::memory_order_acquire)) return;
                    if (done_cb) {
                        j.Cpu().CallGuest(done_cb, {env, self, nptr, static_cast<uint64_t>(result->http_status)});
                    }
                });
            }).detach();
            return JniVal::Void();
        };
        d.methods["close"] = [get_req](FakeJvm& j, uint64_t self, const auto&) {
            if (auto st = get_req(j, self)) st->closed.store(true, std::memory_order_release);
            return JniVal::Void();
        };
        d.methods["shutdown"] = d.methods["close"];
        d.methods["isClosed"] = [get_req](FakeJvm& j, uint64_t self, const auto&) {
            auto st = get_req(j, self);
            return JniVal::Bool(!st || st->closed.load(std::memory_order_acquire));
        };
        d.methods["getSSLCheck"] = [get_req](FakeJvm& j, uint64_t self, const auto&) {
            auto st = get_req(j, self);
            return JniVal::Bool(st && st->ssl_check);
        };
        d.methods["setClosedBySSLCheck"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["getServerTime"] = [get_req](FakeJvm& j, uint64_t self, const auto&) {
            auto st = get_req(j, self);
            return JniVal::Double(st ? st->server_time : 0.0);
        };
        d.methods["getTimeoutMilliseconds"] = [get_req](FakeJvm& j, uint64_t self, const auto&) {
            auto st = get_req(j, self);
            return JniVal::Int(st ? st->timeout_ms : 30000);
        };
        jvm.RegisterJniClass(std::move(d));
    }

    // ---- Cloudcell Logging ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/Logging";
        auto log_fn = [](const char* lvl) -> JniMethodFn {
            return [lvl](FakeJvm& j, uint64_t, const auto& a) {
                std::string tag = a.size() > 0 ? j.GetStringUtf(a[0].AsObj()) : "CC";
                std::string msg = a.size() > 1 ? j.GetStringUtf(a[1].AsObj()) : "";
                if (IsVerboseLogging()) {
                    std::fprintf(stdout, "[CC_%s] %s: %s\n", lvl, tag.c_str(), msg.c_str());
                }
                return JniVal::Void();
            };
        };
        d.static_methods["CC_INFO"] = log_fn("INFO");
        d.static_methods["CC_ERROR"] = log_fn("ERR");
        d.static_methods["CC_WARN"] = log_fn("WARN");
        d.static_methods["CC_TRACE"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm.RegisterJniClass(std::move(d));
    }

    // ---- AndroidAccountManager / GooglePlayWorker / FacebookWorker / AssetManagerService / Prompts / UI ----
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/AndroidAccountManager";
        uint64_t inst = jvm.NewObjectInstance("com/firemonkeys/cloudcellapi/AndroidAccountManager");
        jvm.PinHandle(inst);
        d.static_methods["getInstance"] = [inst](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(inst); };
        d.methods["isLoggedIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["getAccountName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.methods["getAccountType"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("")); };
        d.methods["login"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["logout"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["onResume"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/GooglePlayWorker";
        d.static_methods["isSignedIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["isAvailable"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["isGooglePlayServicesAvailable"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["IsInitialPhaseComplete"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.static_methods["GetSessionChanged"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["IsSignedIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["Constructor"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.methods["IsInitialPhaseComplete"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(true); };
        d.methods["GetSessionChanged"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["GetSessionValid"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.methods["GetCurrentPlayer"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Obj(0); };
        d.static_methods["SignIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["SignOut"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["signIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["signOut"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["onStart"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["onStop"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm.RegisterJniClass(std::move(d));
    }
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/FacebookWorker";
        d.static_methods["isLoggedIn"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Bool(false); };
        d.static_methods["login"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        d.static_methods["logout"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        jvm.RegisterJniClass(std::move(d));
    }
    jvm.RegisterJniClass(JniClassDef{"com/firemonkeys/cloudcellapi/FacebookWorker$AvatarInfo"});
    jvm.RegisterJniClass(JniClassDef{"com/firemonkeys/cloudcellapi/AndroidAssetManagerService"});
    jvm.RegisterJniClass(JniClassDef{"com/firemonkeys/cloudcellapi/AppPromptManager"});
    {
        JniClassDef d;
        d.name = "com/firemonkeys/cloudcellapi/UserInterfaceManager";
        d.methods["WebBrowserCreate"] = [](FakeJvm& j, uint64_t, const auto&) {
            return JniVal::Obj(j.NewObjectInstance("java/lang/Object"));
        };
        d.methods["WebBrowserOpenUrl"] = [online](FakeJvm& j, uint64_t, const auto& a) {
            if (online && a.size() >= 3) {
                std::string full = j.GetStringUtf(a[1].AsObj()) + j.GetStringUtf(a[2].AsObj());
                OpenBrowserUrlIfHttp(full);
            }
            return JniVal::Void();
        };
        jvm.RegisterJniClass(std::move(d));
    }

    // ---- Nimble / Push / GluAnalytics / Ads / Notifications (offline no-op stubs) ----
    const char* kStubClasses[] = {
        "com/ea/nimble/bridge/NimbleCppComponentRegistrar",
        "com/ea/nimble/INimbleLocalNotifications",
        "com/ea/nimble/NimbleLocalNotifications",
        "com/ea/nimble/pushtng/IPushNotification",
        "com/ea/nimble/pushtng/PushNotification",
        "com/firemint/realracing/GoogleNativeAdManager",
        "com/firemint/realracing/LocalNotificationsCenter",
        "com/glu/plugins/gluanalytics/AnalyticsFactory",
        "com/glu/plugins/gluanalytics/GeoLocation",
        "com/glu/plugins/gluanalytics/PerfMeasure",
        "csdk/glucentralservices/util/AndroidPlatform",
        "csdk/glucentralservices/eventbus/ServerEventBus",
        "csdk/gluads/GluAdsNativeBridge",
        "csdk/gluads/Reward",
        "csdk/glucustomersupport/NativeBridge",
        "csdk/glucustomersupport/Consts",
    };
    for (const char* nm : kStubClasses) {
        JniClassDef d;
        d.name = nm;
        if (std::string(nm) == "com/ea/nimble/bridge/NimbleCppComponentRegistrar") {
            d.static_methods["register"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        } else if (std::string(nm) == "com/firemint/realracing/LocalNotificationsCenter") {
            auto noop = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
            d.static_methods["showNotification"] = noop;
            d.static_methods["CancelAllNotifications"] = noop;
            d.static_methods["CancelNotification"] = noop;
            d.static_methods["SaveNotifications"] = noop;
            d.static_methods["LoadNotifications"] = noop;
        } else if (std::string(nm) == "csdk/glucentralservices/util/AndroidPlatform") {
            d.methods["getGpuDeviceVendor"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("PC")); };
            d.methods["getGpuDeviceName"] = [](FakeJvm& j, uint64_t, const auto&) { return JniVal::Obj(j.NewStringUtf("Direct3D 11")); };
            d.methods["sendHTTPRequest"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
            d.methods["scheduleEvent"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
            d.methods["downloadFile"] = [](FakeJvm&, uint64_t, const auto&) { return JniVal::Void(); };
        }
        jvm.RegisterJniClass(std::move(d));
    }
}
