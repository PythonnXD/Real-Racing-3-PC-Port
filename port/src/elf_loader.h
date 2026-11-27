#pragma once

#include "cpu_dynarmic.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct ElfSegmentInfo {
    uint32_t p_type = 0;
    uint32_t p_flags = 0;
    uint64_t p_offset = 0;
    uint64_t p_vaddr = 0;
    uint64_t p_paddr = 0;
    uint64_t p_filesz = 0;
    uint64_t p_memsz = 0;
    uint64_t p_align = 0;
};

struct LoadedModule {
    std::string name;
    std::filesystem::path path;
    std::string sha256_hex;
    uint8_t* base = nullptr;
    uint64_t min_vaddr = 0;
    uint64_t map_size = 0;
    uint64_t load_bias = 0;

    // Dynamic metadata
    uint64_t phdr_guest_addr = 0;
    uint16_t phnum = 0;
    uint16_t phentsize = 0;
    uint64_t init_func = 0;
    std::vector<uint64_t> init_array;
    std::vector<std::string> needed;
    std::vector<ElfSegmentInfo> phdrs;

    // Parsed symbol/relocation tables (retained for cross-module relocation)
    struct RawSym {
        std::string name;
        uint64_t value = 0;
        uint64_t size = 0;
        uint8_t info = 0;
        uint16_t shndx = 0;
    };
    struct RawRela {
        uint64_t offset = 0;
        uint32_t type = 0;
        uint32_t sym_idx = 0;
        int64_t addend = 0;
    };
    std::vector<RawSym> dynsyms;
    std::vector<RawRela> rela_dyn;
    std::vector<RawRela> rela_plt;
    std::unordered_map<std::string, uint64_t> exports;
};

class ElfLoader {
public:
    explicit ElfLoader(CpuRuntime& cpu);
    ~ElfLoader();

    // Register a host-implemented function symbol (creates an SVC trampoline stub).
    uint64_t RegisterHostFunction(const std::string& name, SvcHandlerFn handler);

    // Register a pure AArch64 guest-executable code address directly under a symbol name (zero SVC overhead).
    void RegisterHostAddress(const std::string& name, uint64_t guest_addr);

    // Register a host-provided data object address (e.g. __sF, stderr, stdout, _ctype_).
    void RegisterHostData(const std::string& name, void* host_ptr);

    // Maps an AArch64 ELF64 shared library into flat guest memory without running constructors.
    LoadedModule* LoadModule(const std::filesystem::path& path);

    // Resolves all RELATIVE / ABS64 / GLOB_DAT / JUMP_SLOT relocations across loaded modules.
    // Returns number of unresolved symbols (each gets a diagnostic trap stub).
    size_t ApplyRelocations(std::vector<std::string>* out_unresolved = nullptr);

    // Runs DT_INIT and DT_INIT_ARRAY for every loaded module (in load order).
    void RunInitArrays();

    // Lookup exported guest symbol or registered host symbol (for dlsym / entry calls).
    uint64_t FindExport(const std::string& name) const;
    uint64_t FindHostSymbol(const std::string& name) const;
    const LoadedModule* FindModuleByAddress(uint64_t addr) const;
    const std::deque<LoadedModule>& Modules() const { return modules_; }

    // Computes SHA-256 hex digest of a file using Windows BCrypt.
    static std::string ComputeFileSha256(const std::filesystem::path& path);

private:
    uint64_t ResolveSymbol(const std::string& name, bool is_weak, std::vector<std::string>* unresolved);

    CpuRuntime& cpu_;
    std::deque<LoadedModule> modules_;
    std::unordered_map<std::string, uint64_t> host_symbols_;
    std::unordered_map<std::string, uint64_t> unresolved_stubs_;
};
