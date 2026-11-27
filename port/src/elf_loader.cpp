#include "elf_loader.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {

#pragma pack(push, 1)
struct Elf64Ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct Elf64Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

struct Elf64Sym {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};

struct Elf64Rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
};

struct Elf64Dyn {
    int64_t d_tag;
    uint64_t d_val;
};
#pragma pack(pop)

constexpr uint16_t EM_AARCH64 = 183;
constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PT_PHDR = 6;
constexpr int64_t DT_NULL = 0;
constexpr int64_t DT_NEEDED = 1;
constexpr int64_t DT_INIT = 12;
constexpr int64_t DT_SONAME = 14;
constexpr int64_t DT_INIT_ARRAY = 25;
constexpr int64_t DT_INIT_ARRAYSZ = 27;

constexpr uint32_t R_AARCH64_ABS64 = 257;
constexpr uint32_t R_AARCH64_GLOB_DAT = 1025;
constexpr uint32_t R_AARCH64_JUMP_SLOT = 1026;
constexpr uint32_t R_AARCH64_RELATIVE = 1027;

constexpr uint8_t STB_WEAK = 2;

uint64_t AlignDown(uint64_t v, uint64_t a) { return v & ~(a - 1); }
uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

} // namespace

ElfLoader::ElfLoader(CpuRuntime& cpu) : cpu_(cpu) {}

ElfLoader::~ElfLoader() {
    for (auto& m : modules_) {
        if (m.base) {
            VirtualFree(m.base, 0, MEM_RELEASE);
            m.base = nullptr;
        }
    }
}

std::string ElfLoader::ComputeFileSha256(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    std::vector<uint8_t> buf(64 * 1024);
    while (in.good()) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        auto got = in.gcount();
        if (got > 0) {
            BCryptHashData(hash, buf.data(), static_cast<ULONG>(got), 0);
        }
    }

    uint8_t digest[32]{};
    BCryptFinishHash(hash, digest, sizeof(digest), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : digest) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

uint64_t ElfLoader::RegisterHostFunction(const std::string& name, SvcHandlerFn handler) {
    uint64_t stub = cpu_.AllocateSvcStub(name, std::move(handler));
    host_symbols_[name] = stub;
    return stub;
}

void ElfLoader::RegisterHostAddress(const std::string& name, uint64_t guest_addr) {
    host_symbols_[name] = guest_addr;
}

void ElfLoader::RegisterHostData(const std::string& name, void* host_ptr) {
    host_symbols_[name] = reinterpret_cast<uint64_t>(host_ptr);
}

LoadedModule* ElfLoader::LoadModule(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        std::fprintf(stderr, "[ELF] Failed to open %s\n", path.string().c_str());
        return nullptr;
    }
    const auto file_size = static_cast<size_t>(in.tellg());
    in.seekg(0);
    std::vector<uint8_t> file_data(file_size);
    in.read(reinterpret_cast<char*>(file_data.data()), static_cast<std::streamsize>(file_size));
    in.close();

    if (file_size < sizeof(Elf64Ehdr)) return nullptr;
    Elf64Ehdr ehdr{};
    std::memcpy(&ehdr, file_data.data(), sizeof(ehdr));
    if (std::memcmp(ehdr.e_ident, "\x7f""ELF", 4) != 0 || ehdr.e_ident[4] != 2 || ehdr.e_machine != EM_AARCH64) {
        std::fprintf(stderr, "[ELF] Not an AArch64 ELF64: %s\n", path.string().c_str());
        return nullptr;
    }

    LoadedModule mod;
    mod.path = path;
    mod.name = path.filename().string();
    mod.sha256_hex = ComputeFileSha256(path);
    mod.phnum = ehdr.e_phnum;
    mod.phentsize = ehdr.e_phentsize;

    uint64_t min_va = UINT64_MAX;
    uint64_t max_va = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
        Elf64Phdr ph{};
        std::memcpy(&ph, file_data.data() + ehdr.e_phoff + static_cast<size_t>(i) * ehdr.e_phentsize, sizeof(ph));
        ElfSegmentInfo seg{};
        seg.p_type = ph.p_type;
        seg.p_flags = ph.p_flags;
        seg.p_offset = ph.p_offset;
        seg.p_vaddr = ph.p_vaddr;
        seg.p_paddr = ph.p_paddr;
        seg.p_filesz = ph.p_filesz;
        seg.p_memsz = ph.p_memsz;
        seg.p_align = ph.p_align;
        mod.phdrs.push_back(seg);
        if (ph.p_type == PT_LOAD) {
            min_va = std::min(min_va, AlignDown(ph.p_vaddr, 0x1000));
            max_va = std::max(max_va, AlignUp(ph.p_vaddr + ph.p_memsz, 0x1000));
        }
    }

    if (min_va >= max_va) return nullptr;
    mod.min_vaddr = min_va;
    mod.map_size = AlignUp(max_va - min_va, 0x10000);
    mod.base = static_cast<uint8_t*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(mod.map_size), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!mod.base) {
        std::fprintf(stderr, "[ELF] VirtualAlloc failed (%llu bytes) for %s\n",
                     static_cast<unsigned long long>(mod.map_size), mod.name.c_str());
        return nullptr;
    }
    mod.load_bias = reinterpret_cast<uint64_t>(mod.base) - min_va;

    for (const auto& ph : mod.phdrs) {
        if (ph.p_type == PT_LOAD && ph.p_filesz > 0) {
            std::memcpy(reinterpret_cast<void*>(mod.load_bias + ph.p_vaddr),
                        file_data.data() + ph.p_offset,
                        static_cast<size_t>(ph.p_filesz));
        }
        if (ph.p_type == PT_PHDR) {
            mod.phdr_guest_addr = mod.load_bias + ph.p_vaddr;
        }
    }
    if (mod.phdr_guest_addr == 0) {
        mod.phdr_guest_addr = mod.load_bias + ehdr.e_phoff;
    }

    // Parse section headers for .dynsym, .dynstr, .rela.dyn, .rela.plt, .dynamic
    std::vector<Elf64Shdr> shdrs(ehdr.e_shnum);
    for (uint16_t i = 0; i < ehdr.e_shnum; ++i) {
        std::memcpy(&shdrs[i], file_data.data() + ehdr.e_shoff + static_cast<size_t>(i) * ehdr.e_shentsize, sizeof(Elf64Shdr));
    }
    const auto& shstr_hdr = shdrs[ehdr.e_shstrndx];
    const char* shstr = reinterpret_cast<const char*>(file_data.data() + shstr_hdr.sh_offset);

    auto find_sec = [&](const char* target) -> const Elf64Shdr* {
        for (const auto& s : shdrs) {
            if (std::strcmp(shstr + s.sh_name, target) == 0) return &s;
        }
        return nullptr;
    };

    const Elf64Shdr* sec_dynstr = find_sec(".dynstr");
    const Elf64Shdr* sec_dynsym = find_sec(".dynsym");
    const Elf64Shdr* sec_dynamic = find_sec(".dynamic");
    const Elf64Shdr* sec_rela_dyn = find_sec(".rela.dyn");
    const Elf64Shdr* sec_rela_plt = find_sec(".rela.plt");

    const char* dynstr = sec_dynstr ? reinterpret_cast<const char*>(file_data.data() + sec_dynstr->sh_offset) : "";

    if (sec_dynsym && sec_dynstr) {
        const size_t cnt = static_cast<size_t>(sec_dynsym->sh_size / sizeof(Elf64Sym));
        mod.dynsyms.reserve(cnt);
        for (size_t i = 0; i < cnt; ++i) {
            Elf64Sym s{};
            std::memcpy(&s, file_data.data() + sec_dynsym->sh_offset + i * sizeof(Elf64Sym), sizeof(Elf64Sym));
            LoadedModule::RawSym rs;
            rs.name = dynstr + s.st_name;
            rs.value = s.st_value;
            rs.size = s.st_size;
            rs.info = s.st_info;
            rs.shndx = s.st_shndx;
            if (rs.shndx != 0 && !rs.name.empty()) {
                uint8_t bind = rs.info >> 4;
                auto it = mod.exports.find(rs.name);
                if (it == mod.exports.end() || bind != STB_WEAK) {
                    mod.exports[rs.name] = mod.load_bias + rs.value;
                }
            }
            mod.dynsyms.push_back(std::move(rs));
        }
    }

    auto read_rela = [&](const Elf64Shdr* sec, std::vector<LoadedModule::RawRela>& out) {
        if (!sec) return;
        const size_t cnt = static_cast<size_t>(sec->sh_size / sizeof(Elf64Rela));
        out.reserve(cnt);
        for (size_t i = 0; i < cnt; ++i) {
            Elf64Rela r{};
            std::memcpy(&r, file_data.data() + sec->sh_offset + i * sizeof(Elf64Rela), sizeof(Elf64Rela));
            LoadedModule::RawRela rr;
            rr.offset = r.r_offset;
            rr.type = static_cast<uint32_t>(r.r_info & 0xFFFFFFFFULL);
            rr.sym_idx = static_cast<uint32_t>(r.r_info >> 32);
            rr.addend = r.r_addend;
            out.push_back(rr);
        }
    };
    read_rela(sec_rela_dyn, mod.rela_dyn);
    read_rela(sec_rela_plt, mod.rela_plt);

    uint64_t init_array_va = 0;
    uint64_t init_array_sz = 0;
    if (sec_dynamic && sec_dynstr) {
        const size_t cnt = static_cast<size_t>(sec_dynamic->sh_size / sizeof(Elf64Dyn));
        for (size_t i = 0; i < cnt; ++i) {
            Elf64Dyn d{};
            std::memcpy(&d, file_data.data() + sec_dynamic->sh_offset + i * sizeof(Elf64Dyn), sizeof(Elf64Dyn));
            if (d.d_tag == DT_NULL) break;
            if (d.d_tag == DT_NEEDED) mod.needed.emplace_back(dynstr + d.d_val);
            else if (d.d_tag == DT_SONAME) mod.name = dynstr + d.d_val;
            else if (d.d_tag == DT_INIT && d.d_val != 0) mod.init_func = mod.load_bias + d.d_val;
            else if (d.d_tag == DT_INIT_ARRAY) init_array_va = d.d_val;
            else if (d.d_tag == DT_INIT_ARRAYSZ) init_array_sz = d.d_val;
        }
    }

    // Store raw init_array slot VAs; entries themselves are relocated during ApplyRelocations!
    if (init_array_va != 0 && init_array_sz > 0) {
        const size_t n = static_cast<size_t>(init_array_sz / 8);
        mod.init_array.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            mod.init_array.push_back(mod.load_bias + init_array_va + i * 8);
        }
    }

    modules_.push_back(std::move(mod));
    return &modules_.back();
}

uint64_t ElfLoader::ResolveSymbol(const std::string& name, bool is_weak, std::vector<std::string>* unresolved) {
    // 1. Explicit host override (libc, libm, GLES, EGL, pthread, stubs)
    if (auto it = host_symbols_.find(name); it != host_symbols_.end()) {
        return it->second;
    }
    // 2. Guest module exports (prefer global over weak across loaded modules)
    uint64_t weak_match = 0;
    for (const auto& m : modules_) {
        if (auto it = m.exports.find(name); it != m.exports.end()) {
            return it->second;
        }
    }
    if (weak_match != 0) return weak_match;

    // 3. Weak undefined symbols resolve cleanly to 0 per ELF spec
    if (is_weak) {
        return 0;
    }

    // 4. Unresolved strong symbol -> diagnostic trap stub
    if (auto it = unresolved_stubs_.find(name); it != unresolved_stubs_.end()) {
        return it->second;
    }
    if (unresolved) {
        unresolved->push_back(name);
    }
    std::string sym_copy = name;
    uint64_t stub = cpu_.AllocateSvcStub("UNRESOLVED:" + name, [sym_copy](CpuContext& ctx) {
        std::fprintf(stderr,
                     "[UNRESOLVED SYMBOL CALL] %s (LR=0x%016llx X0=0x%llx X1=0x%llx X2=0x%llx)\n",
                     sym_copy.c_str(),
                     static_cast<unsigned long long>(ctx.GetLR()),
                     static_cast<unsigned long long>(ctx.GetX(0)),
                     static_cast<unsigned long long>(ctx.GetX(1)),
                     static_cast<unsigned long long>(ctx.GetX(2)));
        std::fflush(stderr);
        ctx.SetX(0, 0);
    });
    unresolved_stubs_[name] = stub;
    return stub;
}

size_t ElfLoader::ApplyRelocations(std::vector<std::string>* out_unresolved) {
    std::vector<std::string> local_unresolved;
    auto apply_list = [&](LoadedModule& m, const std::vector<LoadedModule::RawRela>& list) {
        for (const auto& r : list) {
            auto* slot = reinterpret_cast<uint64_t*>(m.load_bias + r.offset);
            switch (r.type) {
                case R_AARCH64_RELATIVE:
                    *slot = static_cast<uint64_t>(static_cast<int64_t>(m.load_bias) + r.addend);
                    break;
                case R_AARCH64_ABS64:
                case R_AARCH64_GLOB_DAT:
                case R_AARCH64_JUMP_SLOT: {
                    const auto& sym = m.dynsyms.at(r.sym_idx);
                    bool is_weak = ((sym.info >> 4) == STB_WEAK);
                    uint64_t target = 0;
                    if (sym.name.empty()) {
                        target = m.load_bias + sym.value;
                    } else {
                        target = ResolveSymbol(sym.name, is_weak, &local_unresolved);
                    }
                    *slot = static_cast<uint64_t>(static_cast<int64_t>(target) + r.addend);
                    break;
                }
                default:
                    std::fprintf(stderr, "[ELF FATAL] Unsupported AArch64 reloc type %u in %s\n",
                                 r.type, m.name.c_str());
                    std::abort();
            }
        }
    };

    for (auto& m : modules_) {
        apply_list(m, m.rela_dyn);
        apply_list(m, m.rela_plt);
    }

    if (out_unresolved) {
        *out_unresolved = local_unresolved;
    }
    return local_unresolved.size();
}

void ElfLoader::RunInitArrays() {
    cpu_.EnsureThreadInit();
    for (const auto& m : modules_) {
        if (m.init_func != 0) {
            cpu_.CallGuest(m.init_func);
        }
        for (uint64_t slot_va : m.init_array) {
            uint64_t fn_ptr = *reinterpret_cast<const uint64_t*>(slot_va);
            if (fn_ptr != 0 && fn_ptr != UINT64_MAX) {
                cpu_.CallGuest(fn_ptr);
            }
        }
    }
}

uint64_t ElfLoader::FindExport(const std::string& name) const {
    for (const auto& m : modules_) {
        if (auto it = m.exports.find(name); it != m.exports.end()) {
            return it->second;
        }
    }
    return FindHostSymbol(name);
}

uint64_t ElfLoader::FindHostSymbol(const std::string& name) const {
    if (auto it = host_symbols_.find(name); it != host_symbols_.end()) {
        return it->second;
    }
    return 0;
}

const LoadedModule* ElfLoader::FindModuleByAddress(uint64_t addr) const {
    for (const auto& m : modules_) {
        uint64_t lo = reinterpret_cast<uint64_t>(m.base);
        if (addr >= lo && addr < lo + m.map_size) {
            return &m;
        }
    }
    return nullptr;
}
