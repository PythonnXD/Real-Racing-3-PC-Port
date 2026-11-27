#pragma once

#include "cpu_dynarmic.h"
#include "elf_loader.h"
#include "vfs.h"

#include <cstdint>
#include <functional>
#include <string>

// Exact AArch64 Linux/Bionic struct stat (128 bytes).
struct BionicStat64 {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    int64_t  st_atime_nsec;
    int64_t  st_mtime_sec;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime_sec;
    int64_t  st_ctime_nsec;
    uint32_t __unused[2];
};
static_assert(sizeof(BionicStat64) == 128, "AArch64 struct stat must be 128 bytes");

// Exact AArch64 Linux/Bionic struct dirent.
struct BionicDirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

// Optional hook for dlopen("libOpenSLES.so") etc.
using CustomDlResolverFn = std::function<uint64_t(const std::string& lib_name, const std::string& sym_name)>;

class BionicRuntime {
public:
    BionicRuntime(CpuRuntime& cpu, ElfLoader& loader, LayeredVfs& vfs);

    // Registers all Bionic libc, libm, pthread, sem, dlfcn, android_log, and offline socket symbols.
    void RegisterAll();

    // Registers optional no-op/stub symbols for non-core SDKs (ArCore, Anzu, Echo).
    void RegisterSdkStubs();

    // Hook for dlopen/dlsym resolution of virtual HLE libraries (e.g. libOpenSLES.so).
    void SetCustomDlResolver(CustomDlResolverFn resolver) { custom_dl_ = std::move(resolver); }

    // Option to skip menu-battery usleep calls (e.g. 41ms sleep from onViewRenderJNI).
    void SetSkipMenuUsleep(bool skip) { skip_menu_usleep_ = skip; }

    // Set optional range [lo, hi) of libRealRacing3.so where menu usleep lives.
    void SetMenuUsleepRange(uint64_t lo, uint64_t hi) { menu_usleep_lo_ = lo; menu_usleep_hi_ = hi; }

    // Access thread-local Bionic errno slot.
    static int* ThreadErrnoPtr();

private:
    CpuRuntime& cpu_;
    ElfLoader& loader_;
    LayeredVfs& vfs_;
    CustomDlResolverFn custom_dl_;
    bool skip_menu_usleep_ = false;
    uint64_t menu_usleep_lo_ = 0;
    uint64_t menu_usleep_hi_ = 0;
};
