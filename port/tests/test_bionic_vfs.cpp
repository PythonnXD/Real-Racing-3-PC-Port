#include "bionic_libc.h"
#include "cpu_dynarmic.h"
#include "elf_loader.h"
#include "jni_env.h"
#include "vfs.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define CHECK(expr)                                                                 \
    do {                                                                            \
        if (!(expr)) {                                                              \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

namespace {

uint64_t AllocCode(const std::vector<uint32_t>& insns) {
    void* mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(mem != nullptr);
    std::memcpy(mem, insns.data(), insns.size() * sizeof(uint32_t));
    return reinterpret_cast<uint64_t>(mem);
}

void TestVfsReadOnlyIsolation() {
    auto tmp_ud = std::filesystem::temp_directory_path() / "rr3_test_vfs_ud";
    std::filesystem::remove_all(tmp_ud);

    LayeredVfs vfs;
    const std::filesystem::path ro = R"(d:\Real Racing 3\com.ea.games.r3_row\files)";
    vfs.Initialize(ro, tmp_ud);

    // Reading existing asset resolves to read-only .depot or apk/res
    auto shader = vfs.ResolveRead("/storage/rr3/.depot/Shaders/2dimage.vsh");
    if (!std::filesystem::exists(shader)) {
        shader = vfs.ResolveRead("/storage/rr3/apk/res/Shaders/2dimage.vsh");
    }
    CHECK(std::filesystem::exists(shader));

    // Writing redirects strictly to userdata_dir, never readonly_files_dir
    auto wpath = vfs.ResolveWrite("/storage/rr3/doc/test_write.tmp");
    CHECK(wpath.string().find("rr3_test_vfs_ud") != std::string::npos);
    {
        std::ofstream out(wpath);
        out << "hello";
    }
    CHECK(std::filesystem::exists(wpath));
    CHECK(!std::filesystem::exists(ro / "doc" / "test_write.tmp"));

    // Subsequent read hits overlay
    CHECK(vfs.ResolveRead("/storage/rr3/doc/test_write.tmp") == wpath);

    std::filesystem::remove_all(tmp_ud);
    std::printf("  [PASS] test_vfs_read_only_isolation\n");
}

void TestAapcs64FormattingAndStat() {
    static_assert(sizeof(BionicStat64) == 128);
    static_assert(offsetof(BionicStat64, st_mode) == 0x10);
    static_assert(offsetof(BionicStat64, st_size) == 0x30);

    auto tmp_ud = std::filesystem::temp_directory_path() / "rr3_test_fmt_ud";
    std::filesystem::remove_all(tmp_ud);
    LayeredVfs vfs;
    vfs.Initialize(R"(d:\Real Racing 3\com.ea.games.r3_row\files)", tmp_ud);

    CpuRuntime cpu;
    ElfLoader loader(cpu);
    BionicRuntime bionic(cpu, loader, vfs);
    bionic.RegisterAll();

    // Call guest snprintf thunk with (buf, 128, "%s %d %.2f", "RR3", 60, 3.14)
    uint64_t snprintf_stub = loader.FindHostSymbol("snprintf");
    CHECK(snprintf_stub != 0);
    char out_buf[128]{};
    const char* fmt = "%s %d %.2f";
    const char* arg0 = "RR3";
    uint64_t written = cpu.CallGuestWithFloats(
        snprintf_stub,
        {reinterpret_cast<uint64_t>(out_buf), sizeof(out_buf),
         reinterpret_cast<uint64_t>(fmt), reinterpret_cast<uint64_t>(arg0), 60},
        {3.14});
    CHECK(written == 11);
    CHECK(std::strcmp(out_buf, "RR3 60 3.14") == 0);

    // Call guest stat thunk on real shader file (.depot or apk/res depending on pack layout)
    uint64_t stat_stub = loader.FindHostSymbol("stat");
    CHECK(stat_stub != 0);
    BionicStat64 st{};
    const char* p = std::filesystem::exists(vfs.ResolveRead("/storage/rr3/.depot/Shaders/2dimage.vsh"))
                        ? "/storage/rr3/.depot/Shaders/2dimage.vsh"
                        : "/storage/rr3/apk/res/Shaders/2dimage.vsh";
    int rc = static_cast<int>(cpu.CallGuest(stat_stub, {reinterpret_cast<uint64_t>(p), reinterpret_cast<uint64_t>(&st)}));
    CHECK(rc == 0);
    CHECK(st.st_size == 342);
    CHECK((st.st_mode & 0170000) == 0100000); // S_IFREG

    std::filesystem::remove_all(tmp_ud);
    std::printf("  [PASS] test_aapcs64_vsnprintf_and_stat (out='%s', st_size=%lld)\n",
                out_buf, static_cast<long long>(st.st_size));
}

void TestPthreadAndEmutls() {
    auto tmp_ud = std::filesystem::temp_directory_path() / "rr3_test_pth_ud";
    std::filesystem::remove_all(tmp_ud);
    LayeredVfs vfs;
    vfs.Initialize(R"(d:\Real Racing 3\com.ea.games.r3_row\files)", tmp_ud);

    CpuRuntime cpu;
    ElfLoader loader(cpu);
    BionicRuntime bionic(cpu, loader, vfs);
    bionic.RegisterAll();

    // Guest thread entry: returns x0 + 7
    // add x0, x0, #7 -> 0x91001c00
    // ret            -> 0xd65f03c0
    uint64_t thread_fn = AllocCode({0x91001c00u, 0xd65f03c0u});

    uint64_t pcreate = loader.FindHostSymbol("pthread_create");
    uint64_t pjoin = loader.FindHostSymbol("pthread_join");
    uint64_t emutls = loader.FindHostSymbol("__emutls_get_address");
    CHECK(pcreate && pjoin && emutls);

    uint64_t tid = 0;
    int rc = static_cast<int>(cpu.CallGuest(pcreate, {reinterpret_cast<uint64_t>(&tid), 0, thread_fn, 35}));
    CHECK(rc == 0 && tid != 0);
    uint64_t ret_val = 0;
    rc = static_cast<int>(cpu.CallGuest(pjoin, {tid, reinterpret_cast<uint64_t>(&ret_val)}));
    CHECK(rc == 0 && ret_val == 42);

    // Verify __emutls_get_address returns zero-initialized storage matching control struct {size, align, offset, templ}
    struct EmutlsControl { uint64_t size, align, index; void* templ; };
    uint64_t init_val = 0x123456789ABCDEF0ULL;
    EmutlsControl ctrl{8, 8, 0, &init_val};
    uint64_t slot1 = cpu.CallGuest(emutls, {reinterpret_cast<uint64_t>(&ctrl)});
    uint64_t slot2 = cpu.CallGuest(emutls, {reinterpret_cast<uint64_t>(&ctrl)});
    CHECK(slot1 != 0 && slot1 == slot2);
    CHECK(*reinterpret_cast<uint64_t*>(slot1) == init_val);

    VirtualFree(reinterpret_cast<void*>(thread_fn), 0, MEM_RELEASE);
    std::filesystem::remove_all(tmp_ud);
    std::printf("  [PASS] test_pthread_and_emutls (join=%llu)\n", static_cast<unsigned long long>(ret_val));
}

void TestRr3InitArrays() {
    auto tmp_ud = std::filesystem::temp_directory_path() / "rr3_test_init_ud";
    std::filesystem::remove_all(tmp_ud);
    LayeredVfs vfs;
    vfs.Initialize(R"(d:\Real Racing 3\com.ea.games.r3_row\files)", tmp_ud);

    CpuRuntime cpu;
    ElfLoader loader(cpu);
    BionicRuntime bionic(cpu, loader, vfs);
    bionic.RegisterAll();
    bionic.RegisterSdkStubs();

    // Provide real FakeJvm for EA::Nimble::getEnv() and EA::Nimble::findClass() called during .init_array!
    FakeJvm jvm(cpu);
    loader.RegisterHostFunction("_ZN2EA6Nimble6getEnvEv", [&jvm](CpuContext& ctx) {
        ctx.SetX(0, jvm.GetJniEnvGuestPtr());
    });
    loader.RegisterHostFunction("_ZN2EA6Nimble9findClassEPKc", [&jvm](CpuContext& ctx) {
        const char* nm = reinterpret_cast<const char*>(static_cast<uintptr_t>(ctx.GetX(0)));
        uint64_t h = nm ? jvm.FindClassHandle(nm) : 0;
        if (!h && nm) {
            std::fprintf(stderr, "    [Nimble::findClass] auto-stubbing '%s'\n", nm);
            h = jvm.RegisterJniClass(JniClassDef{nm});
        }
        ctx.SetX(0, h);
    });

    const std::filesystem::path bin_dir = R"(d:\Real Racing 3\port\work_bins\lib\arm64-v8a)";
    LoadedModule* m1 = loader.LoadModule(bin_dir / "libc++_shared.so");
    LoadedModule* m2 = loader.LoadModule(bin_dir / "libfmodex.so");
    LoadedModule* m3 = loader.LoadModule(bin_dir / "libRealRacing3.so");
    CHECK(m1 != nullptr && m2 != nullptr && m3 != nullptr);
    std::printf("    loaded: libc++=0x%llx, fmod=0x%llx, rr3=0x%llx\n",
                static_cast<unsigned long long>(m1->load_bias),
                static_cast<unsigned long long>(m2->load_bias),
                static_cast<unsigned long long>(m3->load_bias));
    std::fflush(stdout);

    std::vector<std::string> unresolved;
    loader.ApplyRelocations(&unresolved);
    // Print any remaining non-GL unresolved symbols
    size_t non_gl_unresolved = 0;
    for (const auto& s : unresolved) {
        if (s.rfind("gl", 0) != 0 && s.rfind("egl", 0) != 0) {
            std::printf("    [WARN] non-GL unresolved: %s\n", s.c_str());
            ++non_gl_unresolved;
        }
    }
    CHECK(non_gl_unresolved == 0);

    // PHASE 2 GATE: Execute ALL .init_array constructors of libc++_shared.so, libfmodex.so, and libRealRacing3.so!
    loader.RunInitArrays();

    std::filesystem::remove_all(tmp_ud);
    std::printf("  [PASS] test_rr3_init_arrays (ALL 181+ constructors executed cleanly!)\n");
}

void TestAarch64StringMemStubsAndMergedDir() {
    auto tmp_ro = std::filesystem::temp_directory_path() / "rr3_test_merge_ro";
    auto tmp_ud = std::filesystem::temp_directory_path() / "rr3_test_merge_ud";
    std::filesystem::remove_all(tmp_ro);
    std::filesystem::remove_all(tmp_ud);
    std::filesystem::create_directories(tmp_ro / ".depot/Tracks/data");
    std::filesystem::create_directories(tmp_ud / ".depot/Tracks/data"); // empty overlay shadowing readonly!
    { std::ofstream f(tmp_ro / ".depot/Tracks/data/brands_hatch.dat"); f << "track"; }

    LayeredVfs vfs;
    vfs.Initialize(tmp_ro, tmp_ud);
    auto dirs = vfs.ResolveReadDirs("/storage/rr3/.depot/Tracks/data");
    CHECK(dirs.size() == 2);
    // Ensure non-existent dir lookup does NOT create overlay directories
    auto nodirs = vfs.ResolveReadDirs("/storage/rr3/.depot/NonExistentDir");
    CHECK(nodirs.empty());
    CHECK(!std::filesystem::exists(tmp_ud / ".depot/NonExistentDir"));
    // Ensure /storage/rr3_cache maps into userdata/cache/ and '..' traversal writes are blocked
    auto cache_p = vfs.ResolveWrite("/storage/rr3_cache/test.tmp");
    CHECK(!cache_p.empty() && cache_p.wstring().find(L"\\cache\\test.tmp") != std::wstring::npos);
    CHECK(vfs.ResolveWrite("/storage/rr3/../../escape.txt").empty());
    CHECK(vfs.ResolveWrite(R"(d:\Real Racing 3\RR3 14.0.1.apk)").empty());

    CpuRuntime cpu;
    ElfLoader loader(cpu);
    BionicRuntime bionic(cpu, loader, vfs);
    bionic.RegisterAll();

    // Verify posix_memalign (>16B) + free/realloc safety and 64-bit -1LL on offline send()
    uint64_t fn_pmem = loader.FindHostSymbol("posix_memalign");
    uint64_t fn_free = loader.FindHostSymbol("free");
    uint64_t fn_send = loader.FindHostSymbol("send");
    CHECK(fn_pmem && fn_free && fn_send);
    void* aligned_ptr = nullptr;
    CHECK(cpu.CallGuest(fn_pmem, {reinterpret_cast<uint64_t>(&aligned_ptr), 64, 256}) == 0);
    CHECK(aligned_ptr != nullptr && (reinterpret_cast<uintptr_t>(aligned_ptr) % 64) == 0);
    cpu.CallGuest(fn_free, {reinterpret_cast<uint64_t>(aligned_ptr)});
    int64_t send_rc = static_cast<int64_t>(cpu.CallGuest(fn_send, {3, 0, 16, 0}));
    CHECK(send_rc == -1LL);

    uint64_t fn_strlen = loader.FindHostSymbol("strlen");
    uint64_t fn_memcmp = loader.FindHostSymbol("memcmp");
    uint64_t fn_strcmp = loader.FindHostSymbol("strcmp");
    uint64_t fn_strncmp = loader.FindHostSymbol("strncmp");
    uint64_t fn_memcpy = loader.FindHostSymbol("memcpy");
    uint64_t fn_memset = loader.FindHostSymbol("memset");
    uint64_t fn_memmove = loader.FindHostSymbol("memmove");
    CHECK(fn_strlen && fn_memcmp && fn_strcmp && fn_strncmp && fn_memcpy && fn_memset && fn_memmove);

    // Exhaustively test strlen across all alignments (0..7) and lengths (0..40)
    alignas(16) char sbuf[80]{};
    for (int off = 0; off < 8; ++off) {
        for (int len = 0; len <= 40; ++len) {
            std::memset(sbuf, 0xCC, sizeof(sbuf));
            for (int i = 0; i < len; ++i) sbuf[off + i] = static_cast<char>('A' + (i % 26));
            sbuf[off + len] = '\0';
            uint64_t got = cpu.CallGuest(fn_strlen, {reinterpret_cast<uint64_t>(sbuf + off)});
            CHECK(got == static_cast<uint64_t>(len));
        }
    }

    // Test memcmp, strcmp, strncmp including unsigned high-bit bytes (0x80 vs 0x7F)
    const char* s1 = "LTS_2018_MAZDA_RT24_P_REWARD";
    const char* s2 = "LTS_1150_MORGAN_PLUS_SIX_REWARD";
    int r_eq = static_cast<int>(cpu.CallGuest(fn_memcmp, {reinterpret_cast<uint64_t>(s1), reinterpret_cast<uint64_t>(s1), 28}));
    int r_gt = static_cast<int>(cpu.CallGuest(fn_memcmp, {reinterpret_cast<uint64_t>(s1), reinterpret_cast<uint64_t>(s2), 28}));
    int r_lt = static_cast<int>(cpu.CallGuest(fn_memcmp, {reinterpret_cast<uint64_t>(s2), reinterpret_cast<uint64_t>(s1), 28}));
    CHECK(r_eq == 0 && r_gt > 0 && r_lt < 0);
    unsigned char hi[4] = {0x10, 0x80, 0x00, 0x00};
    unsigned char lo[4] = {0x10, 0x7F, 0x00, 0x00};
    int r_hi = static_cast<int>(cpu.CallGuest(fn_memcmp, {reinterpret_cast<uint64_t>(hi), reinterpret_cast<uint64_t>(lo), 2}));
    CHECK(r_hi > 0);
    CHECK(static_cast<int>(cpu.CallGuest(fn_strcmp, {reinterpret_cast<uint64_t>(s1), reinterpret_cast<uint64_t>(s1)})) == 0);
    CHECK(static_cast<int>(cpu.CallGuest(fn_strcmp, {reinterpret_cast<uint64_t>(s1), reinterpret_cast<uint64_t>(s2)})) > 0);
    CHECK(static_cast<int>(cpu.CallGuest(fn_strncmp, {reinterpret_cast<uint64_t>(s1), reinterpret_cast<uint64_t>(s2), 4})) == 0);
    CHECK(static_cast<int>(cpu.CallGuest(fn_strncmp, {reinterpret_cast<uint64_t>(s1), reinterpret_cast<uint64_t>(s2), 5})) > 0);

    // Test memcpy, memset, memmove (overlapping forward & backward)
    alignas(16) uint8_t dst[64]{}, src[64]{};
    for (int i = 0; i < 64; ++i) src[i] = static_cast<uint8_t>(i + 1);
    uint64_t mr = cpu.CallGuest(fn_memset, {reinterpret_cast<uint64_t>(dst), 0xAB, 37});
    CHECK(mr == reinterpret_cast<uint64_t>(dst));
    for (int i = 0; i < 37; ++i) CHECK(dst[i] == 0xAB);
    CHECK(dst[37] == 0);
    uint64_t cr = cpu.CallGuest(fn_memcpy, {reinterpret_cast<uint64_t>(dst + 1), reinterpret_cast<uint64_t>(src + 3), 39});
    CHECK(cr == reinterpret_cast<uint64_t>(dst + 1));
    CHECK(std::memcmp(dst + 1, src + 3, 39) == 0);
    // Overlapping backward move (dst > src)
    uint8_t ov[64]{};
    for (int i = 0; i < 64; ++i) ov[i] = static_cast<uint8_t>(i);
    cpu.CallGuest(fn_memmove, {reinterpret_cast<uint64_t>(ov + 5), reinterpret_cast<uint64_t>(ov), 33});
    for (int i = 0; i < 33; ++i) CHECK(ov[5 + i] == static_cast<uint8_t>(i));
    // Overlapping forward move (dst < src)
    cpu.CallGuest(fn_memmove, {reinterpret_cast<uint64_t>(ov), reinterpret_cast<uint64_t>(ov + 5), 33});
    for (int i = 0; i < 33; ++i) CHECK(ov[i] == static_cast<uint8_t>(i));

    std::filesystem::remove_all(tmp_ro);
    std::filesystem::remove_all(tmp_ud);
    std::printf("  [PASS] test_aarch64_string_mem_stubs_and_merged_dir\n");
}

void TestVfsGuiTierFallbackAndMkdir() {
    auto tmp_ro = std::filesystem::temp_directory_path() / "rr3_test_gui_ro";
    auto tmp_ud = std::filesystem::temp_directory_path() / "rr3_test_gui_ud";
    std::filesystem::remove_all(tmp_ro);
    std::filesystem::remove_all(tmp_ud);
    std::filesystem::create_directories(tmp_ro / ".depot/gui_assets/2048x1536");
    std::filesystem::create_directories(tmp_ro / ".depot/gui_assets/960x640");
    { std::ofstream f(tmp_ro / ".depot/gui_assets/2048x1536/hud.atlas"); f << "hi"; }
    { std::ofstream f(tmp_ro / ".depot/gui_assets/960x640/exact.atlas"); f << "mid"; }

    LayeredVfs vfs;
    vfs.Initialize(tmp_ro, tmp_ud);
    vfs.SetPreferredGuiTier("2048x1536");

    // Missing 960x640/hud.atlas falls back cleanly to installed 2048x1536/hud.atlas
    auto fb = vfs.ResolveRead("/storage/rr3/.depot/gui_assets/960x640/hud.atlas");
    CHECK(std::filesystem::exists(fb));
    CHECK(fb.generic_string().find("2048x1536/hud.atlas") != std::string::npos);

    // Existing 960x640/exact.atlas resolves directly without fallback
    auto ex = vfs.ResolveRead("/storage/rr3/.depot/gui_assets/960x640/exact.atlas");
    CHECK(std::filesystem::exists(ex));
    CHECK(ex.generic_string().find("960x640/exact.atlas") != std::string::npos);

    // Virtual /storage and /storage/rr3 mkdir succeeds cleanly instead of EACCES
    CpuRuntime cpu;
    ElfLoader loader(cpu);
    BionicRuntime bionic(cpu, loader, vfs);
    bionic.RegisterAll();
    uint64_t fn_mkdir = loader.FindHostSymbol("mkdir");
    CHECK(fn_mkdir != 0);
    const char* s1 = "/storage";
    const char* s2 = "/storage/rr3";
    CHECK(static_cast<int>(cpu.CallGuest(fn_mkdir, {reinterpret_cast<uint64_t>(s1), 0755})) == 0);
    CHECK(static_cast<int>(cpu.CallGuest(fn_mkdir, {reinterpret_cast<uint64_t>(s2), 0755})) == 0);

    std::filesystem::remove_all(tmp_ro);
    std::filesystem::remove_all(tmp_ud);
    std::printf("  [PASS] test_vfs_gui_tier_fallback_and_mkdir\n");
}

void TestOnlineWinsockLoopbackAndOfflineBlock() {
    auto tmp_ro = std::filesystem::temp_directory_path() / "rr3_test_net_ro";
    auto tmp_ud = std::filesystem::temp_directory_path() / "rr3_test_net_ud";
    std::filesystem::remove_all(tmp_ro);
    std::filesystem::remove_all(tmp_ud);
    std::filesystem::create_directories(tmp_ro);

    LayeredVfs vfs;
    vfs.Initialize(tmp_ro, tmp_ud);

    CpuRuntime cpu;
    ElfLoader loader(cpu);
    BionicRuntime bionic(cpu, loader, vfs);
    bionic.RegisterAll();

    uint64_t fn_socket = loader.FindHostSymbol("socket");
    uint64_t fn_bind = loader.FindHostSymbol("bind");
    uint64_t fn_listen = loader.FindHostSymbol("listen");
    uint64_t fn_getsockname = loader.FindHostSymbol("getsockname");
    uint64_t fn_connect = loader.FindHostSymbol("connect");
    uint64_t fn_accept = loader.FindHostSymbol("accept");
    uint64_t fn_send = loader.FindHostSymbol("send");
    uint64_t fn_recv = loader.FindHostSymbol("recv");
    uint64_t fn_close = loader.FindHostSymbol("close");
    uint64_t fn_inet_pton = loader.FindHostSymbol("inet_pton");
    CHECK(fn_socket && fn_bind && fn_listen && fn_connect && fn_accept && fn_send && fn_recv && fn_close);

    // Offline default: socket() MUST fail with -1
    CHECK(static_cast<int64_t>(cpu.CallGuest(fn_socket, {2, 1, 0})) == -1LL);

    // Online mode: real TCP loopback roundtrip over virtual socket FDs (512..1023 so FD_SET/select fit)
    bionic.SetOnlineMode(true);
    int srv = static_cast<int>(cpu.CallGuest(fn_socket, {2, 1, 0})); // AF_INET, SOCK_STREAM
    CHECK(srv >= 512 && srv < 1024);

    // Verify Bionic AArch64 struct addrinfo layout (ai_canonname=+24, ai_addr=+32, ai_next=+40)
    auto fn_getaddrinfo = loader.FindHostSymbol("getaddrinfo");
    auto fn_freeaddrinfo = loader.FindHostSymbol("freeaddrinfo");
    CHECK(fn_getaddrinfo && fn_freeaddrinfo);
    uint64_t ai_res = 0;
    const char* lo_host = "127.0.0.1";
    const char* lo_svc = "8080";
    CHECK(static_cast<int>(cpu.CallGuest(fn_getaddrinfo, {
              reinterpret_cast<uint64_t>(lo_host), reinterpret_cast<uint64_t>(lo_svc),
              0, reinterpret_cast<uint64_t>(&ai_res)})) == 0);
    CHECK(ai_res != 0);
    const auto* ai_bytes = reinterpret_cast<const uint8_t*>(ai_res);
    uint32_t ai_addrlen = 0;
    uint64_t ai_addr_ptr = 0;
    std::memcpy(&ai_addrlen, ai_bytes + 16, 4);
    std::memcpy(&ai_addr_ptr, ai_bytes + 32, 8);
    CHECK(ai_addrlen >= 16 && ai_addr_ptr != 0);
    uint16_t sa_fam = 0;
    std::memcpy(&sa_fam, reinterpret_cast<const void*>(ai_addr_ptr), 2);
    CHECK(sa_fam == 2); // AF_INET
    cpu.CallGuest(fn_freeaddrinfo, {ai_res});

    uint8_t sa[16]{};
    sa[0] = 2; // AF_INET
    const char* lo_ip = "127.0.0.1";
    CHECK(static_cast<int>(cpu.CallGuest(fn_inet_pton, {2, reinterpret_cast<uint64_t>(lo_ip), reinterpret_cast<uint64_t>(sa + 4)})) == 1);
    CHECK(static_cast<int>(cpu.CallGuest(fn_bind, {static_cast<uint64_t>(srv), reinterpret_cast<uint64_t>(sa), 16})) == 0);
    CHECK(static_cast<int>(cpu.CallGuest(fn_listen, {static_cast<uint64_t>(srv), 2})) == 0);

    uint8_t bound_sa[16]{};
    uint32_t bound_len = 16;
    CHECK(static_cast<int>(cpu.CallGuest(fn_getsockname, {static_cast<uint64_t>(srv), reinterpret_cast<uint64_t>(bound_sa), reinterpret_cast<uint64_t>(&bound_len)})) == 0);

    int cli = static_cast<int>(cpu.CallGuest(fn_socket, {2, 1, 0}));
    CHECK(cli >= 512 && cli < 1024 && cli != srv);
    CHECK(static_cast<int>(cpu.CallGuest(fn_connect, {static_cast<uint64_t>(cli), reinterpret_cast<uint64_t>(bound_sa), 16})) == 0);
    int acc = static_cast<int>(cpu.CallGuest(fn_accept, {static_cast<uint64_t>(srv), 0, 0}));
    CHECK(acc >= 512 && acc < 1024);

    const char* msg = "PRR_ONLINE_OK";
    CHECK(static_cast<int64_t>(cpu.CallGuest(fn_send, {static_cast<uint64_t>(cli), reinterpret_cast<uint64_t>(msg), 13, 0})) == 13);
    char rbuf[32]{};
    CHECK(static_cast<int64_t>(cpu.CallGuest(fn_recv, {static_cast<uint64_t>(acc), reinterpret_cast<uint64_t>(rbuf), sizeof(rbuf), 0})) == 13);
    CHECK(std::strcmp(rbuf, "PRR_ONLINE_OK") == 0);

    CHECK(static_cast<int>(cpu.CallGuest(fn_close, {static_cast<uint64_t>(acc)})) == 0);
    CHECK(static_cast<int>(cpu.CallGuest(fn_close, {static_cast<uint64_t>(cli)})) == 0);
    CHECK(static_cast<int>(cpu.CallGuest(fn_close, {static_cast<uint64_t>(srv)})) == 0);

    std::filesystem::remove_all(tmp_ro);
    std::filesystem::remove_all(tmp_ud);
    std::printf("  [PASS] test_online_winsock_loopback_and_offline_block\n");
}

} // namespace

int main() {
    TestVfsReadOnlyIsolation();
    TestAapcs64FormattingAndStat();
    TestPthreadAndEmutls();
    TestAarch64StringMemStubsAndMergedDir();
    TestVfsGuiTierFallbackAndMkdir();
    TestOnlineWinsockLoopbackAndOfflineBlock();
    TestRr3InitArrays();
    std::printf("test_bionic_vfs ALL PASSED\n");
    return 0;
}
