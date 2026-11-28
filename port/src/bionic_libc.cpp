#include "bionic_libc.h"
#include "config.h"

#include <dynarmic/interface/A64/a64.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// ---- Thread-local Bionic errno ----
thread_local int tls_bionic_errno = 0;

// ---- Virtual POSIX Socket Table (FDs 512..1023 -> Winsock2 SOCKET, active only in online mode) ----
// Keeps virtual socket FDs disjoint from Windows CRT file descriptors (0..511)
// while still fitting inside Linux's 1024-bit fd_set for select()/FD_SET().
constexpr int kVirtualSocketFdBase = 512;
constexpr int kVirtualSocketFdLimit = 1024;
std::mutex g_sock_mtx;
std::unordered_map<int, SOCKET> g_virt_sockets;
std::unordered_map<int, bool> g_virt_nonblock;
int g_next_sock_fd = kVirtualSocketFdBase;

int AllocVirtSocketFd(SOCKET s, bool nonblock) {
    std::lock_guard<std::mutex> lk(g_sock_mtx);
    for (int tries = 0; tries < (kVirtualSocketFdLimit - kVirtualSocketFdBase); ++tries) {
        int fd = g_next_sock_fd++;
        if (g_next_sock_fd >= kVirtualSocketFdLimit) g_next_sock_fd = kVirtualSocketFdBase;
        if (!g_virt_sockets.count(fd)) {
            g_virt_sockets[fd] = s;
            g_virt_nonblock[fd] = nonblock;
            return fd;
        }
    }
    return -1;
}

bool LookupVirtSocket(int fd, SOCKET& out_s) {
    if (fd < kVirtualSocketFdBase) return false;
    std::lock_guard<std::mutex> lk(g_sock_mtx);
    auto it = g_virt_sockets.find(fd);
    if (it == g_virt_sockets.end()) return false;
    out_s = it->second;
    return true;
}

bool CloseVirtSocket(int fd) {
    if (fd < kVirtualSocketFdBase) return false;
    SOCKET s = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lk(g_sock_mtx);
        auto it = g_virt_sockets.find(fd);
        if (it == g_virt_sockets.end()) return false;
        s = it->second;
        g_virt_sockets.erase(it);
        g_virt_nonblock.erase(fd);
    }
    closesocket(s);
    return true;
}

int WsaToLinuxErrno(int wsa) {
    switch (wsa) {
        case 0: return 0;
        case WSAEINTR: return 4;
        case WSAEBADF: return 9;
        case WSAEACCES: return 13;
        case WSAEFAULT: return 14;
        case WSAEINVAL: return 22;
        case WSAEMFILE: return 24;
        case WSAEWOULDBLOCK: return 11;   // EAGAIN
        case WSAEINPROGRESS: return 115;  // EINPROGRESS
        case WSAEALREADY: return 114;
        case WSAENOTSOCK: return 88;
        case WSAEMSGSIZE: return 90;
        case WSAEPROTONOSUPPORT: return 93;
        case WSAEOPNOTSUPP: return 95;
        case WSAEAFNOSUPPORT: return 97;
        case WSAEADDRINUSE: return 98;
        case WSAEADDRNOTAVAIL: return 99;
        case WSAENETDOWN: return 100;
        case WSAENETUNREACH: return 101;
        case WSAECONNABORTED: return 103;
        case WSAECONNRESET: return 104;
        case WSAENOBUFS: return 105;
        case WSAEISCONN: return 106;
        case WSAENOTCONN: return 107;
        case WSAESHUTDOWN: return 32;     // EPIPE
        case WSAETIMEDOUT: return 110;
        case WSAECONNREFUSED: return 111;
        case WSAEHOSTUNREACH: return 113;
        default: return 5; // EIO
    }
}

void SyncWsaErrno() {
    tls_bionic_errno = WsaToLinuxErrno(WSAGetLastError());
}

int LinuxAfToWin(int af) {
    if (af == 10) return AF_INET6; // Linux AF_INET6=10 -> Win 23
    return af;
}

int WinAfToLinux(int af) {
    if (af == AF_INET6) return 10;
    return af;
}

// Android Bionic AArch64 struct addrinfo (48 bytes, BSD order):
// ai_canonname is at +24, ai_addr is at +32, ai_next is at +40!
struct LinuxAddrinfo {
    int32_t  ai_flags;
    int32_t  ai_family;
    int32_t  ai_socktype;
    int32_t  ai_protocol;
    uint32_t ai_addrlen;
    uint32_t __pad;
    uint64_t ai_canonname;
    uint64_t ai_addr;
    uint64_t ai_next;
};
static_assert(sizeof(LinuxAddrinfo) == 48, "Bionic AArch64 addrinfo must be 48 bytes");
static_assert(offsetof(LinuxAddrinfo, ai_canonname) == 24, "Bionic ai_canonname must be at +24");
static_assert(offsetof(LinuxAddrinfo, ai_addr) == 32, "Bionic ai_addr must be at +32");
static_assert(offsetof(LinuxAddrinfo, ai_next) == 40, "Bionic ai_next must be at +40");

struct LinuxPollFd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct LinuxTimeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct LinuxHostent {
    uint64_t h_name;
    uint64_t h_aliases;
    int32_t  h_addrtype;
    int32_t  h_length;
    uint64_t h_addr_list;
};

bool CopyLinuxSockaddrToWin(const void* guest_sa, int len, sockaddr_storage& out_ss, int& out_len) {
    if (!guest_sa || len < 2 || len > static_cast<int>(sizeof(sockaddr_storage))) return false;
    std::memset(&out_ss, 0, sizeof(out_ss));
    std::memcpy(&out_ss, guest_sa, static_cast<size_t>(len));
    out_ss.ss_family = static_cast<ADDRESS_FAMILY>(LinuxAfToWin(out_ss.ss_family));
    out_len = len;
    return true;
}

void CopyWinSockaddrToLinux(const sockaddr_storage& ss, int ss_len, void* guest_sa, uint32_t* guest_len) {
    if (!guest_sa || !guest_len) return;
    sockaddr_storage tmp = ss;
    tmp.ss_family = static_cast<ADDRESS_FAMILY>(WinAfToLinux(tmp.ss_family));
    int copy_n = std::min<int>(static_cast<int>(*guest_len), ss_len);
    if (copy_n > 0) std::memcpy(guest_sa, &tmp, static_cast<size_t>(copy_n));
    *guest_len = static_cast<uint32_t>(ss_len);
}

int MapWinErrnoToLinux(int e) {
    switch (e) {
        case 0: return 0;
        case EPERM: return 1;
        case ENOENT: return 2;
        case ESRCH: return 3;
        case EINTR: return 4;
        case EIO: return 5;
        case EBADF: return 9;
        case EAGAIN: return 11;
        case ENOMEM: return 12;
        case EACCES: return 13;
        case EFAULT: return 14;
        case EBUSY: return 16;
        case EEXIST: return 17;
        case EXDEV: return 18;
        case ENODEV: return 19;
        case ENOTDIR: return 20;
        case EISDIR: return 21;
        case EINVAL: return 22;
        case ENFILE: return 23;
        case EMFILE: return 24;
        case ENOSPC: return 28;
        case ESPIPE: return 29;
        case EROFS: return 30;
        case EPIPE: return 32;
        case ERANGE: return 34;
        case ENOSYS: return 38;
        case ENOTEMPTY: return 39;
        default: return e;
    }
}

void SyncErrno() { tls_bionic_errno = MapWinErrnoToLinux(errno); }

// ---- Bionic __sF / stdout / stderr / _ctype_ / timezone ----
alignas(16) uint8_t g_bionic_sF[3 * 152]{};
FILE* g_bionic_stdout = reinterpret_cast<FILE*>(&g_bionic_sF[1 * 152]);
FILE* g_bionic_stderr = reinterpret_cast<FILE*>(&g_bionic_sF[2 * 152]);
long g_bionic_timezone = 0;

FILE* ResolveFilePtr(uint64_t guest_fp) {
    if (guest_fp == 0) return nullptr;
    uintptr_t p = static_cast<uintptr_t>(guest_fp);
    uintptr_t base = reinterpret_cast<uintptr_t>(g_bionic_sF);
    if (p == base) return stdin;
    if (p == base + 152) return stdout;
    if (p == base + 304) return stderr;
    return reinterpret_cast<FILE*>(p);
}

// Bionic/OpenBSD ctype table: index 0 is EOF (-1), indices 1..256 are 0..255.
// Flags: _U=0x01, _L=0x02, _N=0x04, _S=0x08, _P=0x10, _C=0x20, _X=0x40, _B=0x80
uint8_t g_ctype_table[257]{};
const uint8_t* g_ctype_ptr = g_ctype_table;

void InitCtypeTable() {
    static bool done = false;
    if (done) return;
    done = true;
    g_ctype_table[0] = 0;
    for (int c = 0; c < 256; ++c) {
        uint8_t f = 0;
        if (c >= 'A' && c <= 'Z') f |= 0x01;
        if (c >= 'a' && c <= 'z') f |= 0x02;
        if (c >= '0' && c <= '9') f |= 0x04;
        if (c == ' ' || (c >= '\t' && c <= '\r')) f |= 0x08;
        if (c == ' ') f |= 0x80;
        if ((c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~')) f |= 0x10;
        if (c < 0x20 || c == 0x7f) f |= 0x20;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) f |= 0x40;
        g_ctype_table[c + 1] = f;
    }
}

// ---- AAPCS64 Variadic / va_list argument reader ----
struct Aapcs64VaList {
    uint64_t __stack;
    uint64_t __gr_top;
    uint64_t __vr_top;
    int32_t  __gr_offs;
    int32_t  __vr_offs;
};

struct ArgReader {
    bool is_va_list = false;
    CpuContext* ctx = nullptr;
    int next_x = 0;
    int next_d = 0;
    size_t next_stack = 0;
    Aapcs64VaList* va = nullptr;

    static ArgReader FromDirect(CpuContext& c, int first_x, int first_d = 0) {
        ArgReader r;
        r.is_va_list = false;
        r.ctx = &c;
        r.next_x = first_x;
        r.next_d = first_d;
        r.next_stack = 0;
        return r;
    }

    static ArgReader FromVaList(Aapcs64VaList* v) {
        ArgReader r;
        r.is_va_list = true;
        r.va = v;
        return r;
    }

    uint64_t NextU64() {
        if (!is_va_list) {
            if (next_x < 8) return ctx->GetX(next_x++);
            return ctx->ReadStackU64(next_stack++);
        }
        if (va->__gr_offs < 0) {
            int32_t offs = va->__gr_offs;
            va->__gr_offs += 8;
            if (va->__gr_offs <= 0) {
                uint64_t val = 0;
                std::memcpy(&val, reinterpret_cast<const void*>(va->__gr_top + offs), sizeof(val));
                return val;
            }
        }
        uint64_t val = 0;
        std::memcpy(&val, reinterpret_cast<const void*>(va->__stack), sizeof(val));
        va->__stack += 8;
        return val;
    }

    double NextDouble() {
        if (!is_va_list) {
            if (next_d < 8) return ctx->GetD(next_d++);
            uint64_t bits = ctx->ReadStackU64(next_stack++);
            double d = 0.0;
            std::memcpy(&d, &bits, sizeof(d));
            return d;
        }
        if (va->__vr_offs < 0) {
            int32_t offs = va->__vr_offs;
            va->__vr_offs += 16;
            if (va->__vr_offs <= 0) {
                double d = 0.0;
                std::memcpy(&d, reinterpret_cast<const void*>(va->__vr_top + offs), sizeof(d));
                return d;
            }
        }
        double d = 0.0;
        std::memcpy(&d, reinterpret_cast<const void*>(va->__stack), sizeof(d));
        va->__stack += 8;
        return d;
    }
};

std::string FormatAapcs64(const char* fmt, ArgReader& args) {
    if (!fmt) return {};
    std::string out;
    out.reserve(128);
    const char* p = fmt;
    while (*p) {
        if (*p != '%') {
            out.push_back(*p++);
            continue;
        }
        const char* spec_start = p++;
        if (*p == '%') {
            out.push_back('%');
            ++p;
            continue;
        }

        std::string spec = "%";
        // Flags
        while (*p && std::strchr("-+ #0'", *p)) {
            if (*p != '\'') spec.push_back(*p);
            ++p;
        }
        // Width
        int width = -1;
        bool has_dyn_width = false;
        if (*p == '*') {
            width = static_cast<int>(args.NextU64());
            spec.push_back('*');
            has_dyn_width = true;
            ++p;
        } else {
            while (*p >= '0' && *p <= '9') spec.push_back(*p++);
        }
        // Precision
        int prec = -1;
        bool has_dyn_prec = false;
        if (*p == '.') {
            spec.push_back(*p++);
            if (*p == '*') {
                prec = static_cast<int>(args.NextU64());
                spec.push_back('*');
                has_dyn_prec = true;
                ++p;
            } else {
                while (*p >= '0' && *p <= '9') spec.push_back(*p++);
            }
        }
        // Length modifiers
        int lcount = 0;
        bool is_z = false, is_j = false, is_t = false, is_h = false, is_hh = false;
        if (*p == 'h') {
            is_h = true; ++p;
            if (*p == 'h') { is_hh = true; ++p; }
        } else if (*p == 'l') {
            lcount = 1; ++p;
            if (*p == 'l') { lcount = 2; ++p; }
        } else if (*p == 'z') { is_z = true; ++p; }
        else if (*p == 'j') { is_j = true; ++p; }
        else if (*p == 't') { is_t = true; ++p; }
        else if (*p == 'L') { ++p; }

        char conv = *p;
        if (!conv) break;
        ++p;

        char tmp[1024];
        auto fmt_one = [&](const char* final_spec, auto val) {
            int n = 0;
            if (has_dyn_width && has_dyn_prec) n = std::snprintf(tmp, sizeof(tmp), final_spec, width, prec, val);
            else if (has_dyn_width) n = std::snprintf(tmp, sizeof(tmp), final_spec, width, val);
            else if (has_dyn_prec) n = std::snprintf(tmp, sizeof(tmp), final_spec, prec, val);
            else n = std::snprintf(tmp, sizeof(tmp), final_spec, val);
            if (n > 0) {
                if (static_cast<size_t>(n) < sizeof(tmp)) {
                    out.append(tmp, static_cast<size_t>(n));
                } else {
                    std::string big(static_cast<size_t>(n) + 1, '\0');
                    if (has_dyn_width && has_dyn_prec) std::snprintf(big.data(), big.size(), final_spec, width, prec, val);
                    else if (has_dyn_width) std::snprintf(big.data(), big.size(), final_spec, width, val);
                    else if (has_dyn_prec) std::snprintf(big.data(), big.size(), final_spec, prec, val);
                    else std::snprintf(big.data(), big.size(), final_spec, val);
                    out.append(big.data(), static_cast<size_t>(n));
                }
            }
        };

        switch (conv) {
            case 'd': case 'i': {
                uint64_t raw = args.NextU64();
                bool is64 = (lcount >= 1 || is_z || is_j || is_t);
                int64_t v = is64 ? static_cast<int64_t>(raw)
                                 : (is_hh ? static_cast<int8_t>(raw)
                                          : (is_h ? static_cast<int16_t>(raw) : static_cast<int32_t>(raw)));
                spec += "lld";
                fmt_one(spec.c_str(), static_cast<long long>(v));
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                uint64_t raw = args.NextU64();
                bool is64 = (lcount >= 1 || is_z || is_j || is_t);
                uint64_t v = is64 ? raw
                                  : (is_hh ? static_cast<uint8_t>(raw)
                                           : (is_h ? static_cast<uint16_t>(raw) : static_cast<uint32_t>(raw)));
                spec += "ll";
                spec.push_back(conv);
                fmt_one(spec.c_str(), static_cast<unsigned long long>(v));
                break;
            }
            case 'p': {
                uint64_t raw = args.NextU64();
                spec.push_back('p');
                fmt_one(spec.c_str(), reinterpret_cast<void*>(raw));
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double d = args.NextDouble();
                spec.push_back(conv);
                fmt_one(spec.c_str(), d);
                break;
            }
            case 'c': {
                int c = static_cast<int>(args.NextU64());
                spec.push_back('c');
                fmt_one(spec.c_str(), c);
                break;
            }
            case 's': {
                const char* s = reinterpret_cast<const char*>(args.NextU64());
                spec.push_back('s');
                fmt_one(spec.c_str(), s ? s : "(null)");
                break;
            }
            case 'n': {
                uint64_t ptr = args.NextU64();
                if (ptr) {
                    if (lcount >= 1) *reinterpret_cast<int64_t*>(ptr) = static_cast<int64_t>(out.size());
                    else *reinterpret_cast<int32_t*>(ptr) = static_cast<int32_t>(out.size());
                }
                break;
            }
            default:
                out.append(spec_start, static_cast<size_t>(p - spec_start));
                break;
        }
    }
    return out;
}

int SscanfAapcs64(const char* input, const char* fmt, ArgReader& args) {
    if (!input || !fmt) return -1;
    const char* s = input;
    const char* f = fmt;
    int assigned = 0;

    while (*f) {
        if (std::isspace(static_cast<unsigned char>(*f))) {
            while (std::isspace(static_cast<unsigned char>(*f))) ++f;
            while (std::isspace(static_cast<unsigned char>(*s))) ++s;
            continue;
        }
        if (*f != '%') {
            if (*s != *f) break;
            ++s; ++f;
            continue;
        }
        ++f;
        if (*f == '%') {
            if (*s != '%') break;
            ++s; ++f;
            continue;
        }
        bool suppress = false;
        if (*f == '*') { suppress = true; ++f; }
        int width = 0;
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); ++f; }
        int lcount = 0;
        bool is_h = false, is_hh = false;
        if (*f == 'h') { is_h = true; ++f; if (*f == 'h') { is_hh = true; ++f; } }
        else if (*f == 'l') { lcount = 1; ++f; if (*f == 'l') { lcount = 2; ++f; } }
        else if (*f == 'z' || *f == 'j' || *f == 't') { lcount = 1; ++f; }

        char conv = *f++;
        if (conv == 'n') {
            if (!suppress) {
                uint64_t ptr = args.NextU64();
                if (ptr) *reinterpret_cast<int32_t*>(ptr) = static_cast<int32_t>(s - input);
            }
            continue;
        }
        if (conv != 'c' && conv != '[') {
            while (std::isspace(static_cast<unsigned char>(*s))) ++s;
        }
        if (!*s) return assigned > 0 ? assigned : -1;

        auto read_token = [&](int max_w) {
            std::string tok;
            while (*s && !std::isspace(static_cast<unsigned char>(*s)) && (max_w == 0 || static_cast<int>(tok.size()) < max_w)) {
                tok.push_back(*s++);
            }
            return tok;
        };

        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            int base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o' ? 8 : (conv == 'i' ? 0 : 10));
            std::string slice = width > 0 ? std::string(s, std::min<size_t>(static_cast<size_t>(width), std::strlen(s))) : std::string(s);
            char* endp = nullptr;
            uint64_t val = (conv == 'd' || conv == 'i')
                ? static_cast<uint64_t>(std::strtoll(slice.c_str(), &endp, base))
                : std::strtoull(slice.c_str(), &endp, base);
            if (endp == slice.c_str()) break;
            s += (endp - slice.c_str());
            if (!suppress) {
                uint64_t ptr = args.NextU64();
                if (ptr) {
                    if (lcount >= 1) *reinterpret_cast<uint64_t*>(ptr) = val;
                    else if (is_hh) *reinterpret_cast<uint8_t*>(ptr) = static_cast<uint8_t>(val);
                    else if (is_h) *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(val);
                    else *reinterpret_cast<uint32_t*>(ptr) = static_cast<uint32_t>(val);
                }
                ++assigned;
            }
        } else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'F' || conv == 'E' || conv == 'G') {
            std::string slice = width > 0 ? std::string(s, std::min<size_t>(static_cast<size_t>(width), std::strlen(s))) : std::string(s);
            char* endp = nullptr;
            double val = std::strtod(slice.c_str(), &endp);
            if (endp == slice.c_str()) break;
            s += (endp - slice.c_str());
            if (!suppress) {
                uint64_t ptr = args.NextU64();
                if (ptr) {
                    if (lcount >= 1) *reinterpret_cast<double*>(ptr) = val;
                    else *reinterpret_cast<float*>(ptr) = static_cast<float>(val);
                }
                ++assigned;
            }
        } else if (conv == 's') {
            std::string tok = read_token(width);
            if (tok.empty()) break;
            if (!suppress) {
                char* dst = reinterpret_cast<char*>(args.NextU64());
                if (dst) std::memcpy(dst, tok.c_str(), tok.size() + 1);
                ++assigned;
            }
        } else if (conv == 'c') {
            int n = width > 0 ? width : 1;
            char* dst = suppress ? nullptr : reinterpret_cast<char*>(args.NextU64());
            int got = 0;
            while (got < n && *s) {
                if (dst) dst[got] = *s;
                ++s; ++got;
            }
            if (got < n) break;
            if (!suppress) ++assigned;
        } else if (conv == '[') {
            bool invert = false;
            if (*f == '^') { invert = true; ++f; }
            bool set[256]{};
            bool first = true;
            while (*f && (*f != ']' || first)) {
                first = false;
                unsigned char c1 = static_cast<unsigned char>(*f++);
                if (*f == '-' && f[1] && f[1] != ']') {
                    ++f;
                    unsigned char c2 = static_cast<unsigned char>(*f++);
                    for (unsigned int c = c1; c <= c2; ++c) set[c] = true;
                } else {
                    set[c1] = true;
                }
            }
            if (*f == ']') ++f;
            std::string tok;
            while (*s && (width == 0 || static_cast<int>(tok.size()) < width)) {
                bool in = set[static_cast<unsigned char>(*s)];
                if (invert ? in : !in) break;
                tok.push_back(*s++);
            }
            if (tok.empty()) break;
            if (!suppress) {
                char* dst = reinterpret_cast<char*>(args.NextU64());
                if (dst) std::memcpy(dst, tok.c_str(), tok.size() + 1);
                ++assigned;
            }
        } else {
            break;
        }
    }
    return assigned;
}

// ---- Stat / Dir helpers ----
void FillBionicStat(const struct _stat64& ws, BionicStat64& bs) {
    std::memset(&bs, 0, sizeof(bs));
    bs.st_dev = ws.st_dev;
    bs.st_ino = ws.st_ino ? ws.st_ino : 1;
    uint32_t mode = 0;
    if (ws.st_mode & _S_IFDIR) mode |= 0040000 | 0755;
    else mode |= 0100000 | 0644;
    bs.st_mode = mode;
    bs.st_nlink = ws.st_nlink ? ws.st_nlink : 1;
    bs.st_size = ws.st_size;
    bs.st_blksize = 4096;
    bs.st_blocks = (ws.st_size + 511) / 512;
    bs.st_atime_sec = ws.st_atime;
    bs.st_mtime_sec = ws.st_mtime;
    bs.st_ctime_sec = ws.st_ctime;
}

struct HostDir {
    std::vector<std::pair<std::string, uint8_t>> entries;
    size_t idx = 0;
    BionicDirent cur{};
};

// ---- Pthread / Sem / Emutls state ----
struct HostMutex {
    std::recursive_mutex mtx;
};

struct HostCond {
    std::condition_variable_any cv;
};

struct HostRwLock {
    std::shared_mutex rw;
};

struct HostSem {
    std::mutex mtx;
    std::condition_variable cv;
    unsigned int count = 0;
};

struct HostThread {
    std::thread th;
    std::atomic<bool> done{false};
    uint64_t retval = 0;
    bool detached = false;
};

std::mutex g_sync_map_mtx;
std::unordered_map<uint64_t, std::unique_ptr<HostMutex>> g_mutexes;
std::unordered_map<uint64_t, std::unique_ptr<HostCond>> g_conds;
std::unordered_map<uint64_t, std::unique_ptr<HostRwLock>> g_rwlocks;
std::unordered_map<uint64_t, std::unique_ptr<HostSem>> g_sems;
std::unordered_map<uint64_t, std::unique_ptr<HostThread>> g_threads;
std::atomic<uint64_t> g_next_tid{100};
std::atomic<uint32_t> g_next_key{1};

HostMutex& GetMutex(uint64_t addr) {
    std::lock_guard<std::mutex> lk(g_sync_map_mtx);
    auto& ptr = g_mutexes[addr];
    if (!ptr) ptr = std::make_unique<HostMutex>();
    return *ptr;
}
HostCond& GetCond(uint64_t addr) {
    std::lock_guard<std::mutex> lk(g_sync_map_mtx);
    auto& ptr = g_conds[addr];
    if (!ptr) ptr = std::make_unique<HostCond>();
    return *ptr;
}
HostRwLock& GetRwLock(uint64_t addr) {
    std::lock_guard<std::mutex> lk(g_sync_map_mtx);
    auto& ptr = g_rwlocks[addr];
    if (!ptr) ptr = std::make_unique<HostRwLock>();
    return *ptr;
}
HostSem& GetSem(uint64_t addr) {
    std::lock_guard<std::mutex> lk(g_sync_map_mtx);
    auto& ptr = g_sems[addr];
    if (!ptr) ptr = std::make_unique<HostSem>();
    return *ptr;
}

thread_local std::unordered_map<uint32_t, uint64_t> tls_pthread_keys;
thread_local std::unordered_map<uint64_t, std::vector<uint8_t>> tls_emutls_slots;
thread_local uint64_t tls_my_pthread_id = 1;

// ---- Guest qsort helper (re-entrant, thread-safe) ----
void GuestQuickSort(CpuRuntime& cpu, uint8_t* base, size_t n, size_t sz, uint64_t cmp_fn, std::vector<uint8_t>& tmp) {
    if (n <= 1) return;
    // Insertion sort for small slices
    if (n <= 16) {
        for (size_t i = 1; i < n; ++i) {
            std::memcpy(tmp.data(), base + i * sz, sz);
            size_t j = i;
            while (j > 0) {
                int c = static_cast<int>(cpu.CallGuest(cmp_fn, {reinterpret_cast<uint64_t>(base + (j - 1) * sz),
                                                                reinterpret_cast<uint64_t>(tmp.data())}));
                if (c <= 0) break;
                std::memcpy(base + j * sz, base + (j - 1) * sz, sz);
                --j;
            }
            std::memcpy(base + j * sz, tmp.data(), sz);
        }
        return;
    }
    // Pivot = middle element moved to end
    size_t mid = n / 2;
    auto swap_elem = [&](size_t a, size_t b) {
        if (a == b) return;
        std::memcpy(tmp.data(), base + a * sz, sz);
        std::memcpy(base + a * sz, base + b * sz, sz);
        std::memcpy(base + b * sz, tmp.data(), sz);
    };
    swap_elem(mid, n - 1);
    uint64_t pivot_addr = reinterpret_cast<uint64_t>(base + (n - 1) * sz);
    size_t store = 0;
    for (size_t i = 0; i < n - 1; ++i) {
        int c = static_cast<int>(cpu.CallGuest(cmp_fn, {reinterpret_cast<uint64_t>(base + i * sz), pivot_addr}));
        if (c < 0) {
            swap_elem(i, store++);
        }
    }
    swap_elem(store, n - 1);
    GuestQuickSort(cpu, base, store, sz, cmp_fn, tmp);
    GuestQuickSort(cpu, base + (store + 1) * sz, n - store - 1, sz, cmp_fn, tmp);
}

// ---- Custom dlopen handles ----
struct DlHandle {
    std::string name;
    bool is_self = false;
};
std::mutex g_dl_mtx;
std::vector<std::unique_ptr<DlHandle>> g_dl_handles;
thread_local std::string tls_dlerror;

// ---- IEEE754 double -> binary128 helper (for AArch64 strtold/wcstold) ----
Dynarmic::A64::Vector DoubleToBinary128(double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(d));
    uint64_t sign = (bits >> 63) & 1;
    uint64_t exp = (bits >> 52) & 0x7FF;
    uint64_t frac = bits & 0x000FFFFFFFFFFFFFULL;
    if (exp == 0 && frac == 0) {
        return {0, sign << 63};
    }
    if (exp == 0x7FF) {
        return {0, (sign << 63) | (0x7FFFULL << 48) | (frac ? (1ULL << 47) : 0)};
    }
    uint64_t exp128 = exp - 1023 + 16383;
    uint64_t hi = (sign << 63) | (exp128 << 48) | (frac >> 4);
    uint64_t lo = frac << 60;
    return {lo, hi};
}

// ---- Linux tm struct (56 bytes on AArch64 LP64) ----
struct LinuxTm {
    int32_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    int32_t _pad;
    int64_t tm_gmtoff;
    uint64_t tm_zone;
};
static const char* kUtcZone = "UTC";
thread_local LinuxTm tls_tm{};

void FillLinuxTm(const std::tm& src, LinuxTm& dst) {
    dst.tm_sec = src.tm_sec;
    dst.tm_min = src.tm_min;
    dst.tm_hour = src.tm_hour;
    dst.tm_mday = src.tm_mday;
    dst.tm_mon = src.tm_mon;
    dst.tm_year = src.tm_year;
    dst.tm_wday = src.tm_wday;
    dst.tm_yday = src.tm_yday;
    dst.tm_isdst = src.tm_isdst;
    dst._pad = 0;
    dst.tm_gmtoff = 0;
    dst.tm_zone = reinterpret_cast<uint64_t>(kUtcZone);
}

// ---- Guest-side dl_phdr_info scratch ----
struct LinuxDlPhdrInfo {
    uint64_t dlpi_addr;
    uint64_t dlpi_name;
    uint64_t dlpi_phdr;
    uint16_t dlpi_phnum;
    uint16_t _pad[3];
};

} // namespace

int* BionicRuntime::ThreadErrnoPtr() { return &tls_bionic_errno; }

BionicRuntime::BionicRuntime(CpuRuntime& cpu, ElfLoader& loader, LayeredVfs& vfs)
    : cpu_(cpu), loader_(loader), vfs_(vfs) {
    InitCtypeTable();
}

void BionicRuntime::SetOnlineMode(bool enabled) {
    online_mode_ = enabled;
    if (enabled) {
        static std::once_flag wsa_once;
        std::call_once(wsa_once, []() {
            WSADATA wd{};
            WSAStartup(MAKEWORD(2, 2), &wd);
        });
    }
}

void BionicRuntime::RegisterAll() {
    auto fn = [&](const std::string& name, SvcHandlerFn h) {
        loader_.RegisterHostFunction(name, std::move(h));
    };

    // ---- Data symbols ----
    loader_.RegisterHostData("__sF", g_bionic_sF);
    loader_.RegisterHostData("stdout", &g_bionic_stdout);
    loader_.RegisterHostData("stderr", &g_bionic_stderr);
    loader_.RegisterHostData("_ctype_", &g_ctype_ptr);
    loader_.RegisterHostData("timezone", &g_bionic_timezone);

    // ---- Core runtime / errno / abort / assert / atexit ----
    fn("__errno", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(&tls_bionic_errno)); });
    fn("__cxa_atexit", [](CpuContext& c) { c.SetX(0, 0); });
    fn("__cxa_finalize", [](CpuContext& c) { c.SetX(0, 0); });
    fn("__cxa_thread_atexit_impl", [](CpuContext& c) { c.SetX(0, 0); });
    fn("__register_atfork", [](CpuContext& c) { c.SetX(0, 0); });
    fn("android_set_abort_message", [](CpuContext& c) {
        const char* msg = reinterpret_cast<const char*>(c.GetX(0));
        if (msg) std::fprintf(stderr, "[ANDROID ABORT MSG] %s\n", msg);
    });
    fn("__assert2", [](CpuContext& c) {
        std::fprintf(stderr, "[ANDROID ASSERT] %s:%d %s: %s\n",
                     reinterpret_cast<const char*>(c.GetX(0)),
                     static_cast<int>(c.GetW(1)),
                     reinterpret_cast<const char*>(c.GetX(2)),
                     reinterpret_cast<const char*>(c.GetX(3)));
        std::fflush(stderr);
        std::abort();
    });
    fn("__stack_chk_fail", [](CpuContext& c) {
        std::fprintf(stderr, "[STACK_CHK_FAIL] LR=0x%016llx\n", static_cast<unsigned long long>(c.GetLR()));
        std::abort();
    });
    fn("abort", [](CpuContext& c) {
        std::fprintf(stderr, "[GUEST ABORT] LR=0x%016llx\n", static_cast<unsigned long long>(c.GetLR()));
        std::abort();
    });
    fn("exit", [](CpuContext& c) { std::exit(static_cast<int>(c.GetW(0))); });
    fn("raise", [](CpuContext& c) {
        std::fprintf(stderr, "[GUEST RAISE] sig=%d LR=0x%016llx\n",
                     static_cast<int>(c.GetW(0)), static_cast<unsigned long long>(c.GetLR()));
        c.SetX(0, 0);
    });
    fn("signal", [](CpuContext& c) { c.SetX(0, 0); });
    fn("sigaction", [](CpuContext& c) { c.SetX(0, 0); });
    fn("ptrace", [](CpuContext& c) { c.SetX(0, 0); }); // Assert helper in memory_profiler.cpp
    fn("getpid", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(GetCurrentProcessId())); });
    fn("geteuid", [](CpuContext& c) { c.SetW(0, 10000); });
    fn("getenv", [](CpuContext& c) {
        const char* k = reinterpret_cast<const char*>(c.GetX(0));
        c.SetX(0, k ? reinterpret_cast<uint64_t>(std::getenv(k)) : 0);
    });
    fn("sysconf", [](CpuContext& c) {
        int name = static_cast<int>(c.GetW(0));
        switch (name) {
            case 39: // _SC_PAGESIZE (Android Bionic)
            case 30: // _SC_PAGESIZE (glibc)
                c.SetX(0, 4096);
                break;
            case 97: // _SC_NPROCESSORS_ONLN (Bionic)
            case 96: // _SC_NPROCESSORS_CONF (Bionic)
            case 84: // glibc _SC_NPROCESSORS_ONLN
                c.SetX(0, 8);
                break;
            case 2: // _SC_CLK_TCK
                c.SetX(0, 100);
                break;
            default:
                c.SetX(0, 4096);
                break;
        }
    });
    fn("getauxval", [](CpuContext& c) {
        uint64_t type = c.GetX(0);
        if (type == 16) { // AT_HWCAP: FP | ASIMD | AES | PMULL | SHA1 | SHA2 | CRC32
            c.SetX(0, 0xFF);
        } else if (type == 6) { // AT_PAGESZ
            c.SetX(0, 4096);
        } else {
            c.SetX(0, 0);
        }
    });
    fn("getentropy", [](CpuContext& c) {
        void* buf = reinterpret_cast<void*>(c.GetX(0));
        size_t len = static_cast<size_t>(c.GetX(1));
        if (len > 256 || !buf) {
            tls_bionic_errno = 5;
            c.SetX(0, static_cast<uint64_t>(-1));
            return;
        }
        BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        c.SetX(0, 0);
    });
    fn("syscall", [](CpuContext& c) {
        int64_t nr = static_cast<int64_t>(c.GetX(0));
        if (nr == 0x116) { // SYS_getrandom
            void* buf = reinterpret_cast<void*>(c.GetX(1));
            size_t len = static_cast<size_t>(c.GetX(2));
            BCryptGenRandom(nullptr, static_cast<PUCHAR>(buf), static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            c.SetX(0, len);
        } else if (nr == 178) { // SYS_gettid
            c.SetX(0, static_cast<uint64_t>(GetCurrentThreadId()));
        } else {
            // Includes 0x11c SYS_mlock2 -> ENOSYS (38) cleanly triggers OpenSSL fallback
            tls_bionic_errno = 38;
            c.SetX(0, static_cast<uint64_t>(-1));
        }
    });

    // ---- Android logging & syslog ----
    fn("__android_log_print", [](CpuContext& c) {
        if (!IsVerboseLogging()) {
            c.SetW(0, 0);
            return;
        }
        int prio = static_cast<int>(c.GetW(0));
        const char* tag = reinterpret_cast<const char*>(c.GetX(1));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(2));
        auto r = ArgReader::FromDirect(c, 3, 0);
        std::string msg = FormatAapcs64(fmt, r);
        std::fprintf(stderr, "[LOG/%d %s] %s\n", prio, tag ? tag : "?", msg.c_str());
        c.SetW(0, static_cast<uint32_t>(msg.size()));
    });
    fn("__android_log_vprint", [](CpuContext& c) {
        if (!IsVerboseLogging()) {
            c.SetW(0, 0);
            return;
        }
        int prio = static_cast<int>(c.GetW(0));
        const char* tag = reinterpret_cast<const char*>(c.GetX(1));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(2));
        auto* va = reinterpret_cast<Aapcs64VaList*>(c.GetX(3));
        Aapcs64VaList copy = *va;
        auto r = ArgReader::FromVaList(&copy);
        std::string msg = FormatAapcs64(fmt, r);
        std::fprintf(stderr, "[LOG/%d %s] %s\n", prio, tag ? tag : "?", msg.c_str());
        c.SetW(0, static_cast<uint32_t>(msg.size()));
    });
    fn("openlog", [](CpuContext&) {});
    fn("closelog", [](CpuContext&) {});
    fn("syslog", [](CpuContext&) {});

    // ---- Memory allocation / mmap ----
    // Track >16B _aligned_malloc allocations so free()/realloc() never corrupt MSVC heap.
    static std::mutex s_aligned_mtx;
    static std::unordered_map<void*, size_t> s_aligned_allocs;
    static std::atomic<size_t> s_aligned_count{0};

    auto alloc_aligned_safe = [](size_t align, size_t sz) -> void* {
        size_t req = sz ? sz : 1;
        if (align <= 16) return std::malloc(req);
        // Round alignment up to power of two
        size_t a = 16;
        while (a < align && a < (1ULL << 24)) a <<= 1;
        void* p = _aligned_malloc(req, a);
        if (p) {
            std::lock_guard<std::mutex> lk(s_aligned_mtx);
            s_aligned_allocs[p] = req;
            s_aligned_count.fetch_add(1, std::memory_order_release);
        }
        return p;
    };

    fn("malloc", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::malloc(static_cast<size_t>(c.GetX(0))))); });
    fn("calloc", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::calloc(static_cast<size_t>(c.GetX(0)), static_cast<size_t>(c.GetX(1))))); });
    fn("realloc", [](CpuContext& c) {
        void* ptr = reinterpret_cast<void*>(c.GetX(0));
        size_t newsz = static_cast<size_t>(c.GetX(1));
        if (ptr && s_aligned_count.load(std::memory_order_acquire) > 0) {
            std::lock_guard<std::mutex> lk(s_aligned_mtx);
            auto it = s_aligned_allocs.find(ptr);
            if (it != s_aligned_allocs.end()) {
                size_t oldsz = it->second;
                void* np = std::malloc(newsz ? newsz : 1);
                if (np) {
                    std::memcpy(np, ptr, std::min(oldsz, newsz));
                    _aligned_free(ptr);
                    s_aligned_allocs.erase(it);
                    s_aligned_count.fetch_sub(1, std::memory_order_release);
                }
                c.SetX(0, reinterpret_cast<uint64_t>(np));
                return;
            }
        }
        c.SetX(0, reinterpret_cast<uint64_t>(std::realloc(ptr, newsz)));
    });
    fn("free", [](CpuContext& c) {
        void* ptr = reinterpret_cast<void*>(c.GetX(0));
        if (!ptr) return;
        if (s_aligned_count.load(std::memory_order_acquire) > 0) {
            std::lock_guard<std::mutex> lk(s_aligned_mtx);
            auto it = s_aligned_allocs.find(ptr);
            if (it != s_aligned_allocs.end()) {
                _aligned_free(ptr);
                s_aligned_allocs.erase(it);
                s_aligned_count.fetch_sub(1, std::memory_order_release);
                return;
            }
        }
        std::free(ptr);
    });
    fn("posix_memalign", [alloc_aligned_safe](CpuContext& c) {
        auto** out = reinterpret_cast<void**>(c.GetX(0));
        size_t align = static_cast<size_t>(c.GetX(1));
        size_t sz = static_cast<size_t>(c.GetX(2));
        if (!out) { c.SetW(0, 22); return; }
        void* p = alloc_aligned_safe(align, sz);
        if (!p) { c.SetW(0, 12); return; }
        *out = p;
        c.SetW(0, 0);
    });
    fn("memalign", [alloc_aligned_safe](CpuContext& c) {
        size_t align = static_cast<size_t>(c.GetX(0));
        size_t sz = static_cast<size_t>(c.GetX(1));
        c.SetX(0, reinterpret_cast<uint64_t>(alloc_aligned_safe(align, sz)));
    });
    fn("mmap", [](CpuContext& c) {
        size_t len = static_cast<size_t>(c.GetX(1));
        int flags = static_cast<int>(c.GetW(3));
        int fd = static_cast<int>(c.GetW(4));
        int64_t off = static_cast<int64_t>(c.GetX(5));
        size_t alloc_len = len ? len : 4096;
        void* p = VirtualAlloc(nullptr, alloc_len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) {
            tls_bionic_errno = 12;
            c.SetX(0, static_cast<uint64_t>(-1));
            return;
        }
        // Linux MAP_ANONYMOUS = 0x20. For file-backed mappings, read file bytes into the allocation.
        if ((flags & 0x20) == 0 && fd >= 0 && len > 0) {
            intptr_t osf = _get_osfhandle(fd);
            if (osf != -1) {
                HANDLE h = reinterpret_cast<HANDLE>(osf);
                LARGE_INTEGER zero{}, saved{}, target{};
                bool have_pos = SetFilePointerEx(h, zero, &saved, FILE_CURRENT) != 0;
                target.QuadPart = off;
                if (SetFilePointerEx(h, target, nullptr, FILE_BEGIN)) {
                    uint8_t* dst = static_cast<uint8_t*>(p);
                    size_t remaining = len;
                    while (remaining > 0) {
                        DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 0x40000000ULL));
                        DWORD got = 0;
                        if (!ReadFile(h, dst, chunk, &got, nullptr) || got == 0) break;
                        dst += got;
                        remaining -= got;
                    }
                }
                if (have_pos) SetFilePointerEx(h, saved, nullptr, FILE_BEGIN);
            }
        }
        c.SetX(0, reinterpret_cast<uint64_t>(p));
    });
    fn("munmap", [](CpuContext& c) {
        void* p = reinterpret_cast<void*>(c.GetX(0));
        if (p) VirtualFree(p, 0, MEM_RELEASE);
        c.SetW(0, 0);
    });
    fn("mprotect", [](CpuContext& c) { c.SetW(0, 0); });
    fn("madvise", [](CpuContext& c) { c.SetW(0, 0); });
    fn("mlock", [](CpuContext& c) { c.SetW(0, 0); });
    fn("mremap", [](CpuContext& c) {
        void* oldp = reinterpret_cast<void*>(c.GetX(0));
        size_t oldsz = static_cast<size_t>(c.GetX(1));
        size_t newsz = static_cast<size_t>(c.GetX(2));
        void* newp = VirtualAlloc(nullptr, newsz ? newsz : 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!newp) { c.SetX(0, static_cast<uint64_t>(-1)); return; }
        if (oldp) {
            std::memcpy(newp, oldp, std::min(oldsz, newsz));
            VirtualFree(oldp, 0, MEM_RELEASE);
        }
        c.SetX(0, reinterpret_cast<uint64_t>(newp));
    });

    // ---- Memory / String ops + Bionic _chk wrappers ----
    // Pure AArch64 machine-code implementations running 100% inside Dynarmic JIT + 64-bit fastmem
    // (zero SVC exits! Eliminates ~13.6M/frame JIT exits during RR3 menu LTS string scans).
    {
        // Verified AArch64 encodings (disassembly-verified via Capstone):
        const uint64_t code_strlen = cpu_.AllocateCodeStub({
            0xAA0003E1u, // mov x1, x0
            0xF240083Fu, // tst x1, #7
            0x54000080u, // b.eq word_prep
            0x38401422u, // ldrb w2, [x1], #1
            0x35FFFFA2u, // cbnz w2, align_loop
            0x14000014u, // b byte_done
            0xD2802024u, 0xF2A02024u, 0xF2C02024u, 0xF2E02024u, // x4 = 0x0101010101010101
            0xD2901005u, 0xF2B01005u, 0xF2D01005u, 0xF2F01005u, // x5 = 0x8080808080808080
            0xF8408422u, // ldr x2, [x1], #8
            0xCB040043u, // sub x3, x2, x4
            0x8A220063u, // bic x3, x3, x2
            0xEA050063u, // ands x3, x3, x5
            0x54FFFF80u, // b.eq word_loop
            0xD1002021u, // sub x1, x1, #8
            0xDAC00063u, // rbit x3, x3
            0xDAC01063u, // clz x3, x3
            0xCB000021u, // sub x1, x1, x0
            0x8B430C20u, // add x0, x1, x3, lsr #3
            0xD65F03C0u, // ret
            0xCB000020u, // byte_done: sub x0, x1, x0
            0xD1000400u, // sub x0, x0, #1
            0xD65F03C0u, // ret
        });
        const uint64_t code_memcmp = cpu_.AllocateCodeStub({
            0xF100205Fu, // cmp x2, #8
            0x54000123u, // b.lo byte_loop
            0xF9400003u, // ldr x3, [x0]
            0xF9400024u, // ldr x4, [x1]
            0xEB04007Fu, // cmp x3, x4
            0x540000A1u, // b.ne byte_loop
            0x91002000u, // add x0, x0, #8
            0x91002021u, // add x1, x1, #8
            0xD1002042u, // sub x2, x2, #8
            0x17FFFFF7u, // b loop8
            0xB40000E2u, // byte_loop: cbz x2, equal
            0x38401403u, // ldrb w3, [x0], #1
            0x38401424u, // ldrb w4, [x1], #1
            0x6B040063u, // subs w3, w3, w4
            0x540000A1u, // b.ne diff
            0xD1000442u, // sub x2, x2, #1
            0x17FFFFFAu, // b byte_loop
            0x52800000u, // equal: mov w0, #0
            0xD65F03C0u, // ret
            0x2A0303E0u, // diff: mov w0, w3
            0xD65F03C0u, // ret
        });
        const uint64_t code_strcmp = cpu_.AllocateCodeStub({
            0x38401402u, // ldrb w2, [x0], #1
            0x38401423u, // ldrb w3, [x1], #1
            0x6B03005Fu, // cmp w2, w3
            0x54000081u, // b.ne diff
            0x35FFFF82u, // cbnz w2, loop
            0x52800000u, // mov w0, #0
            0xD65F03C0u, // ret
            0x4B030040u, // diff: sub w0, w2, w3
            0xD65F03C0u, // ret
        });
        const uint64_t code_strncmp = cpu_.AllocateCodeStub({
            0xB4000102u, // cbz x2, equal
            0x38401403u, // ldrb w3, [x0], #1
            0x38401424u, // ldrb w4, [x1], #1
            0x6B04007Fu, // cmp w3, w4
            0x540000C1u, // b.ne diff
            0x34000063u, // cbz w3, equal
            0xD1000442u, // sub x2, x2, #1
            0x17FFFFF9u, // b loop
            0x52800000u, // equal: mov w0, #0
            0xD65F03C0u, // ret
            0x4B040060u, // diff: sub w0, w3, w4
            0xD65F03C0u, // ret
        });
        const uint64_t code_memcpy = cpu_.AllocateCodeStub({
            0xAA0003E5u, // mov x5, x0
            0xF100405Fu, // cmp x2, #16
            0x540000A3u, // b.lo byte_loop
            0xA8C11023u, // ldp x3, x4, [x1], #16
            0xA88110A3u, // stp x3, x4, [x5], #16
            0xD1004042u, // sub x2, x2, #16
            0x17FFFFFBu, // b loop16
            0xB40000A2u, // byte_loop: cbz x2, done
            0x38401423u, // ldrb w3, [x1], #1
            0x380014A3u, // strb w3, [x5], #1
            0xD1000442u, // sub x2, x2, #1
            0x17FFFFFCu, // b byte_loop
            0xD65F03C0u, // done: ret
        });
        const uint64_t code_memset = cpu_.AllocateCodeStub({
            0xAA0003E5u, // mov x5, x0
            0x92401C21u, // and x1, x1, #0xff
            0xD2802024u, 0xF2A02024u, 0xF2C02024u, 0xF2E02024u, // x4 = 0x0101010101010101
            0x9B047C23u, // mul x3, x1, x4
            0xF100405Fu, // cmp x2, #16
            0x54000083u, // b.lo byte_loop
            0xA8810CA3u, // stp x3, x3, [x5], #16
            0xD1004042u, // sub x2, x2, #16
            0x17FFFFFCu, // b loop16
            0xB4000082u, // byte_loop: cbz x2, done
            0x380014A1u, // strb w1, [x5], #1
            0xD1000442u, // sub x2, x2, #1
            0x17FFFFFDu, // b byte_loop
            0xD65F03C0u, // done: ret
        });
        const uint64_t code_memmove = cpu_.AllocateCodeStub({
            0xEB01001Fu, // cmp x0, x1
            0x540003C0u, // b.eq done
            0x54000083u, // b.lo fwd
            0x8B020026u, // add x6, x1, x2
            0xEB06001Fu, // cmp x0, x6
            0x540001A3u, // b.lo bwd
            0xAA0003E5u, // fwd: mov x5, x0
            0xF100405Fu, // cmp x2, #16
            0x540000A3u, // b.lo fwd_byte
            0xA8C11023u, // ldp x3, x4, [x1], #16
            0xA88110A3u, // stp x3, x4, [x5], #16
            0xD1004042u, // sub x2, x2, #16
            0x17FFFFFBu, // b fwd_loop16
            0xB4000242u, // fwd_byte: cbz x2, done
            0x38401423u, // ldrb w3, [x1], #1
            0x380014A3u, // strb w3, [x5], #1
            0xD1000442u, // sub x2, x2, #1
            0x17FFFFFCu, // b fwd_byte
            0x8B020005u, // bwd: add x5, x0, x2
            0x8B020026u, // add x6, x1, x2
            0xF100205Fu, // cmp x2, #8
            0x540000A3u, // b.lo bwd_byte
            0xF85F8CC3u, // ldr x3, [x6, #-8]!
            0xF81F8CA3u, // str x3, [x5, #-8]!
            0xD1002042u, // sub x2, x2, #8
            0x17FFFFFBu, // b bwd_loop8
            0xB40000A2u, // bwd_byte: cbz x2, done
            0x385FFCC3u, // ldrb w3, [x6, #-1]!
            0x381FFCA3u, // strb w3, [x5, #-1]!
            0xD1000442u, // sub x2, x2, #1
            0x17FFFFFCu, // b bwd_byte
            0xD65F03C0u, // done: ret
        });
        loader_.RegisterHostAddress("memcpy", code_memcpy);
        loader_.RegisterHostAddress("__memcpy_chk", code_memcpy);
        loader_.RegisterHostAddress("memmove", code_memmove);
        loader_.RegisterHostAddress("__memmove_chk", code_memmove);
        loader_.RegisterHostAddress("memset", code_memset);
        loader_.RegisterHostAddress("__memset_chk", code_memset);
        loader_.RegisterHostAddress("memcmp", code_memcmp);
        loader_.RegisterHostAddress("strlen", code_strlen);
        loader_.RegisterHostAddress("__strlen_chk", code_strlen);
        loader_.RegisterHostAddress("strcmp", code_strcmp);
        loader_.RegisterHostAddress("strncmp", code_strncmp);
    }
    fn("memchr", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::memchr(reinterpret_cast<const void*>(c.GetX(0)), static_cast<int>(c.GetW(1)), static_cast<size_t>(c.GetX(2))))); });
    fn("memrchr", [](CpuContext& c) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(c.GetX(0));
        uint8_t ch = static_cast<uint8_t>(c.GetW(1));
        size_t n = static_cast<size_t>(c.GetX(2));
        for (size_t i = n; i > 0; --i) {
            if (p[i - 1] == ch) { c.SetX(0, reinterpret_cast<uint64_t>(p + i - 1)); return; }
        }
        c.SetX(0, 0);
    });

    fn("strnlen", [](CpuContext& c) { c.SetX(0, ::strnlen(reinterpret_cast<const char*>(c.GetX(0)), static_cast<size_t>(c.GetX(1)))); });
    fn("strcasecmp", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(_stricmp(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1))))); });
    fn("strcpy", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strcpy(reinterpret_cast<char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1))))); });
    fn("__strcpy_chk", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strcpy(reinterpret_cast<char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1))))); });
    fn("strncpy", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strncpy(reinterpret_cast<char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1)), static_cast<size_t>(c.GetX(2))))); });
    fn("__strncpy_chk", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strncpy(reinterpret_cast<char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1)), static_cast<size_t>(c.GetX(2))))); });
    fn("__strncpy_chk2", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strncpy(reinterpret_cast<char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1)), static_cast<size_t>(c.GetX(2))))); });
    fn("strcat", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strcat(reinterpret_cast<char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1))))); });
    fn("__strcat_chk", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strcat(reinterpret_cast<char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1))))); });
    fn("strchr", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strchr(reinterpret_cast<const char*>(c.GetX(0)), static_cast<int>(c.GetW(1))))); });
    fn("__strchr_chk", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strchr(reinterpret_cast<const char*>(c.GetX(0)), static_cast<int>(c.GetW(1))))); });
    fn("strrchr", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strrchr(reinterpret_cast<const char*>(c.GetX(0)), static_cast<int>(c.GetW(1))))); });
    fn("__strrchr_chk", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strrchr(reinterpret_cast<const char*>(c.GetX(0)), static_cast<int>(c.GetW(1))))); });
    fn("strstr", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strstr(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1))))); });
    fn("strpbrk", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strpbrk(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1))))); });
    fn("strspn", [](CpuContext& c) { c.SetX(0, std::strspn(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1)))); });
    fn("strcspn", [](CpuContext& c) { c.SetX(0, std::strcspn(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1)))); });
    fn("strdup", [](CpuContext& c) {
        const char* s = reinterpret_cast<const char*>(c.GetX(0));
        size_t len = std::strlen(s) + 1;
        char* d = static_cast<char*>(std::malloc(len));
        if (d) std::memcpy(d, s, len);
        c.SetX(0, reinterpret_cast<uint64_t>(d));
    });
    fn("strtok", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strtok(reinterpret_cast<char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1))))); });
    fn("strtok_r", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(strtok_s(reinterpret_cast<char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1)), reinterpret_cast<char**>(c.GetX(2))))); });
    fn("strerror", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::strerror(static_cast<int>(c.GetW(0))))); });
    fn("strerror_r", [](CpuContext& c) {
        const char* s = std::strerror(static_cast<int>(c.GetW(0)));
        char* buf = reinterpret_cast<char*>(c.GetX(1));
        size_t sz = static_cast<size_t>(c.GetX(2));
        if (buf && sz > 0) { std::strncpy(buf, s ? s : "err", sz - 1); buf[sz - 1] = '\0'; }
        c.SetW(0, 0);
    });
    fn("basename", [](CpuContext& c) {
        char* s = reinterpret_cast<char*>(c.GetX(0));
        if (!s || !*s) { static char dot[] = "."; c.SetX(0, reinterpret_cast<uint64_t>(dot)); return; }
        char* slash = std::strrchr(s, '/');
        c.SetX(0, reinterpret_cast<uint64_t>(slash ? slash + 1 : s));
    });

    // ---- Number parsing ----
    fn("atoi", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::atoi(reinterpret_cast<const char*>(c.GetX(0))))); });
    fn("atol", [](CpuContext& c) { c.SetX(0, static_cast<uint64_t>(std::strtoll(reinterpret_cast<const char*>(c.GetX(0)), nullptr, 10))); });
    fn("atof", [](CpuContext& c) { c.SetD(0, std::atof(reinterpret_cast<const char*>(c.GetX(0)))); });
    fn("strtol", [](CpuContext& c) { c.SetX(0, static_cast<uint64_t>(std::strtoll(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<char**>(c.GetX(1)), static_cast<int>(c.GetW(2))))); });
    fn("strtoll", [](CpuContext& c) { c.SetX(0, static_cast<uint64_t>(std::strtoll(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<char**>(c.GetX(1)), static_cast<int>(c.GetW(2))))); });
    fn("strtoll_l", [](CpuContext& c) { c.SetX(0, static_cast<uint64_t>(std::strtoll(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<char**>(c.GetX(1)), static_cast<int>(c.GetW(2))))); });
    fn("strtoul", [](CpuContext& c) { c.SetX(0, std::strtoull(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<char**>(c.GetX(1)), static_cast<int>(c.GetW(2)))); });
    fn("strtoull", [](CpuContext& c) { c.SetX(0, std::strtoull(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<char**>(c.GetX(1)), static_cast<int>(c.GetW(2)))); });
    fn("strtoull_l", [](CpuContext& c) { c.SetX(0, std::strtoull(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<char**>(c.GetX(1)), static_cast<int>(c.GetW(2)))); });
    fn("strtod", [](CpuContext& c) { c.SetD(0, std::strtod(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<char**>(c.GetX(1)))); });
    fn("strtof", [](CpuContext& c) { c.SetS(0, std::strtof(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<char**>(c.GetX(1)))); });
    fn("strtold", [](CpuContext& c) {
        double d = std::strtod(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<char**>(c.GetX(1)));
        c.jit->SetVector(0, DoubleToBinary128(d));
    });
    fn("strtold_l", [](CpuContext& c) {
        double d = std::strtod(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<char**>(c.GetX(1)));
        c.jit->SetVector(0, DoubleToBinary128(d));
    });

    // ---- ctype / locale ----
    fn("tolower", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::tolower(static_cast<unsigned char>(c.GetW(0))))); });
    fn("toupper", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(c.GetW(0))))); });
    fn("tolower_l", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::tolower(static_cast<unsigned char>(c.GetW(0))))); });
    fn("toupper_l", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(c.GetW(0))))); });
    fn("isalnum", [](CpuContext& c) { c.SetW(0, std::isalnum(static_cast<unsigned char>(c.GetW(0))) ? 1 : 0); });
    fn("isupper", [](CpuContext& c) { c.SetW(0, std::isupper(static_cast<unsigned char>(c.GetW(0))) ? 1 : 0); });
    fn("isxdigit", [](CpuContext& c) { c.SetW(0, std::isxdigit(static_cast<unsigned char>(c.GetW(0))) ? 1 : 0); });
    fn("isdigit_l", [](CpuContext& c) { c.SetW(0, std::isdigit(static_cast<unsigned char>(c.GetW(0))) ? 1 : 0); });
    fn("islower_l", [](CpuContext& c) { c.SetW(0, std::islower(static_cast<unsigned char>(c.GetW(0))) ? 1 : 0); });
    fn("isupper_l", [](CpuContext& c) { c.SetW(0, std::isupper(static_cast<unsigned char>(c.GetW(0))) ? 1 : 0); });
    fn("isxdigit_l", [](CpuContext& c) { c.SetW(0, std::isxdigit(static_cast<unsigned char>(c.GetW(0))) ? 1 : 0); });
    fn("__ctype_get_mb_cur_max", [](CpuContext& c) { c.SetX(0, 1); });
    fn("newlocale", [](CpuContext& c) { c.SetX(0, 0x1001); });
    fn("freelocale", [](CpuContext&) {});
    fn("uselocale", [](CpuContext& c) { c.SetX(0, 0x1001); });
    fn("setlocale", [](CpuContext& c) { static char loc[] = "C"; c.SetX(0, reinterpret_cast<uint64_t>(loc)); });
    fn("localeconv", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::localeconv())); });
    fn("strcoll_l", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::strcmp(reinterpret_cast<const char*>(c.GetX(0)), reinterpret_cast<const char*>(c.GetX(1))))); });
    fn("strxfrm_l", [](CpuContext& c) {
        char* dst = reinterpret_cast<char*>(c.GetX(0));
        const char* src = reinterpret_cast<const char*>(c.GetX(1));
        size_t n = static_cast<size_t>(c.GetX(2));
        size_t len = std::strlen(src);
        if (dst && n > 0) { std::strncpy(dst, src, n - 1); dst[std::min(len, n - 1)] = '\0'; }
        c.SetX(0, len);
    });

    // ---- UTF-32 wchar_t (4-byte Android wchar_t!) ----
    fn("wcslen", [](CpuContext& c) {
        const uint32_t* s = reinterpret_cast<const uint32_t*>(c.GetX(0));
        size_t n = 0; while (s[n]) ++n;
        c.SetX(0, n);
    });
    fn("wmemcpy", [](CpuContext& c) {
        c.SetX(0, reinterpret_cast<uint64_t>(std::memcpy(reinterpret_cast<void*>(c.GetX(0)), reinterpret_cast<const void*>(c.GetX(1)), static_cast<size_t>(c.GetX(2)) * 4)));
    });
    fn("wmemmove", [](CpuContext& c) {
        c.SetX(0, reinterpret_cast<uint64_t>(std::memmove(reinterpret_cast<void*>(c.GetX(0)), reinterpret_cast<const void*>(c.GetX(1)), static_cast<size_t>(c.GetX(2)) * 4)));
    });
    fn("wmemset", [](CpuContext& c) {
        uint32_t* p = reinterpret_cast<uint32_t*>(c.GetX(0));
        uint32_t v = c.GetW(1);
        size_t n = static_cast<size_t>(c.GetX(2));
        for (size_t i = 0; i < n; ++i) p[i] = v;
        c.SetX(0, reinterpret_cast<uint64_t>(p));
    });
    fn("wmemchr", [](CpuContext& c) {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(c.GetX(0));
        uint32_t v = c.GetW(1);
        size_t n = static_cast<size_t>(c.GetX(2));
        for (size_t i = 0; i < n; ++i) { if (p[i] == v) { c.SetX(0, reinterpret_cast<uint64_t>(p + i)); return; } }
        c.SetX(0, 0);
    });
    fn("wmemcmp", [](CpuContext& c) {
        const uint32_t* a = reinterpret_cast<const uint32_t*>(c.GetX(0));
        const uint32_t* b = reinterpret_cast<const uint32_t*>(c.GetX(1));
        size_t n = static_cast<size_t>(c.GetX(2));
        for (size_t i = 0; i < n; ++i) {
            if (a[i] != b[i]) { c.SetW(0, a[i] < b[i] ? static_cast<uint32_t>(-1) : 1u); return; }
        }
        c.SetW(0, 0);
    });
    fn("btowc", [](CpuContext& c) { int ch = static_cast<int>(c.GetW(0)); c.SetW(0, (ch >= 0 && ch < 128) ? static_cast<uint32_t>(ch) : 0xFFFFFFFFu); });
    fn("wctob", [](CpuContext& c) { uint32_t wc = c.GetW(0); c.SetW(0, (wc < 128) ? wc : static_cast<uint32_t>(-1)); });
    fn("mbrlen", [](CpuContext& c) { const char* s = reinterpret_cast<const char*>(c.GetX(0)); size_t n = static_cast<size_t>(c.GetX(1)); c.SetX(0, (!s || !n || !*s) ? 0 : 1); });
    fn("mbtowc", [](CpuContext& c) {
        uint32_t* pwc = reinterpret_cast<uint32_t*>(c.GetX(0));
        const char* s = reinterpret_cast<const char*>(c.GetX(1));
        size_t n = static_cast<size_t>(c.GetX(2));
        if (!s) { c.SetW(0, 0); return; }
        if (!n) { c.SetW(0, static_cast<uint32_t>(-1)); return; }
        if (pwc) *pwc = static_cast<unsigned char>(*s);
        c.SetW(0, *s ? 1 : 0);
    });
    fn("mbrtowc", [](CpuContext& c) {
        uint32_t* pwc = reinterpret_cast<uint32_t*>(c.GetX(0));
        const char* s = reinterpret_cast<const char*>(c.GetX(1));
        size_t n = static_cast<size_t>(c.GetX(2));
        if (!s || !n) { c.SetX(0, 0); return; }
        if (pwc) *pwc = static_cast<unsigned char>(*s);
        c.SetX(0, *s ? 1 : 0);
    });
    fn("wcrtomb", [](CpuContext& c) {
        char* s = reinterpret_cast<char*>(c.GetX(0));
        uint32_t wc = c.GetW(1);
        if (s) *s = static_cast<char>(wc & 0xFF);
        c.SetX(0, 1);
    });
    fn("mbsrtowcs", [](CpuContext& c) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(c.GetX(0));
        const char** srcp = reinterpret_cast<const char**>(c.GetX(1));
        size_t len = static_cast<size_t>(c.GetX(2));
        const char* s = *srcp;
        size_t cnt = 0;
        while (*s && (!dst || cnt < len)) {
            if (dst) dst[cnt] = static_cast<unsigned char>(*s);
            ++s; ++cnt;
        }
        if (dst && cnt < len) { dst[cnt] = 0; *srcp = nullptr; } else if (dst) { *srcp = s; }
        c.SetX(0, cnt);
    });
    fn("mbsnrtowcs", [](CpuContext& c) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(c.GetX(0));
        const char** srcp = reinterpret_cast<const char**>(c.GetX(1));
        size_t nms = static_cast<size_t>(c.GetX(2));
        size_t len = static_cast<size_t>(c.GetX(3));
        const char* s = *srcp;
        size_t cnt = 0;
        while (cnt < nms && *s && (!dst || cnt < len)) {
            if (dst) dst[cnt] = static_cast<unsigned char>(*s);
            ++s; ++cnt;
        }
        if (dst) {
            if (cnt < len && (cnt == nms || !*s)) { if (!*s) { dst[cnt] = 0; *srcp = nullptr; } else *srcp = s; }
            else *srcp = s;
        }
        c.SetX(0, cnt);
    });
    fn("wcsnrtombs", [](CpuContext& c) {
        char* dst = reinterpret_cast<char*>(c.GetX(0));
        const uint32_t** srcp = reinterpret_cast<const uint32_t**>(c.GetX(1));
        size_t nwc = static_cast<size_t>(c.GetX(2));
        size_t len = static_cast<size_t>(c.GetX(3));
        const uint32_t* s = *srcp;
        size_t cnt = 0;
        while (cnt < nwc && *s && (!dst || cnt < len)) {
            if (dst) dst[cnt] = static_cast<char>(*s & 0xFF);
            ++s; ++cnt;
        }
        if (dst) {
            if (cnt < len && !*s) { dst[cnt] = '\0'; *srcp = nullptr; }
            else *srcp = s;
        }
        c.SetX(0, cnt);
    });
    auto wpred = [&](const std::string& nm, int(*p)(int)) {
        fn(nm, [p](CpuContext& c) { uint32_t wc = c.GetW(0); c.SetW(0, (wc < 128 && p(static_cast<int>(wc))) ? 1 : 0); });
    };
    wpred("iswalpha_l", std::isalpha); wpred("iswblank_l", [](int ch) -> int { return (ch == ' ' || ch == '\t') ? 1 : 0; });
    wpred("iswcntrl_l", std::iscntrl); wpred("iswdigit_l", std::isdigit);
    wpred("iswlower_l", std::islower); wpred("iswprint_l", std::isprint);
    wpred("iswpunct_l", std::ispunct); wpred("iswspace_l", std::isspace);
    wpred("iswupper_l", std::isupper); wpred("iswxdigit_l", std::isxdigit);
    fn("towlower_l", [](CpuContext& c) { uint32_t w = c.GetW(0); c.SetW(0, w < 128 ? static_cast<uint32_t>(std::tolower(static_cast<int>(w))) : w); });
    fn("towupper_l", [](CpuContext& c) { uint32_t w = c.GetW(0); c.SetW(0, w < 128 ? static_cast<uint32_t>(std::toupper(static_cast<int>(w))) : w); });
    fn("wcscoll_l", [](CpuContext& c) {
        const uint32_t* a = reinterpret_cast<const uint32_t*>(c.GetX(0));
        const uint32_t* b = reinterpret_cast<const uint32_t*>(c.GetX(1));
        while (*a && *a == *b) { ++a; ++b; }
        c.SetW(0, *a < *b ? static_cast<uint32_t>(-1) : (*a > *b ? 1u : 0u));
    });
    fn("wcsxfrm_l", [](CpuContext& c) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(c.GetX(0));
        const uint32_t* src = reinterpret_cast<const uint32_t*>(c.GetX(1));
        size_t n = static_cast<size_t>(c.GetX(2));
        size_t len = 0; while (src[len]) ++len;
        if (dst && n > 0) { size_t cp = std::min(len, n - 1); std::memcpy(dst, src, cp * 4); dst[cp] = 0; }
        c.SetX(0, len);
    });
    auto wtoascii = [](const uint32_t* ws) {
        std::string s;
        while (ws && *ws) { s.push_back(static_cast<char>(*ws & 0x7F)); ++ws; }
        return s;
    };
    fn("wcstol", [=](CpuContext& c) { std::string s = wtoascii(reinterpret_cast<const uint32_t*>(c.GetX(0))); c.SetX(0, static_cast<uint64_t>(std::strtoll(s.c_str(), nullptr, static_cast<int>(c.GetW(2))))); });
    fn("wcstoll", [=](CpuContext& c) { std::string s = wtoascii(reinterpret_cast<const uint32_t*>(c.GetX(0))); c.SetX(0, static_cast<uint64_t>(std::strtoll(s.c_str(), nullptr, static_cast<int>(c.GetW(2))))); });
    fn("wcstoul", [=](CpuContext& c) { std::string s = wtoascii(reinterpret_cast<const uint32_t*>(c.GetX(0))); c.SetX(0, std::strtoull(s.c_str(), nullptr, static_cast<int>(c.GetW(2)))); });
    fn("wcstoull", [=](CpuContext& c) { std::string s = wtoascii(reinterpret_cast<const uint32_t*>(c.GetX(0))); c.SetX(0, std::strtoull(s.c_str(), nullptr, static_cast<int>(c.GetW(2)))); });
    fn("wcstod", [=](CpuContext& c) { std::string s = wtoascii(reinterpret_cast<const uint32_t*>(c.GetX(0))); c.SetD(0, std::strtod(s.c_str(), nullptr)); });
    fn("wcstof", [=](CpuContext& c) { std::string s = wtoascii(reinterpret_cast<const uint32_t*>(c.GetX(0))); c.SetS(0, std::strtof(s.c_str(), nullptr)); });
    fn("wcstold", [=](CpuContext& c) { std::string s = wtoascii(reinterpret_cast<const uint32_t*>(c.GetX(0))); c.jit->SetVector(0, DoubleToBinary128(std::strtod(s.c_str(), nullptr))); });
    fn("swprintf", [=](CpuContext& c) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(c.GetX(0));
        size_t maxn = static_cast<size_t>(c.GetX(1));
        std::string fmt = wtoascii(reinterpret_cast<const uint32_t*>(c.GetX(2)));
        auto r = ArgReader::FromDirect(c, 3, 0);
        std::string res = FormatAapcs64(fmt.c_str(), r);
        if (dst && maxn > 0) {
            size_t cp = std::min(res.size(), maxn - 1);
            for (size_t i = 0; i < cp; ++i) dst[i] = static_cast<unsigned char>(res[i]);
            dst[cp] = 0;
        }
        c.SetW(0, static_cast<uint32_t>(res.size()));
    });

    // ---- Formatting & scanning ----
    fn("snprintf", [](CpuContext& c) {
        char* buf = reinterpret_cast<char*>(c.GetX(0));
        size_t sz = static_cast<size_t>(c.GetX(1));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(2));
        auto r = ArgReader::FromDirect(c, 3, 0);
        std::string s = FormatAapcs64(fmt, r);
        if (buf && sz > 0) {
            size_t cp = std::min(s.size(), sz - 1);
            std::memcpy(buf, s.data(), cp);
            buf[cp] = '\0';
        }
        c.SetW(0, static_cast<uint32_t>(s.size()));
    });
    fn("sprintf", [](CpuContext& c) {
        char* buf = reinterpret_cast<char*>(c.GetX(0));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(1));
        auto r = ArgReader::FromDirect(c, 2, 0);
        std::string s = FormatAapcs64(fmt, r);
        if (buf) std::memcpy(buf, s.c_str(), s.size() + 1);
        c.SetW(0, static_cast<uint32_t>(s.size()));
    });
    fn("vsnprintf", [](CpuContext& c) {
        char* buf = reinterpret_cast<char*>(c.GetX(0));
        size_t sz = static_cast<size_t>(c.GetX(1));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(2));
        Aapcs64VaList copy = *reinterpret_cast<Aapcs64VaList*>(c.GetX(3));
        auto r = ArgReader::FromVaList(&copy);
        std::string s = FormatAapcs64(fmt, r);
        if (buf && sz > 0) {
            size_t cp = std::min(s.size(), sz - 1);
            std::memcpy(buf, s.data(), cp);
            buf[cp] = '\0';
        }
        c.SetW(0, static_cast<uint32_t>(s.size()));
    });
    fn("__vsnprintf_chk", [](CpuContext& c) {
        // (char* dst, size_t maxlen, int flag, size_t slen, const char* fmt, va_list ap)
        char* buf = reinterpret_cast<char*>(c.GetX(0));
        size_t sz = static_cast<size_t>(c.GetX(1));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(4));
        Aapcs64VaList copy = *reinterpret_cast<Aapcs64VaList*>(c.GetX(5));
        auto r = ArgReader::FromVaList(&copy);
        std::string s = FormatAapcs64(fmt, r);
        if (buf && sz > 0) {
            size_t cp = std::min(s.size(), sz - 1);
            std::memcpy(buf, s.data(), cp);
            buf[cp] = '\0';
        }
        c.SetW(0, static_cast<uint32_t>(s.size()));
    });
    fn("__vsprintf_chk", [](CpuContext& c) {
        // (char* dst, int flag, size_t slen, const char* fmt, va_list ap)
        char* buf = reinterpret_cast<char*>(c.GetX(0));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(3));
        Aapcs64VaList copy = *reinterpret_cast<Aapcs64VaList*>(c.GetX(4));
        auto r = ArgReader::FromVaList(&copy);
        std::string s = FormatAapcs64(fmt, r);
        if (buf) std::memcpy(buf, s.c_str(), s.size() + 1);
        c.SetW(0, static_cast<uint32_t>(s.size()));
    });
    fn("vasprintf", [](CpuContext& c) {
        char** outp = reinterpret_cast<char**>(c.GetX(0));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(1));
        Aapcs64VaList copy = *reinterpret_cast<Aapcs64VaList*>(c.GetX(2));
        auto r = ArgReader::FromVaList(&copy);
        std::string s = FormatAapcs64(fmt, r);
        char* mem = static_cast<char*>(std::malloc(s.size() + 1));
        std::memcpy(mem, s.c_str(), s.size() + 1);
        *outp = mem;
        c.SetW(0, static_cast<uint32_t>(s.size()));
    });
    fn("printf", [](CpuContext& c) {
        const char* fmt = reinterpret_cast<const char*>(c.GetX(0));
        auto r = ArgReader::FromDirect(c, 1, 0);
        std::string s = FormatAapcs64(fmt, r);
        std::fwrite(s.data(), 1, s.size(), stdout);
        c.SetW(0, static_cast<uint32_t>(s.size()));
    });
    fn("vprintf", [](CpuContext& c) {
        const char* fmt = reinterpret_cast<const char*>(c.GetX(0));
        Aapcs64VaList copy = *reinterpret_cast<Aapcs64VaList*>(c.GetX(1));
        auto r = ArgReader::FromVaList(&copy);
        std::string s = FormatAapcs64(fmt, r);
        std::fwrite(s.data(), 1, s.size(), stdout);
        c.SetW(0, static_cast<uint32_t>(s.size()));
    });
    fn("fprintf", [](CpuContext& c) {
        FILE* fp = ResolveFilePtr(c.GetX(0));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(1));
        auto r = ArgReader::FromDirect(c, 2, 0);
        std::string s = FormatAapcs64(fmt, r);
        if (fp) std::fwrite(s.data(), 1, s.size(), fp);
        c.SetW(0, static_cast<uint32_t>(s.size()));
    });
    fn("vfprintf", [](CpuContext& c) {
        FILE* fp = ResolveFilePtr(c.GetX(0));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(1));
        Aapcs64VaList copy = *reinterpret_cast<Aapcs64VaList*>(c.GetX(2));
        auto r = ArgReader::FromVaList(&copy);
        std::string s = FormatAapcs64(fmt, r);
        if (fp) std::fwrite(s.data(), 1, s.size(), fp);
        c.SetW(0, static_cast<uint32_t>(s.size()));
    });
    fn("puts", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::puts(reinterpret_cast<const char*>(c.GetX(0))))); });
    fn("perror", [](CpuContext& c) { std::perror(reinterpret_cast<const char*>(c.GetX(0))); });
    auto translate_scanf_fmt = [](const char* fmt) -> std::pair<std::string, int> {
        // Rewrite AArch64 64-bit %ld/%lu/%lx/%lX/%li/%lo to MSVC %lld/... and count pointer args.
        std::string out;
        int nargs = 0;
        if (!fmt) return {out, 0};
        for (const char* p = fmt; *p; ) {
            if (*p != '%') { out.push_back(*p++); continue; }
            out.push_back(*p++);
            if (*p == '%') { out.push_back(*p++); continue; }
            bool suppress = false;
            if (*p == '*') { suppress = true; out.push_back(*p++); }
            while (*p >= '0' && *p <= '9') out.push_back(*p++);
            // Length modifiers
            if (*p == 'l' && p[1] != 'l' &&
                (p[1] == 'd' || p[1] == 'i' || p[1] == 'u' || p[1] == 'o' || p[1] == 'x' || p[1] == 'X')) {
                out.append("ll");
                ++p;
            } else if (*p == 'z' || *p == 't' || *p == 'j') {
                out.append("ll");
                ++p;
            } else if (*p == 'l' && p[1] == 'l') {
                out.append("ll");
                p += 2;
            } else if (*p == 'h' && p[1] == 'h') {
                out.append("hh");
                p += 2;
            } else if (*p == 'h' || *p == 'l' || *p == 'L') {
                out.push_back(*p++);
            }
            if (*p == '[') {
                out.push_back(*p++);
                if (*p == '^') out.push_back(*p++);
                if (*p == ']') out.push_back(*p++);
                while (*p && *p != ']') out.push_back(*p++);
                if (*p == ']') out.push_back(*p++);
            } else if (*p) {
                out.push_back(*p++);
            }
            if (!suppress) ++nargs;
        }
        return {out, nargs};
    };
    fn("sscanf", [translate_scanf_fmt](CpuContext& c) {
        const char* str = reinterpret_cast<const char*>(c.GetX(0));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(1));
        if (!str || !fmt) { c.SetW(0, static_cast<uint32_t>(-1)); return; }
        auto [wfmt, nargs] = translate_scanf_fmt(fmt);
        auto r = ArgReader::FromDirect(c, 2, 0);
        void* a[16]{};
        for (int i = 0; i < std::min(nargs, 16); ++i) a[i] = reinterpret_cast<void*>(r.NextU64());
        int rc = std::sscanf(str, wfmt.c_str(),
                             a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                             a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
        c.SetW(0, static_cast<uint32_t>(rc));
    });
    fn("vsscanf", [translate_scanf_fmt](CpuContext& c) {
        const char* str = reinterpret_cast<const char*>(c.GetX(0));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(1));
        if (!str || !fmt) { c.SetW(0, static_cast<uint32_t>(-1)); return; }
        Aapcs64VaList copy = *reinterpret_cast<Aapcs64VaList*>(c.GetX(2));
        auto [wfmt, nargs] = translate_scanf_fmt(fmt);
        auto r = ArgReader::FromVaList(&copy);
        void* a[16]{};
        for (int i = 0; i < std::min(nargs, 16); ++i) a[i] = reinterpret_cast<void*>(r.NextU64());
        int rc = std::sscanf(str, wfmt.c_str(),
                             a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                             a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
        c.SetW(0, static_cast<uint32_t>(rc));
    });
    fn("fscanf", [translate_scanf_fmt](CpuContext& c) {
        FILE* fp = ResolveFilePtr(c.GetX(0));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(1));
        if (!fp || !fmt) { c.SetW(0, static_cast<uint32_t>(-1)); return; }
        auto [wfmt, nargs] = translate_scanf_fmt(fmt);
        auto r = ArgReader::FromDirect(c, 2, 0);
        void* a[16]{};
        for (int i = 0; i < std::min(nargs, 16); ++i) a[i] = reinterpret_cast<void*>(r.NextU64());
        int rc = std::fscanf(fp, wfmt.c_str(),
                             a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                             a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
        c.SetW(0, static_cast<uint32_t>(rc));
    });

    // ---- Math (libm) ----
    fn("sin", [](CpuContext& c) { c.SetD(0, std::sin(c.GetD(0))); });
    fn("cos", [](CpuContext& c) { c.SetD(0, std::cos(c.GetD(0))); });
    fn("acos", [](CpuContext& c) { c.SetD(0, std::acos(c.GetD(0))); });
    fn("atan2", [](CpuContext& c) { c.SetD(0, std::atan2(c.GetD(0), c.GetD(1))); });
    fn("pow", [](CpuContext& c) { c.SetD(0, std::pow(c.GetD(0), c.GetD(1))); });
    fn("log", [](CpuContext& c) { c.SetD(0, std::log(c.GetD(0))); });
    fn("exp2", [](CpuContext& c) { c.SetD(0, std::exp2(c.GetD(0))); });
    fn("fmod", [](CpuContext& c) { c.SetD(0, std::fmod(c.GetD(0), c.GetD(1))); });
    fn("ldexp", [](CpuContext& c) { c.SetD(0, std::ldexp(c.GetD(0), static_cast<int>(c.GetW(0)))); });
    fn("frexp", [](CpuContext& c) { c.SetD(0, std::frexp(c.GetD(0), reinterpret_cast<int*>(c.GetX(0)))); });
    fn("modf", [](CpuContext& c) { c.SetD(0, std::modf(c.GetD(0), reinterpret_cast<double*>(c.GetX(0)))); });

    fn("sinf", [](CpuContext& c) { c.SetS(0, std::sinf(c.GetS(0))); });
    fn("cosf", [](CpuContext& c) { c.SetS(0, std::cosf(c.GetS(0))); });
    fn("tanf", [](CpuContext& c) { c.SetS(0, std::tanf(c.GetS(0))); });
    fn("asinf", [](CpuContext& c) { c.SetS(0, std::asinf(c.GetS(0))); });
    fn("acosf", [](CpuContext& c) { c.SetS(0, std::acosf(c.GetS(0))); });
    fn("atanf", [](CpuContext& c) { c.SetS(0, std::atanf(c.GetS(0))); });
    fn("atan2f", [](CpuContext& c) { c.SetS(0, std::atan2f(c.GetS(0), c.GetS(1))); });
    fn("powf", [](CpuContext& c) { c.SetS(0, std::powf(c.GetS(0), c.GetS(1))); });
    fn("logf", [](CpuContext& c) { c.SetS(0, std::logf(c.GetS(0))); });
    fn("log10f", [](CpuContext& c) { c.SetS(0, std::log10f(c.GetS(0))); });
    fn("expf", [](CpuContext& c) { c.SetS(0, std::expf(c.GetS(0))); });
    fn("exp2f", [](CpuContext& c) { c.SetS(0, std::exp2f(c.GetS(0))); });
    fn("fmodf", [](CpuContext& c) { c.SetS(0, std::fmodf(c.GetS(0), c.GetS(1))); });
    fn("modff", [](CpuContext& c) { c.SetS(0, std::modff(c.GetS(0), reinterpret_cast<float*>(c.GetX(0)))); });
    fn("nextafterf", [](CpuContext& c) { c.SetS(0, std::nextafterf(c.GetS(0), c.GetS(1))); });
    fn("sincosf", [](CpuContext& c) {
        float x = c.GetS(0);
        auto* sp = reinterpret_cast<float*>(c.GetX(0));
        auto* cp = reinterpret_cast<float*>(c.GetX(1));
        if (sp) *sp = std::sinf(x);
        if (cp) *cp = std::cosf(x);
    });
    fn("rand", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::rand())); });
    fn("srand", [](CpuContext& c) { std::srand(c.GetW(0)); });

    // ---- File I/O (stdio + POSIX fd + layered VFS) ----
    fn("fopen", [this](CpuContext& c) {
        const char* p = reinterpret_cast<const char*>(c.GetX(0));
        const char* m = reinterpret_cast<const char*>(c.GetX(1));
        if (!p || !m) { c.SetX(0, 0); return; }
        bool write_mode = (std::strpbrk(m, "wa+") != nullptr);
        std::filesystem::path host_p;
        if (write_mode) {
            host_p = vfs_.ResolveWrite(p);
            if (host_p.empty()) { tls_bionic_errno = 13; c.SetX(0, 0); return; }
            if ((std::strchr(m, '+') || std::strchr(m, 'a')) && !std::filesystem::exists(host_p)) {
                auto ro = vfs_.ResolveRead(p);
                if (!ro.empty() && ro != host_p && std::filesystem::exists(ro)) {
                    std::error_code ec;
                    std::filesystem::copy_file(ro, host_p, ec);
                }
            }
        } else {
            host_p = vfs_.ResolveRead(p);
        }
        std::string bin_mode;
        for (const char* q = m; *q; ++q) {
            if (*q == 'r' || *q == 'w' || *q == 'a' || *q == '+' || *q == 'b') bin_mode.push_back(*q);
        }
        if (bin_mode.empty()) bin_mode = "rb";
        if (bin_mode.find('b') == std::string::npos) bin_mode.push_back('b');
        FILE* fp = _wfopen(host_p.c_str(), std::wstring(bin_mode.begin(), bin_mode.end()).c_str());
        if (!fp) SyncErrno();
        c.SetX(0, reinterpret_cast<uint64_t>(fp));
    });
    fn("fdopen", [](CpuContext& c) {
        int fd = static_cast<int>(c.GetW(0));
        const char* m = reinterpret_cast<const char*>(c.GetX(1));
        std::string bin_mode;
        if (m) {
            for (const char* q = m; *q; ++q) {
                if (*q == 'r' || *q == 'w' || *q == 'a' || *q == '+' || *q == 'b') bin_mode.push_back(*q);
            }
        }
        if (bin_mode.empty()) bin_mode = "rb";
        if (bin_mode.find('b') == std::string::npos) bin_mode.push_back('b');
        FILE* fp = _fdopen(fd, bin_mode.c_str());
        if (!fp) SyncErrno();
        c.SetX(0, reinterpret_cast<uint64_t>(fp));
    });
    fn("fclose", [](CpuContext& c) {
        FILE* fp = ResolveFilePtr(c.GetX(0));
        if (!fp || fp == stdin || fp == stdout || fp == stderr) { c.SetW(0, 0); return; }
        c.SetW(0, static_cast<uint32_t>(std::fclose(fp)));
    });
    fn("fread", [](CpuContext& c) {
        c.SetX(0, std::fread(reinterpret_cast<void*>(c.GetX(0)), static_cast<size_t>(c.GetX(1)), static_cast<size_t>(c.GetX(2)), ResolveFilePtr(c.GetX(3))));
    });
    fn("fwrite", [](CpuContext& c) {
        c.SetX(0, std::fwrite(reinterpret_cast<const void*>(c.GetX(0)), static_cast<size_t>(c.GetX(1)), static_cast<size_t>(c.GetX(2)), ResolveFilePtr(c.GetX(3))));
    });
    fn("__fwrite_chk", [](CpuContext& c) {
        c.SetX(0, std::fwrite(reinterpret_cast<const void*>(c.GetX(0)), static_cast<size_t>(c.GetX(1)), static_cast<size_t>(c.GetX(2)), ResolveFilePtr(c.GetX(3))));
    });
    fn("fseek", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(_fseeki64(ResolveFilePtr(c.GetX(0)), static_cast<int64_t>(c.GetX(1)), static_cast<int>(c.GetW(2))))); });
    fn("fseeko", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(_fseeki64(ResolveFilePtr(c.GetX(0)), static_cast<int64_t>(c.GetX(1)), static_cast<int>(c.GetW(2))))); });
    fn("ftell", [](CpuContext& c) { c.SetX(0, static_cast<uint64_t>(_ftelli64(ResolveFilePtr(c.GetX(0))))); });
    fn("ftello", [](CpuContext& c) { c.SetX(0, static_cast<uint64_t>(_ftelli64(ResolveFilePtr(c.GetX(0))))); });
    fn("rewind", [](CpuContext& c) { std::rewind(ResolveFilePtr(c.GetX(0))); });
    fn("fflush", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::fflush(ResolveFilePtr(c.GetX(0))))); });
    fn("feof", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::feof(ResolveFilePtr(c.GetX(0))))); });
    fn("ferror", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::ferror(ResolveFilePtr(c.GetX(0))))); });
    fn("clearerr", [](CpuContext& c) { std::clearerr(ResolveFilePtr(c.GetX(0))); });
    fn("fileno", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(_fileno(ResolveFilePtr(c.GetX(0))))); });
    fn("fgets", [](CpuContext& c) { c.SetX(0, reinterpret_cast<uint64_t>(std::fgets(reinterpret_cast<char*>(c.GetX(0)), static_cast<int>(c.GetW(1)), ResolveFilePtr(c.GetX(2))))); });
    fn("fputs", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::fputs(reinterpret_cast<const char*>(c.GetX(0)), ResolveFilePtr(c.GetX(1))))); });
    fn("fputc", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::fputc(static_cast<int>(c.GetW(0)), ResolveFilePtr(c.GetX(1))))); });
    fn("getc", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::fgetc(ResolveFilePtr(c.GetX(0))))); });
    fn("ungetc", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::ungetc(static_cast<int>(c.GetW(0)), ResolveFilePtr(c.GetX(1))))); });
    fn("setvbuf", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(std::setvbuf(ResolveFilePtr(c.GetX(0)), reinterpret_cast<char*>(c.GetX(1)), static_cast<int>(c.GetW(2)), static_cast<size_t>(c.GetX(3))))); });

    auto do_open = [this](CpuContext& c) {
        const char* p = reinterpret_cast<const char*>(c.GetX(0));
        int lflags = static_cast<int>(c.GetW(1));
        if (!p) { tls_bionic_errno = 14; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        int acc = lflags & 3;
        bool write_mode = (acc == 1 || acc == 2 || (lflags & 0100) || (lflags & 01000) || (lflags & 02000));
        std::filesystem::path host_p = write_mode ? vfs_.ResolveWrite(p) : vfs_.ResolveRead(p);
        if (host_p.empty()) { tls_bionic_errno = 13; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        if ((acc == 2 || (lflags & 02000)) && !std::filesystem::exists(host_p)) {
            auto ro = vfs_.ResolveRead(p);
            if (!ro.empty() && ro != host_p && std::filesystem::exists(ro)) {
                std::error_code ec; std::filesystem::copy_file(ro, host_p, ec);
            }
        }
        int wflags = _O_BINARY;
        if (acc == 0) wflags |= _O_RDONLY;
        else if (acc == 1) wflags |= _O_WRONLY;
        else wflags |= _O_RDWR;
        if (lflags & 0100) wflags |= _O_CREAT;
        if (lflags & 0200) wflags |= _O_EXCL;
        if (lflags & 01000) wflags |= _O_TRUNC;
        if (lflags & 02000) wflags |= _O_APPEND;
        int fd = _wopen(host_p.c_str(), wflags, _S_IREAD | _S_IWRITE);
        if (fd < 0) SyncErrno();
        c.SetW(0, static_cast<uint32_t>(fd));
    };
    fn("open", do_open);
    fn("__open_2", do_open);
    fn("close", [](CpuContext& c) {
        int fd = static_cast<int>(c.GetW(0));
        if (CloseVirtSocket(fd)) { c.SetW(0, 0); return; }
        int r = _close(fd);
        if (r < 0) SyncErrno();
        c.SetW(0, static_cast<uint32_t>(r));
    });
    fn("read", [](CpuContext& c) {
        int fd = static_cast<int>(c.GetW(0));
        SOCKET s = INVALID_SOCKET;
        if (LookupVirtSocket(fd, s)) {
            int r = ::recv(s, reinterpret_cast<char*>(c.GetX(1)), static_cast<int>(c.GetW(2)), 0);
            if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
            c.SetX(0, static_cast<uint64_t>(static_cast<int64_t>(r)));
            return;
        }
        int r = _read(fd, reinterpret_cast<void*>(c.GetX(1)), static_cast<unsigned int>(c.GetW(2)));
        if (r < 0) SyncErrno();
        c.SetX(0, static_cast<uint64_t>(static_cast<int64_t>(r)));
    });
    fn("__read_chk", [](CpuContext& c) {
        int fd = static_cast<int>(c.GetW(0));
        SOCKET s = INVALID_SOCKET;
        if (LookupVirtSocket(fd, s)) {
            int r = ::recv(s, reinterpret_cast<char*>(c.GetX(1)), static_cast<int>(c.GetW(2)), 0);
            if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
            c.SetX(0, static_cast<uint64_t>(static_cast<int64_t>(r)));
            return;
        }
        int r = _read(fd, reinterpret_cast<void*>(c.GetX(1)), static_cast<unsigned int>(c.GetW(2)));
        if (r < 0) SyncErrno();
        c.SetX(0, static_cast<uint64_t>(static_cast<int64_t>(r)));
    });
    fn("write", [](CpuContext& c) {
        int fd = static_cast<int>(c.GetW(0));
        SOCKET s = INVALID_SOCKET;
        if (LookupVirtSocket(fd, s)) {
            int r = ::send(s, reinterpret_cast<const char*>(c.GetX(1)), static_cast<int>(c.GetW(2)), 0);
            if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
            c.SetX(0, static_cast<uint64_t>(static_cast<int64_t>(r)));
            return;
        }
        int r = _write(fd, reinterpret_cast<const void*>(c.GetX(1)), static_cast<unsigned int>(c.GetW(2)));
        if (r < 0) SyncErrno();
        c.SetX(0, static_cast<uint64_t>(static_cast<int64_t>(r)));
    });
    fn("lseek64", [](CpuContext& c) { int64_t r = _lseeki64(static_cast<int>(c.GetW(0)), static_cast<int64_t>(c.GetX(1)), static_cast<int>(c.GetW(2))); if (r < 0) SyncErrno(); c.SetX(0, static_cast<uint64_t>(r)); });
    fn("fsync", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(_commit(static_cast<int>(c.GetW(0))))); });
    fn("ftruncate", [](CpuContext& c) { c.SetW(0, static_cast<uint32_t>(_chsize_s(static_cast<int>(c.GetW(0)), static_cast<int64_t>(c.GetX(1))) == 0 ? 0 : -1)); });
    fn("truncate", [this](CpuContext& c) {
        const char* p = reinterpret_cast<const char*>(c.GetX(0));
        auto hp = p ? vfs_.ResolveWrite(p) : std::filesystem::path();
        if (hp.empty()) { tls_bionic_errno = 13; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        std::error_code ec;
        std::filesystem::resize_file(hp, static_cast<uintmax_t>(c.GetX(1)), ec);
        c.SetW(0, ec ? static_cast<uint32_t>(-1) : 0u);
    });
    fn("fcntl", [](CpuContext& c) {
        int fd = static_cast<int>(c.GetW(0));
        int cmd = static_cast<int>(c.GetW(1));
        uint64_t arg = c.GetX(2);
        SOCKET s = INVALID_SOCKET;
        if (LookupVirtSocket(fd, s)) {
            if (cmd == 3) { // F_GETFL
                std::lock_guard<std::mutex> lk(g_sock_mtx);
                c.SetW(0, g_virt_nonblock[fd] ? 0x802u : 0x2u);
                return;
            }
            if (cmd == 4) { // F_SETFL
                bool nb = (arg & 0x800ULL) != 0;
                u_long mode = nb ? 1u : 0u;
                ioctlsocket(s, FIONBIO, &mode);
                std::lock_guard<std::mutex> lk(g_sock_mtx);
                g_virt_nonblock[fd] = nb;
                c.SetW(0, 0);
                return;
            }
        }
        c.SetW(0, 0);
    });
    fn("ioctl", [](CpuContext& c) {
        int fd = static_cast<int>(c.GetW(0));
        uint32_t req = c.GetW(1);
        SOCKET s = INVALID_SOCKET;
        if (LookupVirtSocket(fd, s)) {
            if (req == 0x5421u) { // Linux FIONBIO
                int* pval = reinterpret_cast<int*>(c.GetX(2));
                u_long mode = (pval && *pval) ? 1u : 0u;
                int r = ioctlsocket(s, FIONBIO, &mode);
                if (r == 0) {
                    std::lock_guard<std::mutex> lk(g_sock_mtx);
                    g_virt_nonblock[fd] = (mode != 0);
                }
                c.SetW(0, static_cast<uint32_t>(r));
                return;
            }
            if (req == 0x541Bu) { // Linux FIONREAD
                u_long avail = 0;
                int r = ioctlsocket(s, FIONREAD, &avail);
                int* pval = reinterpret_cast<int*>(c.GetX(2));
                if (r == 0 && pval) *pval = static_cast<int>(avail);
                c.SetW(0, static_cast<uint32_t>(r));
                return;
            }
        }
        c.SetW(0, 0);
    });
    fn("pipe", [](CpuContext& c) {
        int* fds = reinterpret_cast<int*>(c.GetX(0));
        int r = _pipe(fds, 4096, _O_BINARY);
        if (r < 0) SyncErrno();
        c.SetW(0, static_cast<uint32_t>(r));
    });
    fn("sendfile", [](CpuContext& c) { c.SetX(0, static_cast<uint64_t>(-1)); tls_bionic_errno = 38; });

    // ---- stat / filesystem / dir ----
    auto do_stat = [this](CpuContext& c) {
        const char* p = reinterpret_cast<const char*>(c.GetX(0));
        auto* out = reinterpret_cast<BionicStat64*>(c.GetX(1));
        if (!p || !out) { tls_bionic_errno = 14; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        auto hp = vfs_.ResolveRead(p);
        struct _stat64 ws{};
        if (_wstat64(hp.c_str(), &ws) != 0) {
            SyncErrno();
            c.SetW(0, static_cast<uint32_t>(-1));
            return;
        }
        FillBionicStat(ws, *out);
        c.SetW(0, 0);
    };
    fn("stat", do_stat);
    fn("lstat", do_stat);
    fn("fstat", [](CpuContext& c) {
        struct _stat64 ws{};
        if (_fstat64(static_cast<int>(c.GetW(0)), &ws) != 0) {
            SyncErrno();
            c.SetW(0, static_cast<uint32_t>(-1));
            return;
        }
        FillBionicStat(ws, *reinterpret_cast<BionicStat64*>(c.GetX(1)));
        c.SetW(0, 0);
    });
    fn("statfs", [](CpuContext& c) {
        uint64_t* buf = reinterpret_cast<uint64_t*>(c.GetX(1));
        if (buf) {
            std::memset(buf, 0, 120);
            buf[0] = 0xEF53; // EXT4_SUPER_MAGIC
            buf[1] = 4096;   // f_bsize
            buf[2] = 10000000; // f_blocks
            buf[3] = 5000000;  // f_bfree
            buf[4] = 5000000;  // f_bavail
            buf[5] = 1000000;  // f_files
            buf[6] = 500000;   // f_ffree
            buf[8] = 255;      // f_namelen
            buf[9] = 4096;     // f_frsize
        }
        c.SetW(0, 0);
    });
    fn("statvfs", [](CpuContext& c) {
        uint64_t* buf = reinterpret_cast<uint64_t*>(c.GetX(1));
        if (buf) {
            std::memset(buf, 0, 112);
            buf[0] = 4096; buf[1] = 4096;
            buf[2] = 10000000; buf[3] = 5000000; buf[4] = 5000000;
            buf[5] = 1000000; buf[6] = 500000; buf[7] = 500000;
            buf[10] = 255;
        }
        c.SetW(0, 0);
    });
    fn("access", [this](CpuContext& c) {
        const char* p = reinterpret_cast<const char*>(c.GetX(0));
        auto hp = vfs_.ResolveRead(p ? p : "");
        bool ex = std::filesystem::exists(hp);
        if (!ex) tls_bionic_errno = 2;
        c.SetW(0, ex ? 0u : static_cast<uint32_t>(-1));
    });
    fn("mkdir", [this](CpuContext& c) {
        const char* p = reinterpret_cast<const char*>(c.GetX(0));
        if (p && (std::strcmp(p, "/storage") == 0 || std::strcmp(p, "/storage/") == 0 ||
                  std::strcmp(p, "/storage/rr3") == 0 || std::strcmp(p, "/storage/rr3/") == 0)) {
            c.SetW(0, 0u);
            return;
        }
        auto hp = p ? vfs_.ResolveWrite(p) : std::filesystem::path();
        if (hp.empty()) { tls_bionic_errno = 13; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        std::error_code ec;
        std::filesystem::create_directories(hp, ec);
        c.SetW(0, ec ? static_cast<uint32_t>(-1) : 0u);
    });
    fn("rmdir", [this](CpuContext& c) {
        const char* p = reinterpret_cast<const char*>(c.GetX(0));
        auto hp = p ? vfs_.ResolveWrite(p) : std::filesystem::path();
        if (hp.empty()) { tls_bionic_errno = 13; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        std::error_code ec;
        bool ok = std::filesystem::remove(hp, ec);
        c.SetW(0, (ok && !ec) ? 0u : static_cast<uint32_t>(-1));
    });
    fn("unlink", [this](CpuContext& c) {
        const char* p = reinterpret_cast<const char*>(c.GetX(0));
        auto hp = p ? vfs_.ResolveWrite(p) : std::filesystem::path();
        if (hp.empty()) { tls_bionic_errno = 13; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        std::error_code ec;
        bool ok = std::filesystem::remove(hp, ec);
        if (!ok) tls_bionic_errno = 2;
        c.SetW(0, ok ? 0u : static_cast<uint32_t>(-1));
    });
    fn("remove", [this](CpuContext& c) {
        const char* p = reinterpret_cast<const char*>(c.GetX(0));
        auto hp = p ? vfs_.ResolveWrite(p) : std::filesystem::path();
        if (hp.empty()) { tls_bionic_errno = 13; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        std::error_code ec;
        bool ok = std::filesystem::remove(hp, ec);
        if (!ok) tls_bionic_errno = 2;
        c.SetW(0, ok ? 0u : static_cast<uint32_t>(-1));
    });
    fn("rename", [this](CpuContext& c) {
        const char* p0 = reinterpret_cast<const char*>(c.GetX(0));
        const char* p1 = reinterpret_cast<const char*>(c.GetX(1));
        auto src = p0 ? vfs_.ResolveWrite(p0) : std::filesystem::path();
        auto dst = p1 ? vfs_.ResolveWrite(p1) : std::filesystem::path();
        if (src.empty() || dst.empty()) { tls_bionic_errno = 13; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        std::error_code ec;
        std::filesystem::rename(src, dst, ec);
        if (ec) tls_bionic_errno = 2;
        c.SetW(0, ec ? static_cast<uint32_t>(-1) : 0u);
    });
    fn("opendir", [this](CpuContext& c) {
        const char* p = reinterpret_cast<const char*>(c.GetX(0));
        auto dirs = vfs_.ResolveReadDirs(p ? p : "");
        if (dirs.empty()) {
            tls_bionic_errno = 2;
            c.SetX(0, 0);
            return;
        }
        auto* hd = new HostDir();
        std::unordered_map<std::string, uint8_t> seen;
        for (const auto& dir : dirs) {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                std::string nm = entry.path().filename().string();
                if (seen.find(nm) == seen.end()) {
                    seen[nm] = entry.is_directory() ? 4 : 8;
                }
            }
        }
        hd->entries.emplace_back(".", 4);
        hd->entries.emplace_back("..", 4);
        for (auto& kv : seen) hd->entries.emplace_back(kv.first, kv.second);
        c.SetX(0, reinterpret_cast<uint64_t>(hd));
    });
    fn("readdir", [](CpuContext& c) {
        auto* hd = reinterpret_cast<HostDir*>(c.GetX(0));
        if (!hd || hd->idx >= hd->entries.size()) { c.SetX(0, 0); return; }
        auto& e = hd->entries[hd->idx++];
        std::memset(&hd->cur, 0, sizeof(hd->cur));
        hd->cur.d_ino = hd->idx;
        hd->cur.d_off = static_cast<int64_t>(hd->idx);
        hd->cur.d_reclen = sizeof(BionicDirent);
        hd->cur.d_type = e.second;
        std::strncpy(hd->cur.d_name, e.first.c_str(), sizeof(hd->cur.d_name) - 1);
        c.SetX(0, reinterpret_cast<uint64_t>(&hd->cur));
    });
    fn("closedir", [](CpuContext& c) {
        delete reinterpret_cast<HostDir*>(c.GetX(0));
        c.SetW(0, 0);
    });
    fn("getcwd", [](CpuContext& c) {
        char* buf = reinterpret_cast<char*>(c.GetX(0));
        size_t sz = static_cast<size_t>(c.GetX(1));
        const char* cwd = LayeredVfs::kAndroidFilesRoot;
        if (!buf) { buf = static_cast<char*>(std::malloc(std::strlen(cwd) + 1)); sz = std::strlen(cwd) + 1; }
        std::strncpy(buf, cwd, sz - 1); buf[sz - 1] = '\0';
        c.SetX(0, reinterpret_cast<uint64_t>(buf));
    });
    fn("chdir", [](CpuContext& c) { c.SetW(0, 0); });
    fn("realpath", [this](CpuContext& c) {
        const char* p = reinterpret_cast<const char*>(c.GetX(0));
        char* resolved = reinterpret_cast<char*>(c.GetX(1));
        std::string s = p ? p : "";
        if (!resolved) resolved = static_cast<char*>(std::malloc(s.size() + 1));
        std::memcpy(resolved, s.c_str(), s.size() + 1);
        c.SetX(0, reinterpret_cast<uint64_t>(resolved));
    });
    fn("readlink", [](CpuContext& c) { tls_bionic_errno = 22; c.SetX(0, static_cast<uint64_t>(-1)); });
    fn("symlink", [](CpuContext& c) { tls_bionic_errno = 38; c.SetW(0, static_cast<uint32_t>(-1)); });
    fn("link", [](CpuContext& c) { tls_bionic_errno = 38; c.SetW(0, static_cast<uint32_t>(-1)); });
    fn("fchmod", [](CpuContext& c) { c.SetW(0, 0); });
    fn("fchmodat", [](CpuContext& c) { c.SetW(0, 0); });
    fn("fchown", [](CpuContext& c) { c.SetW(0, 0); });
    fn("utimes", [](CpuContext& c) { c.SetW(0, 0); });
    fn("utimensat", [](CpuContext& c) { c.SetW(0, 0); });
    fn("pathconf", [](CpuContext& c) { c.SetX(0, 255); });
    fn("getpwuid_r", [](CpuContext& c) { c.SetW(0, 2); });

    // ---- Time / sleep ----
    fn("time", [](CpuContext& c) {
        int64_t t = static_cast<int64_t>(std::time(nullptr));
        auto* out = reinterpret_cast<int64_t*>(c.GetX(0));
        if (out) *out = t;
        c.SetX(0, static_cast<uint64_t>(t));
    });
    fn("difftime", [](CpuContext& c) { c.SetD(0, static_cast<double>(static_cast<int64_t>(c.GetX(0)) - static_cast<int64_t>(c.GetX(1)))); });
    fn("gettimeofday", [](CpuContext& c) {
        auto* tv = reinterpret_cast<int64_t*>(c.GetX(0));
        if (tv) {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
            tv[0] = us / 1000000;
            tv[1] = us % 1000000;
        }
        c.SetW(0, 0);
    });
    fn("clock_gettime", [](CpuContext& c) {
        int clk = static_cast<int>(c.GetW(0));
        auto* ts = reinterpret_cast<int64_t*>(c.GetX(1));
        if (ts) {
            if (clk == 0) { // CLOCK_REALTIME
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                ts[0] = ns / 1000000000LL;
                ts[1] = ns % 1000000000LL;
            } else { // CLOCK_MONOTONIC / BOOTTIME / etc.
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                ts[0] = ns / 1000000000LL;
                ts[1] = ns % 1000000000LL;
            }
        }
        c.SetW(0, 0);
    });
    fn("localtime", [](CpuContext& c) {
        std::time_t t = static_cast<std::time_t>(*reinterpret_cast<const int64_t*>(c.GetX(0)));
        std::tm tm_v{}; localtime_s(&tm_v, &t);
        FillLinuxTm(tm_v, tls_tm);
        c.SetX(0, reinterpret_cast<uint64_t>(&tls_tm));
    });
    fn("localtime_r", [](CpuContext& c) {
        std::time_t t = static_cast<std::time_t>(*reinterpret_cast<const int64_t*>(c.GetX(0)));
        std::tm tm_v{}; localtime_s(&tm_v, &t);
        auto* dst = reinterpret_cast<LinuxTm*>(c.GetX(1));
        FillLinuxTm(tm_v, *dst);
        c.SetX(0, reinterpret_cast<uint64_t>(dst));
    });
    fn("gmtime", [](CpuContext& c) {
        std::time_t t = static_cast<std::time_t>(*reinterpret_cast<const int64_t*>(c.GetX(0)));
        std::tm tm_v{}; gmtime_s(&tm_v, &t);
        FillLinuxTm(tm_v, tls_tm);
        c.SetX(0, reinterpret_cast<uint64_t>(&tls_tm));
    });
    fn("gmtime_r", [](CpuContext& c) {
        std::time_t t = static_cast<std::time_t>(*reinterpret_cast<const int64_t*>(c.GetX(0)));
        std::tm tm_v{}; gmtime_s(&tm_v, &t);
        auto* dst = reinterpret_cast<LinuxTm*>(c.GetX(1));
        FillLinuxTm(tm_v, *dst);
        c.SetX(0, reinterpret_cast<uint64_t>(dst));
    });
    fn("mktime", [](CpuContext& c) {
        auto* ltm = reinterpret_cast<LinuxTm*>(c.GetX(0));
        std::tm tm_v{};
        tm_v.tm_sec = ltm->tm_sec; tm_v.tm_min = ltm->tm_min; tm_v.tm_hour = ltm->tm_hour;
        tm_v.tm_mday = ltm->tm_mday; tm_v.tm_mon = ltm->tm_mon; tm_v.tm_year = ltm->tm_year;
        tm_v.tm_isdst = ltm->tm_isdst;
        c.SetX(0, static_cast<uint64_t>(std::mktime(&tm_v)));
    });
    auto do_strftime = [](CpuContext& c) {
        char* buf = reinterpret_cast<char*>(c.GetX(0));
        size_t maxsz = static_cast<size_t>(c.GetX(1));
        const char* fmt = reinterpret_cast<const char*>(c.GetX(2));
        auto* ltm = reinterpret_cast<const LinuxTm*>(c.GetX(3));
        std::tm tm_v{};
        tm_v.tm_sec = ltm->tm_sec; tm_v.tm_min = ltm->tm_min; tm_v.tm_hour = ltm->tm_hour;
        tm_v.tm_mday = ltm->tm_mday; tm_v.tm_mon = ltm->tm_mon; tm_v.tm_year = ltm->tm_year;
        tm_v.tm_wday = ltm->tm_wday; tm_v.tm_yday = ltm->tm_yday; tm_v.tm_isdst = ltm->tm_isdst;
        c.SetX(0, std::strftime(buf, maxsz, fmt, &tm_v));
    };
    fn("strftime", do_strftime);
    fn("strftime_l", do_strftime);
    fn("sleep", [](CpuContext& c) { Sleep(c.GetW(0) * 1000); c.SetW(0, 0); });
    fn("usleep", [this](CpuContext& c) {
        uint32_t us = c.GetW(0);
        uint64_t lr = c.GetLR();
        if (skip_menu_usleep_ && lr >= menu_usleep_lo_ && lr < menu_usleep_hi_) {
            c.SetW(0, 0);
            return;
        }
        if (us >= 1000) Sleep(us / 1000);
        else if (us > 0) std::this_thread::yield();
        c.SetW(0, 0);
    });
    fn("nanosleep", [](CpuContext& c) {
        auto* ts = reinterpret_cast<const int64_t*>(c.GetX(0));
        if (ts) {
            int64_t ms = ts[0] * 1000 + ts[1] / 1000000;
            if (ms > 0) Sleep(static_cast<DWORD>(ms));
            else std::this_thread::yield();
        }
        c.SetW(0, 0);
    });
    fn("sched_yield", [](CpuContext& c) { std::this_thread::yield(); c.SetW(0, 0); });

    // ---- qsort ----
    fn("qsort", [this](CpuContext& c) {
        auto* base = reinterpret_cast<uint8_t*>(c.GetX(0));
        size_t nmemb = static_cast<size_t>(c.GetX(1));
        size_t size = static_cast<size_t>(c.GetX(2));
        uint64_t cmp = c.GetX(3);
        if (nmemb > 1 && size > 0 && base && cmp) {
            std::vector<uint8_t> tmp(size);
            GuestQuickSort(cpu_, base, nmemb, size, cmp, tmp);
        }
    });

    // ---- setjmp / longjmp ----
    fn("setjmp", [](CpuContext& c) {
        auto* buf = reinterpret_cast<uint64_t*>(c.GetX(0));
        for (int i = 0; i < 12; ++i) buf[i] = c.GetX(19 + i); // x19..x30
        buf[12] = c.GetSP();
        for (int i = 0; i < 8; ++i) buf[13 + i] = c.jit->GetVector(8 + i)[0]; // d8..d15
        c.SetX(0, 0);
    });
    fn("longjmp", [](CpuContext& c) {
        auto* buf = reinterpret_cast<const uint64_t*>(c.GetX(0));
        int val = static_cast<int>(c.GetW(1));
        for (int i = 0; i < 12; ++i) c.SetX(19 + i, buf[i]); // restores x30 (LR) too!
        c.SetSP(buf[12]);
        for (int i = 0; i < 8; ++i) {
            Dynarmic::A64::Vector v{buf[13 + i], 0};
            c.jit->SetVector(8 + i, v);
        }
        c.SetX(0, static_cast<uint64_t>(val ? val : 1));
    });

    // ---- pthreads / semaphores / emutls ----
    fn("pthread_attr_init", [](CpuContext& c) { c.SetW(0, 0); });
    fn("pthread_attr_destroy", [](CpuContext& c) { c.SetW(0, 0); });
    fn("pthread_attr_setdetachstate", [](CpuContext& c) { c.SetW(0, 0); });
    fn("pthread_attr_setstacksize", [](CpuContext& c) { c.SetW(0, 0); });
    fn("pthread_mutexattr_init", [](CpuContext& c) { c.SetW(0, 0); });
    fn("pthread_mutexattr_destroy", [](CpuContext& c) { c.SetW(0, 0); });
    fn("pthread_mutexattr_settype", [](CpuContext& c) { c.SetW(0, 0); });
    fn("pthread_mutex_init", [](CpuContext& c) { (void)GetMutex(c.GetX(0)); c.SetW(0, 0); });
    fn("pthread_mutex_destroy", [](CpuContext& c) {
        std::lock_guard<std::mutex> lk(g_sync_map_mtx);
        g_mutexes.erase(c.GetX(0));
        c.SetW(0, 0);
    });
    fn("pthread_mutex_lock", [](CpuContext& c) { GetMutex(c.GetX(0)).mtx.lock(); c.SetW(0, 0); });
    fn("pthread_mutex_trylock", [](CpuContext& c) { c.SetW(0, GetMutex(c.GetX(0)).mtx.try_lock() ? 0u : 16u); });
    fn("pthread_mutex_unlock", [](CpuContext& c) { GetMutex(c.GetX(0)).mtx.unlock(); c.SetW(0, 0); });

    fn("pthread_cond_init", [](CpuContext& c) { (void)GetCond(c.GetX(0)); c.SetW(0, 0); });
    fn("pthread_cond_destroy", [](CpuContext& c) {
        std::lock_guard<std::mutex> lk(g_sync_map_mtx);
        g_conds.erase(c.GetX(0));
        c.SetW(0, 0);
    });
    fn("pthread_cond_signal", [](CpuContext& c) { GetCond(c.GetX(0)).cv.notify_one(); c.SetW(0, 0); });
    fn("pthread_cond_broadcast", [](CpuContext& c) { GetCond(c.GetX(0)).cv.notify_all(); c.SetW(0, 0); });
    fn("pthread_cond_wait", [](CpuContext& c) {
        auto& cv = GetCond(c.GetX(0)).cv;
        auto& mtx = GetMutex(c.GetX(1)).mtx;
        cv.wait(mtx);
        c.SetW(0, 0);
    });
    fn("pthread_cond_timedwait", [](CpuContext& c) {
        auto& cv = GetCond(c.GetX(0)).cv;
        auto& mtx = GetMutex(c.GetX(1)).mtx;
        auto* ts = reinterpret_cast<const int64_t*>(c.GetX(2));
        auto dur = std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds(ts[0]) + std::chrono::nanoseconds(ts[1]));
        auto tp = std::chrono::system_clock::time_point(dur);
        auto st = cv.wait_until(mtx, tp);
        c.SetW(0, (st == std::cv_status::timeout) ? 110u : 0u); // ETIMEDOUT = 110
    });

    fn("pthread_rwlock_init", [](CpuContext& c) { (void)GetMutex(c.GetX(0)); c.SetW(0, 0); });
    fn("pthread_rwlock_destroy", [](CpuContext& c) {
        std::lock_guard<std::mutex> lk(g_sync_map_mtx);
        g_mutexes.erase(c.GetX(0));
        c.SetW(0, 0);
    });
    // Use recursive mutex for rwlock so unlock never mismatches shared vs exclusive.
    fn("pthread_rwlock_rdlock", [](CpuContext& c) { GetMutex(c.GetX(0)).mtx.lock(); c.SetW(0, 0); });
    fn("pthread_rwlock_wrlock", [](CpuContext& c) { GetMutex(c.GetX(0)).mtx.lock(); c.SetW(0, 0); });
    fn("pthread_rwlock_unlock", [](CpuContext& c) { GetMutex(c.GetX(0)).mtx.unlock(); c.SetW(0, 0); });

    fn("pthread_once", [this](CpuContext& c) {
        auto* ctrl = reinterpret_cast<volatile long*>(c.GetX(0));
        uint64_t init_fn = c.GetX(1);
        long prev = _InterlockedCompareExchange(ctrl, 1, 0);
        if (prev == 0) {
            cpu_.CallGuest(init_fn);
            _InterlockedExchange(ctrl, 2);
        } else {
            while (_InterlockedCompareExchange(ctrl, 2, 2) != 2) {
                std::this_thread::yield();
            }
        }
        c.SetW(0, 0);
    });

    fn("pthread_key_create", [](CpuContext& c) {
        auto* key_out = reinterpret_cast<uint32_t*>(c.GetX(0));
        *key_out = g_next_key.fetch_add(1);
        c.SetW(0, 0);
    });
    fn("pthread_key_delete", [](CpuContext& c) { c.SetW(0, 0); });
    fn("pthread_getspecific", [](CpuContext& c) {
        uint32_t k = c.GetW(0);
        auto it = tls_pthread_keys.find(k);
        c.SetX(0, it != tls_pthread_keys.end() ? it->second : 0);
    });
    fn("pthread_setspecific", [](CpuContext& c) {
        tls_pthread_keys[c.GetW(0)] = c.GetX(1);
        c.SetW(0, 0);
    });
    fn("pthread_self", [](CpuContext& c) { c.SetX(0, tls_my_pthread_id); });
    fn("pthread_equal", [](CpuContext& c) { c.SetW(0, c.GetX(0) == c.GetX(1) ? 1u : 0u); });
    fn("pthread_create", [this](CpuContext& c) {
        auto* tid_out = reinterpret_cast<uint64_t*>(c.GetX(0));
        uint64_t start_fn = c.GetX(2);
        uint64_t arg = c.GetX(3);
        uint64_t tid = g_next_tid.fetch_add(1);
        auto ht = std::make_unique<HostThread>();
        HostThread* raw = ht.get();
        {
            std::lock_guard<std::mutex> lk(g_sync_map_mtx);
            g_threads[tid] = std::move(ht);
        }
        raw->th = std::thread([this, raw, tid, start_fn, arg]() {
            tls_my_pthread_id = tid;
            cpu_.EnsureThreadInit();
            raw->retval = cpu_.CallGuest(start_fn, {arg});
            raw->done.store(true);
        });
        if (tid_out) *tid_out = tid;
        c.SetW(0, 0);
    });
    fn("pthread_join", [](CpuContext& c) {
        uint64_t tid = c.GetX(0);
        auto* ret_out = reinterpret_cast<uint64_t*>(c.GetX(1));
        HostThread* raw = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_sync_map_mtx);
            auto it = g_threads.find(tid);
            if (it != g_threads.end()) raw = it->second.get();
        }
        if (raw && raw->th.joinable()) raw->th.join();
        if (raw && ret_out) *ret_out = raw->retval;
        {
            std::lock_guard<std::mutex> lk(g_sync_map_mtx);
            g_threads.erase(tid);
        }
        c.SetW(0, 0);
    });
    fn("pthread_detach", [](CpuContext& c) {
        uint64_t tid = c.GetX(0);
        std::lock_guard<std::mutex> lk(g_sync_map_mtx);
        auto it = g_threads.find(tid);
        if (it != g_threads.end() && it->second->th.joinable()) {
            it->second->detached = true;
            it->second->th.detach();
        }
        c.SetW(0, 0);
    });
    fn("pthread_exit", [](CpuContext& c) {
        c.jit->HaltExecution();
    });

    fn("sem_init", [](CpuContext& c) {
        auto& s = GetSem(c.GetX(0));
        std::lock_guard<std::mutex> lk(s.mtx);
        s.count = c.GetW(2);
        c.SetW(0, 0);
    });
    fn("sem_destroy", [](CpuContext& c) {
        std::lock_guard<std::mutex> lk(g_sync_map_mtx);
        g_sems.erase(c.GetX(0));
        c.SetW(0, 0);
    });
    fn("sem_wait", [](CpuContext& c) {
        auto& s = GetSem(c.GetX(0));
        std::unique_lock<std::mutex> lk(s.mtx);
        s.cv.wait(lk, [&]() { return s.count > 0; });
        --s.count;
        c.SetW(0, 0);
    });
    fn("sem_post", [](CpuContext& c) {
        auto& s = GetSem(c.GetX(0));
        { std::lock_guard<std::mutex> lk(s.mtx); ++s.count; }
        s.cv.notify_one();
        c.SetW(0, 0);
    });
    fn("sem_open", [](CpuContext& c) { c.SetX(0, static_cast<uint64_t>(-1)); tls_bionic_errno = 38; });
    fn("sem_close", [](CpuContext& c) { c.SetW(0, 0); });
    fn("sem_unlink", [](CpuContext& c) { c.SetW(0, 0); });

    fn("__emutls_get_address", [](CpuContext& c) {
        struct EmutlsCtrl { uint64_t size, align, index; void* templ; };
        auto* ctrl = reinterpret_cast<EmutlsCtrl*>(c.GetX(0));
        uint64_t key = reinterpret_cast<uint64_t>(ctrl);
        auto& slot = tls_emutls_slots[key];
        if (slot.empty()) {
            size_t sz = ctrl && ctrl->size ? static_cast<size_t>(ctrl->size) : 8;
            slot.assign(sz, 0);
            if (ctrl && ctrl->templ) {
                std::memcpy(slot.data(), ctrl->templ, sz);
            }
        }
        c.SetX(0, reinterpret_cast<uint64_t>(slot.data()));
    });

    // ---- dlfcn & dl_iterate_phdr ----
    fn("dlopen", [this](CpuContext& c) {
        const char* name = reinterpret_cast<const char*>(c.GetX(0));
        std::string s = name ? name : "";
        if (s.empty() || s.find("libc") != std::string::npos ||
            s.find("libRealRacing3") != std::string::npos ||
            s.find("libGLES") != std::string::npos ||
            s.find("libEGL") != std::string::npos ||
            s.find("libOpenSLES") != std::string::npos) {
            auto h = std::make_unique<DlHandle>();
            h->name = s;
            h->is_self = s.empty();
            uint64_t ptr = reinterpret_cast<uint64_t>(h.get());
            std::lock_guard<std::mutex> lk(g_dl_mtx);
            g_dl_handles.push_back(std::move(h));
            c.SetX(0, ptr);
            return;
        }
        tls_dlerror = "dlopen: library not found: " + s;
        c.SetX(0, 0);
    });
    fn("dlsym", [this](CpuContext& c) {
        uint64_t hval = c.GetX(0);
        const char* sym = reinterpret_cast<const char*>(c.GetX(1));
        if (!sym) { c.SetX(0, 0); return; }
        std::string lib_name;
        if (hval != 0 && hval != UINT64_MAX) {
            lib_name = reinterpret_cast<DlHandle*>(hval)->name;
        }
        if (custom_dl_) {
            uint64_t custom = custom_dl_(lib_name, sym);
            if (custom != 0) { c.SetX(0, custom); return; }
        }
        uint64_t addr = loader_.FindHostSymbol(sym);
        if (!addr) addr = loader_.FindExport(sym);
        if (!addr) tls_dlerror = std::string("dlsym: symbol not found: ") + sym;
        c.SetX(0, addr);
    });
    fn("dlclose", [](CpuContext& c) { c.SetW(0, 0); });
    fn("dlerror", [](CpuContext& c) {
        if (tls_dlerror.empty()) { c.SetX(0, 0); return; }
        c.SetX(0, reinterpret_cast<uint64_t>(tls_dlerror.c_str()));
    });
    fn("dl_iterate_phdr", [this](CpuContext& c) {
        uint64_t cb = c.GetX(0);
        uint64_t data = c.GetX(1);
        int rc = 0;
        for (const auto& m : loader_.Modules()) {
            LinuxDlPhdrInfo info{};
            info.dlpi_addr = m.load_bias;
            info.dlpi_name = reinterpret_cast<uint64_t>(m.name.c_str());
            info.dlpi_phdr = m.phdr_guest_addr;
            info.dlpi_phnum = m.phnum;
            rc = static_cast<int>(cpu_.CallGuest(cb, {reinterpret_cast<uint64_t>(&info), sizeof(info), data}));
            if (rc != 0) break;
        }
        c.SetW(0, static_cast<uint32_t>(rc));
    });

    // ---- Sockets / network (strictly offline by default; real Winsock2 bridge when online_mode_ is true) ----
    auto net_fail = [](CpuContext& c) { tls_bionic_errno = 101; c.SetX(0, static_cast<uint64_t>(-1LL)); }; // ENETUNREACH
    fn("socket", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        int domain = LinuxAfToWin(static_cast<int>(c.GetW(0)));
        int raw_type = static_cast<int>(c.GetW(1));
        int proto = static_cast<int>(c.GetW(2));
        bool nonblock = (raw_type & 0x800) != 0; // Linux SOCK_NONBLOCK
        int type = raw_type & 0xF;
        SOCKET s = ::socket(domain, type, proto);
        if (s == INVALID_SOCKET) { SyncWsaErrno(); c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
        if (nonblock) {
            u_long mode = 1;
            ioctlsocket(s, FIONBIO, &mode);
        }
        int vfd = AllocVirtSocketFd(s, nonblock);
        if (vfd < 0) {
            closesocket(s);
            tls_bionic_errno = 24; // EMFILE
            c.SetW(0, static_cast<uint32_t>(-1));
            return;
        }
        c.SetW(0, static_cast<uint32_t>(vfd));
    });
    fn("connect", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        sockaddr_storage ss{};
        int slen = 0;
        if (!CopyLinuxSockaddrToWin(reinterpret_cast<const void*>(c.GetX(1)), static_cast<int>(c.GetW(2)), ss, slen)) {
            tls_bionic_errno = 22; c.SetW(0, static_cast<uint32_t>(-1)); return;
        }
        int r = ::connect(s, reinterpret_cast<const sockaddr*>(&ss), slen);
        if (r == SOCKET_ERROR) {
            int werr = WSAGetLastError();
            tls_bionic_errno = (werr == WSAEWOULDBLOCK) ? 115 : WsaToLinuxErrno(werr); // EINPROGRESS for non-blocking connect
            c.SetW(0, static_cast<uint32_t>(-1));
            return;
        }
        c.SetW(0, 0);
    });
    fn("bind", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        sockaddr_storage ss{};
        int slen = 0;
        if (!CopyLinuxSockaddrToWin(reinterpret_cast<const void*>(c.GetX(1)), static_cast<int>(c.GetW(2)), ss, slen)) {
            tls_bionic_errno = 22; c.SetW(0, static_cast<uint32_t>(-1)); return;
        }
        int r = ::bind(s, reinterpret_cast<const sockaddr*>(&ss), slen);
        if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetW(0, static_cast<uint32_t>(-1)); return; }
        c.SetW(0, 0);
    });
    fn("listen", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        int r = ::listen(s, static_cast<int>(c.GetW(1)));
        if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetW(0, static_cast<uint32_t>(-1)); return; }
        c.SetW(0, 0);
    });
    fn("accept", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        sockaddr_storage ss{};
        int slen = sizeof(ss);
        SOCKET ns = ::accept(s, reinterpret_cast<sockaddr*>(&ss), &slen);
        if (ns == INVALID_SOCKET) { SyncWsaErrno(); c.SetW(0, static_cast<uint32_t>(-1)); return; }
        int vfd = AllocVirtSocketFd(ns, false);
        if (vfd < 0) {
            closesocket(ns);
            tls_bionic_errno = 24; // EMFILE
            c.SetW(0, static_cast<uint32_t>(-1));
            return;
        }
        CopyWinSockaddrToLinux(ss, slen, reinterpret_cast<void*>(c.GetX(1)), reinterpret_cast<uint32_t*>(c.GetX(2)));
        c.SetW(0, static_cast<uint32_t>(vfd));
    });
    fn("send", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
        int r = ::send(s, reinterpret_cast<const char*>(c.GetX(1)), static_cast<int>(c.GetX(2)), 0);
        if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
        c.SetX(0, static_cast<uint64_t>(static_cast<int64_t>(r)));
    });
    fn("sendto", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
        const void* dst = reinterpret_cast<const void*>(c.GetX(4));
        int r = SOCKET_ERROR;
        if (dst) {
            sockaddr_storage ss{};
            int slen = 0;
            if (!CopyLinuxSockaddrToWin(dst, static_cast<int>(c.GetW(5)), ss, slen)) {
                tls_bionic_errno = 22; c.SetX(0, static_cast<uint64_t>(-1LL)); return;
            }
            r = ::sendto(s, reinterpret_cast<const char*>(c.GetX(1)), static_cast<int>(c.GetX(2)), 0,
                         reinterpret_cast<const sockaddr*>(&ss), slen);
        } else {
            r = ::send(s, reinterpret_cast<const char*>(c.GetX(1)), static_cast<int>(c.GetX(2)), 0);
        }
        if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
        c.SetX(0, static_cast<uint64_t>(static_cast<int64_t>(r)));
    });
    fn("sendmmsg", net_fail);
    fn("recv", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
        int lflags = static_cast<int>(c.GetW(3));
        int wflags = (lflags & 2) ? MSG_PEEK : 0;
        int r = ::recv(s, reinterpret_cast<char*>(c.GetX(1)), static_cast<int>(c.GetX(2)), wflags);
        if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
        c.SetX(0, static_cast<uint64_t>(static_cast<int64_t>(r)));
    });
    fn("recvfrom", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
        int lflags = static_cast<int>(c.GetW(3));
        int wflags = (lflags & 2) ? MSG_PEEK : 0;
        sockaddr_storage ss{};
        int slen = sizeof(ss);
        void* src_addr = reinterpret_cast<void*>(c.GetX(4));
        uint32_t* src_len = reinterpret_cast<uint32_t*>(c.GetX(5));
        int r = src_addr
                    ? ::recvfrom(s, reinterpret_cast<char*>(c.GetX(1)), static_cast<int>(c.GetX(2)), wflags,
                                 reinterpret_cast<sockaddr*>(&ss), &slen)
                    : ::recv(s, reinterpret_cast<char*>(c.GetX(1)), static_cast<int>(c.GetX(2)), wflags);
        if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetX(0, static_cast<uint64_t>(-1LL)); return; }
        if (src_addr && src_len) CopyWinSockaddrToLinux(ss, slen, src_addr, src_len);
        c.SetX(0, static_cast<uint64_t>(static_cast<int64_t>(r)));
    });
    fn("recvmmsg", net_fail);
    fn("shutdown", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        int r = ::shutdown(s, static_cast<int>(c.GetW(1)));
        if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetW(0, static_cast<uint32_t>(-1)); return; }
        c.SetW(0, 0);
    });
    fn("getsockopt", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        int level = static_cast<int>(c.GetW(1));
        int opt = static_cast<int>(c.GetW(2));
        void* val = reinterpret_cast<void*>(c.GetX(3));
        uint32_t* lenp = reinterpret_cast<uint32_t*>(c.GetX(4));
        if (level == 1 && opt == 4 && val && lenp && *lenp >= 4) { // SOL_SOCKET + SO_ERROR
            int werr = 0; int wlen = sizeof(werr);
            int r = ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&werr), &wlen);
            if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetW(0, static_cast<uint32_t>(-1)); return; }
            *reinterpret_cast<int32_t*>(val) = WsaToLinuxErrno(werr);
            *lenp = 4;
            c.SetW(0, 0);
            return;
        }
        c.SetW(0, 0);
    });
    fn("setsockopt", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        int level = static_cast<int>(c.GetW(1));
        int opt = static_cast<int>(c.GetW(2));
        const void* val = reinterpret_cast<const void*>(c.GetX(3));
        int len = static_cast<int>(c.GetW(4));
        if (level == 1) { // Linux SOL_SOCKET
            if ((opt == 20 || opt == 21) && val && len >= 16) { // SO_RCVTIMEO / SO_SNDTIMEO
                const auto* tv = reinterpret_cast<const LinuxTimeval*>(val);
                DWORD ms = static_cast<DWORD>(std::max<int64_t>(0, tv->tv_sec * 1000 + tv->tv_usec / 1000));
                ::setsockopt(s, SOL_SOCKET, opt == 20 ? SO_RCVTIMEO : SO_SNDTIMEO,
                             reinterpret_cast<const char*>(&ms), sizeof(ms));
            } else if (opt == 2 && val && len >= 4) { // SO_REUSEADDR
                ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(val), 4);
            } else if (opt == 9 && val && len >= 4) { // SO_KEEPALIVE
                ::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(val), 4);
            } else if (opt == 7 && val && len >= 4) { // SO_SNDBUF
                ::setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(val), 4);
            } else if (opt == 8 && val && len >= 4) { // SO_RCVBUF
                ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(val), 4);
            }
        } else if (level == IPPROTO_TCP && opt == 1 && val && len >= 4) { // TCP_NODELAY
            ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(val), 4);
        }
        c.SetW(0, 0);
    });
    fn("getsockname", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        sockaddr_storage ss{}; int slen = sizeof(ss);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&ss), &slen) == SOCKET_ERROR) {
            SyncWsaErrno(); c.SetW(0, static_cast<uint32_t>(-1)); return;
        }
        CopyWinSockaddrToLinux(ss, slen, reinterpret_cast<void*>(c.GetX(1)), reinterpret_cast<uint32_t*>(c.GetX(2)));
        c.SetW(0, 0);
    });
    fn("getpeername", [this, net_fail](CpuContext& c) {
        if (!online_mode_) { net_fail(c); return; }
        SOCKET s = INVALID_SOCKET;
        if (!LookupVirtSocket(static_cast<int>(c.GetW(0)), s)) { tls_bionic_errno = 9; c.SetW(0, static_cast<uint32_t>(-1)); return; }
        sockaddr_storage ss{}; int slen = sizeof(ss);
        if (::getpeername(s, reinterpret_cast<sockaddr*>(&ss), &slen) == SOCKET_ERROR) {
            SyncWsaErrno(); c.SetW(0, static_cast<uint32_t>(-1)); return;
        }
        CopyWinSockaddrToLinux(ss, slen, reinterpret_cast<void*>(c.GetX(1)), reinterpret_cast<uint32_t*>(c.GetX(2)));
        c.SetW(0, 0);
    });
    fn("poll", [this](CpuContext& c) {
        auto* pfds = reinterpret_cast<LinuxPollFd*>(c.GetX(0));
        uint32_t nfds = c.GetW(1);
        int timeout_ms = static_cast<int>(c.GetW(2));
        if (!online_mode_ || !pfds || nfds == 0) {
            if (timeout_ms < 0) Sleep(50);
            else if (timeout_ms > 0) Sleep(static_cast<DWORD>(std::min(timeout_ms, 50)));
            c.SetW(0, 0);
            return;
        }
        std::vector<WSAPOLLFD> wfds(nfds);
        bool has_sock = false;
        for (uint32_t i = 0; i < nfds; ++i) {
            pfds[i].revents = 0;
            SOCKET s = INVALID_SOCKET;
            if (LookupVirtSocket(pfds[i].fd, s)) {
                wfds[i].fd = s;
                SHORT ev = 0;
                if (pfds[i].events & 0x001) ev |= POLLRDNORM;
                if (pfds[i].events & 0x004) ev |= POLLWRNORM;
                wfds[i].events = ev;
                wfds[i].revents = 0;
                has_sock = true;
            } else {
                wfds[i].fd = INVALID_SOCKET;
                wfds[i].events = 0;
                wfds[i].revents = 0;
            }
        }
        if (!has_sock) {
            if (timeout_ms > 0) Sleep(static_cast<DWORD>(std::min(timeout_ms, 50)));
            c.SetW(0, 0);
            return;
        }
        int r = WSAPoll(wfds.data(), nfds, timeout_ms);
        if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetW(0, static_cast<uint32_t>(-1)); return; }
        for (uint32_t i = 0; i < nfds; ++i) {
            int16_t rev = 0;
            if (wfds[i].revents & (POLLRDNORM | POLLRDBAND)) rev |= 0x001; // POLLIN
            if (wfds[i].revents & POLLWRNORM) rev |= 0x004;                // POLLOUT
            if (wfds[i].revents & POLLERR) rev |= 0x008;                   // POLLERR
            if (wfds[i].revents & POLLHUP) rev |= 0x010;                   // POLLHUP
            if (wfds[i].revents & POLLNVAL) rev |= 0x020;                  // POLLNVAL
            pfds[i].revents = rev;
        }
        c.SetW(0, static_cast<uint32_t>(r));
    });
    fn("select", [this](CpuContext& c) {
        int nfds = static_cast<int>(c.GetW(0));
        auto* rfds = reinterpret_cast<uint64_t*>(c.GetX(1));
        auto* wfds = reinterpret_cast<uint64_t*>(c.GetX(2));
        auto* efds = reinterpret_cast<uint64_t*>(c.GetX(3));
        auto* ltv = reinterpret_cast<const LinuxTimeval*>(c.GetX(4));
        if (!online_mode_) {
            if (!ltv) Sleep(50);
            else {
                int64_t ms = ltv->tv_sec * 1000 + ltv->tv_usec / 1000;
                if (ms > 0) Sleep(static_cast<DWORD>(std::min<int64_t>(ms, 50)));
            }
            c.SetW(0, 0);
            return;
        }
        // Linux fd_set supports FDs 0..1023; virtual sockets use FD>=10000 so poll() is preferred,
        // but support any FD < 1024 cleanly and cap idle sleep cleanly.
        fd_set wr{}, ww{}, we{};
        FD_ZERO(&wr); FD_ZERO(&ww); FD_ZERO(&we);
        std::vector<std::pair<int, SOCKET>> tracked;
        int limit = std::min(nfds, 1024);
        auto check_set = [&](uint64_t* bits, fd_set& dst) {
            if (!bits) return;
            for (int fd = 0; fd < limit; ++fd) {
                if (bits[fd / 64] & (1ULL << (fd % 64))) {
                    SOCKET s = INVALID_SOCKET;
                    if (LookupVirtSocket(fd, s)) {
                        FD_SET(s, &dst);
                        tracked.emplace_back(fd, s);
                    }
                }
            }
        };
        check_set(rfds, wr);
        check_set(wfds, ww);
        check_set(efds, we);
        if (tracked.empty()) {
            if (ltv) {
                int64_t ms = ltv->tv_sec * 1000 + ltv->tv_usec / 1000;
                if (ms > 0) Sleep(static_cast<DWORD>(std::min<int64_t>(ms, 50)));
            }
            c.SetW(0, 0);
            return;
        }
        timeval wtv{};
        timeval* ptv = nullptr;
        if (ltv) {
            wtv.tv_sec = static_cast<long>(ltv->tv_sec);
            wtv.tv_usec = static_cast<long>(ltv->tv_usec);
            ptv = &wtv;
        }
        int r = ::select(0, rfds ? &wr : nullptr, wfds ? &ww : nullptr, efds ? &we : nullptr, ptv);
        if (r == SOCKET_ERROR) { SyncWsaErrno(); c.SetW(0, static_cast<uint32_t>(-1)); return; }
        auto write_back = [&](uint64_t* bits, fd_set& src) {
            if (!bits) return;
            std::memset(bits, 0, 128);
            for (auto [fd, s] : tracked) {
                if (FD_ISSET(s, &src)) bits[fd / 64] |= (1ULL << (fd % 64));
            }
        };
        write_back(rfds, wr);
        write_back(wfds, ww);
        write_back(efds, we);
        c.SetW(0, static_cast<uint32_t>(r));
    });
    fn("__FD_SET_chk", [](CpuContext& c) {
        int fd = static_cast<int>(c.GetW(0));
        auto* bits = reinterpret_cast<uint64_t*>(c.GetX(1));
        if (bits && fd >= 0 && fd < 1024) bits[fd / 64] |= (1ULL << (fd % 64));
    });
    fn("__FD_ISSET_chk", [](CpuContext& c) {
        int fd = static_cast<int>(c.GetW(0));
        auto* bits = reinterpret_cast<const uint64_t*>(c.GetX(1));
        int set = (bits && fd >= 0 && fd < 1024 && (bits[fd / 64] & (1ULL << (fd % 64)))) ? 1 : 0;
        c.SetW(0, static_cast<uint32_t>(set));
    });
    fn("__cmsg_nxthdr", [](CpuContext& c) { c.SetX(0, 0); });
    fn("getaddrinfo", [this](CpuContext& c) {
        if (!online_mode_) { c.SetW(0, static_cast<uint32_t>(-2)); return; }
        const char* node = reinterpret_cast<const char*>(c.GetX(0));
        const char* service = reinterpret_cast<const char*>(c.GetX(1));
        const auto* lhints = reinterpret_cast<const LinuxAddrinfo*>(c.GetX(2));
        auto** res_out = reinterpret_cast<LinuxAddrinfo**>(c.GetX(3));
        if (!res_out) { c.SetW(0, static_cast<uint32_t>(-1)); return; }
        *res_out = nullptr;
        addrinfo whints{};
        if (lhints) {
            whints.ai_flags = lhints->ai_flags;
            whints.ai_family = LinuxAfToWin(lhints->ai_family);
            whints.ai_socktype = lhints->ai_socktype;
            whints.ai_protocol = lhints->ai_protocol;
        }
        addrinfo* wres = nullptr;
        int rc = ::getaddrinfo(node, service, lhints ? &whints : nullptr, &wres);
        if (rc != 0 || !wres) { c.SetW(0, static_cast<uint32_t>(-2)); return; }
        LinuxAddrinfo* head = nullptr;
        LinuxAddrinfo* tail = nullptr;
        for (addrinfo* cur = wres; cur; cur = cur->ai_next) {
            auto* la = new LinuxAddrinfo{};
            la->ai_flags = cur->ai_flags;
            la->ai_family = WinAfToLinux(cur->ai_family);
            la->ai_socktype = cur->ai_socktype;
            la->ai_protocol = cur->ai_protocol;
            la->ai_addrlen = static_cast<uint32_t>(cur->ai_addrlen);
            if (cur->ai_addr && cur->ai_addrlen > 0) {
                auto* sa_copy = new uint8_t[cur->ai_addrlen];
                std::memcpy(sa_copy, cur->ai_addr, cur->ai_addrlen);
                uint16_t fam = static_cast<uint16_t>(WinAfToLinux(cur->ai_addr->sa_family));
                std::memcpy(sa_copy, &fam, 2);
                la->ai_addr = reinterpret_cast<uint64_t>(sa_copy);
            }
            if (cur->ai_canonname) {
                size_t clen = std::strlen(cur->ai_canonname) + 1;
                char* cn = new char[clen];
                std::memcpy(cn, cur->ai_canonname, clen);
                la->ai_canonname = reinterpret_cast<uint64_t>(cn);
            }
            if (!head) head = la;
            if (tail) tail->ai_next = reinterpret_cast<uint64_t>(la);
            tail = la;
        }
        ::freeaddrinfo(wres);
        *res_out = head;
        c.SetW(0, 0);
    });
    fn("freeaddrinfo", [](CpuContext& c) {
        auto* cur = reinterpret_cast<LinuxAddrinfo*>(c.GetX(0));
        while (cur) {
            auto* nxt = reinterpret_cast<LinuxAddrinfo*>(cur->ai_next);
            delete[] reinterpret_cast<uint8_t*>(cur->ai_addr);
            delete[] reinterpret_cast<char*>(cur->ai_canonname);
            delete cur;
            cur = nxt;
        }
    });
    fn("gai_strerror", [](CpuContext& c) {
        static const char* s = "name resolution failed";
        c.SetX(0, reinterpret_cast<uint64_t>(s));
    });
    fn("gethostbyname", [this](CpuContext& c) {
        if (!online_mode_) { c.SetX(0, 0); return; }
        const char* name = reinterpret_cast<const char*>(c.GetX(0));
        if (!name) { c.SetX(0, 0); return; }
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (::getaddrinfo(name, nullptr, &hints, &res) != 0 || !res) { c.SetX(0, 0); return; }
        thread_local LinuxHostent he{};
        thread_local char name_buf[256]{};
        thread_local uint32_t ipv4_addr = 0;
        thread_local uint64_t addr_ptrs[2]{};
        thread_local uint64_t alias_ptrs[1]{0};
        std::strncpy(name_buf, name, sizeof(name_buf) - 1);
        ipv4_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
        ::freeaddrinfo(res);
        addr_ptrs[0] = reinterpret_cast<uint64_t>(&ipv4_addr);
        addr_ptrs[1] = 0;
        he.h_name = reinterpret_cast<uint64_t>(name_buf);
        he.h_aliases = reinterpret_cast<uint64_t>(alias_ptrs);
        he.h_addrtype = AF_INET;
        he.h_length = 4;
        he.h_addr_list = reinterpret_cast<uint64_t>(addr_ptrs);
        c.SetX(0, reinterpret_cast<uint64_t>(&he));
    });
    fn("getnameinfo", [this](CpuContext& c) {
        if (!online_mode_) { c.SetW(0, static_cast<uint32_t>(-2)); return; }
        sockaddr_storage ss{}; int slen = 0;
        if (!CopyLinuxSockaddrToWin(reinterpret_cast<const void*>(c.GetX(0)), static_cast<int>(c.GetW(1)), ss, slen)) {
            c.SetW(0, static_cast<uint32_t>(-2)); return;
        }
        int rc = ::getnameinfo(reinterpret_cast<const sockaddr*>(&ss), slen,
                               reinterpret_cast<char*>(c.GetX(2)), static_cast<DWORD>(c.GetW(3)),
                               reinterpret_cast<char*>(c.GetX(4)), static_cast<DWORD>(c.GetW(5)),
                               static_cast<int>(c.GetW(6)));
        c.SetW(0, static_cast<uint32_t>(rc));
    });
    fn("gethostname", [](CpuContext& c) {
        char* buf = reinterpret_cast<char*>(c.GetX(0));
        size_t len = static_cast<size_t>(c.GetX(1));
        if (buf && len > 0) { std::strncpy(buf, "rr3-pc", len - 1); buf[len - 1] = '\0'; }
        c.SetW(0, 0);
    });
    fn("if_nametoindex", [](CpuContext& c) { c.SetW(0, 0); });
    fn("inet_addr", [](CpuContext& c) {
        const char* cp = reinterpret_cast<const char*>(c.GetX(0));
        if (!cp) { c.SetW(0, 0xFFFFFFFFu); return; }
        in_addr ia{};
        if (::inet_pton(AF_INET, cp, &ia) == 1) {
            c.SetW(0, ia.s_addr);
        } else {
            c.SetW(0, 0xFFFFFFFFu);
        }
    });
    fn("inet_pton", [](CpuContext& c) {
        int af = LinuxAfToWin(static_cast<int>(c.GetW(0)));
        const char* src = reinterpret_cast<const char*>(c.GetX(1));
        void* dst = reinterpret_cast<void*>(c.GetX(2));
        c.SetW(0, static_cast<uint32_t>((src && dst) ? ::inet_pton(af, src, dst) : 0));
    });
    fn("inet_ntop", [](CpuContext& c) {
        int af = LinuxAfToWin(static_cast<int>(c.GetW(0)));
        const void* src = reinterpret_cast<const void*>(c.GetX(1));
        char* dst = reinterpret_cast<char*>(c.GetX(2));
        size_t sz = static_cast<size_t>(c.GetW(3));
        const char* r = (src && dst) ? ::inet_ntop(af, src, dst, sz) : nullptr;
        c.SetX(0, reinterpret_cast<uint64_t>(r));
    });
}

void BionicRuntime::RegisterSdkStubs() {
    auto noop0 = [&](const std::string& name) {
        loader_.RegisterHostFunction(name, [](CpuContext& c) { c.SetX(0, 0); });
    };
    // ARCore stubs (unsupported on PC)
    const char* ar_syms[] = {
        "ArCamera_getDisplayOrientedPose", "ArCamera_getTrackingState",
        "ArConfig_create", "ArConfig_destroy",
        "ArCoreApk_checkAvailability", "ArCoreApk_requestInstall",
        "ArFrame_acquireCamera", "ArFrame_create", "ArFrame_destroy",
        "ArPose_create", "ArPose_destroy", "ArPose_getMatrix",
        "ArSession_configure", "ArSession_create", "ArSession_destroy",
        "ArSession_pause", "ArSession_resume", "ArSession_setCameraTextureName", "ArSession_update",
        // Glu/Fuel Echo telemetry stubs
        "EchoCreate", "EchoDestroy", "EchoLogin", "EchoResume", "EchoSet", "EchoSuspend",
        // Anzu in-game ad stubs
        "Anzu_ApplicationActive", "Anzu_GetVersionFloat", "Anzu_Initialize",
        "Anzu_RegisterLogCallback", "Anzu_RegisterMessageCallback",
        "Anzu_RegisterTextureInitCallback", "Anzu_RegisterTextureUpdateCallback",
        "Anzu_RegisterUriSchemaHook", "Anzu_SetCoppaRegulated", "Anzu_SetGDPRConsent",
        "Anzu_Uninitialize", "Anzu__Texture_CreateInstance",
        "Anzu__Texture_NativeRenderer_AssignCustomHandler",
        "Anzu__Texture_NativeRenderer_GetRenderCallback",
        "Anzu__Texture_NativeRenderer_GetRenderID",
        "Anzu__Texture_NativeRenderer_SetExpectedFormat",
        "Anzu__Texture_PausePlayback", "Anzu__Texture_RemoveInstance",
        "Anzu__Texture_ResumePlayback", "Anzu__Texture_SetVisibility",
        "Anzu__Texture_SetVisibilityScore",
    };
    for (const char* s : ar_syms) noop0(s);
}
