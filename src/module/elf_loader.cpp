/**
 * @file elf_loader.cpp
 * @brief Implementation of in-kernel freestanding ELF64 Relocatable Object & Binary Loader.
 */

#include "elf_loader.hpp"
#include "kapi.hpp"
#include "../memory/module_area.hpp"
#include "../klibc/klibc.hpp"
#include "../lib/string_utils.hpp"
#include "../lib/printk.hpp"

namespace cortez::module {

bool ElfLoader::isElf(const void* data, usize len) noexcept {
    if (!data || len < 4) return false;
    const auto* bytes = static_cast<const u8*>(data);
    return bytes[0] == elf::ELFMAG0 &&
           bytes[1] == elf::ELFMAG1 &&
           bytes[2] == elf::ELFMAG2 &&
           bytes[3] == elf::ELFMAG3;
}

Result<ElfLoadedImage> ElfLoader::load(const void* elfData, usize elfSize, const char* nameHint) noexcept {
    if (!isElf(elfData, elfSize) || elfSize < sizeof(elf::Elf64_Ehdr)) {
        return Status::InvalidArgument;
    }

    const auto* ehdr = static_cast<const elf::Elf64_Ehdr*>(elfData);

    if (ehdr->e_ident[elf::EI_CLASS] != elf::ELFCLASS64 ||
        ehdr->e_ident[elf::EI_DATA] != elf::ELFDATA2LSB) {
        pr_err("elf", "unsupported ELF class or endianness");
        return Status::InvalidArgument;
    }

    if (ehdr->e_machine != elf::EM_X86_64) {
        pr_err("elf", "unsupported machine architecture (expected x86_64)");
        return Status::InvalidArgument;
    }

    if (ehdr->e_type != elf::ET_REL) {
        pr_err("elf", "unsupported ELF type (expected ET_REL)");
        return Status::InvalidArgument;
    }

    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        pr_err("elf", "missing section headers");
        return Status::InvalidArgument;
    }

    if (ehdr->e_shoff + ehdr->e_shnum * sizeof(elf::Elf64_Shdr) > elfSize) {
        pr_err("elf", "corrupted ELF section header table");
        return Status::InvalidArgument;
    }

    const auto* shdrs = reinterpret_cast<const elf::Elf64_Shdr*>(
        static_cast<const u8*>(elfData) + ehdr->e_shoff);

    auto* secOffsets = static_cast<usize*>(klibc_malloc(ehdr->e_shnum * sizeof(usize)));
    if (!secOffsets) return Status::OutOfMemory;
    memset(secOffsets, 0, ehdr->e_shnum * sizeof(usize));

    usize symtabIdx = 0;
    usize strtabIdx = 0;
    usize currOffset = 0;

    for (usize i = 0; i < ehdr->e_shnum; ++i) {
        const auto& sh = shdrs[i];
        if (sh.sh_type == elf::SHT_SYMTAB) {
            symtabIdx = i;
            strtabIdx = sh.sh_link;
        }
        if (sh.sh_flags & elf::SHF_ALLOC) {
            usize align = (sh.sh_addralign > 0) ? sh.sh_addralign : 16;
            currOffset = static_cast<usize>(alignUp(currOffset, align));
            secOffsets[i] = currOffset;
            currOffset += sh.sh_size;
        }
    }

    if (currOffset == 0) {
        klibc_free(secOffsets);
        return Status::InvalidArgument;
    }

    usize totalBytes = static_cast<usize>(alignUp(currOffset, 4096));
    void* codeBase = memory::ModuleArea::instance().allocate(totalBytes);
    if (!codeBase) {
        klibc_free(secOffsets);
        return Status::OutOfMemory;
    }
    memset(codeBase, 0, totalBytes);

    for (usize i = 0; i < ehdr->e_shnum; ++i) {
        const auto& sh = shdrs[i];
        if ((sh.sh_flags & elf::SHF_ALLOC) && sh.sh_size > 0) {
            u8* dst = static_cast<u8*>(codeBase) + secOffsets[i];
            if (sh.sh_type != elf::SHT_NOBITS) {
                if (sh.sh_offset + sh.sh_size <= elfSize) {
                    memcpy(dst, static_cast<const u8*>(elfData) + sh.sh_offset, sh.sh_size);
                }
            } else {
                memset(dst, 0, sh.sh_size);
            }
        }
    }

    ElfLoadedImage img{};
    img.codeBase   = codeBase;
    img.totalBytes = totalBytes;
    if (nameHint) {
        str::copy(img.name, nameHint, sizeof(img.name));
    }

    virt_addr_t* resolvedSyms = nullptr;
    usize symCount = 0;

    if (symtabIdx > 0 && strtabIdx > 0 && strtabIdx < ehdr->e_shnum) {
        const auto& symSh = shdrs[symtabIdx];
        const auto& strSh = shdrs[strtabIdx];

        if (symSh.sh_offset + symSh.sh_size <= elfSize &&
            strSh.sh_offset + strSh.sh_size <= elfSize) {
            symCount = symSh.sh_size / sizeof(elf::Elf64_Sym);
            const auto* syms = reinterpret_cast<const elf::Elf64_Sym*>(
                static_cast<const u8*>(elfData) + symSh.sh_offset);
            const char* strtab = reinterpret_cast<const char*>(
                static_cast<const u8*>(elfData) + strSh.sh_offset);

            resolvedSyms = static_cast<virt_addr_t*>(klibc_malloc(symCount * sizeof(virt_addr_t)));
            if (!resolvedSyms) {
                memory::ModuleArea::instance().free(codeBase, totalBytes);
                klibc_free(secOffsets);
                return Status::OutOfMemory;
            }
            memset(resolvedSyms, 0, symCount * sizeof(virt_addr_t));

            for (usize si = 0; si < symCount; ++si) {
                const auto& sym = syms[si];
                const char* symName = (sym.st_name < strSh.sh_size) ? (strtab + sym.st_name) : "";

                if (sym.st_shndx == elf::SHN_UNDEF) {
                    if (symName[0]) {
                        const void* addr = lookupKernelSymbol(symName);
                        if (!addr) {
                            pr_err("elf", "%s: undefined external symbol '%s'",
                                   nameHint ? nameHint : "binary", symName);
                            klibc_free(resolvedSyms);
                            klibc_free(secOffsets);
                            memory::ModuleArea::instance().free(codeBase, totalBytes);
                            return Status::NotFound;
                        }
                        resolvedSyms[si] = reinterpret_cast<virt_addr_t>(addr);
                    }
                } else if (sym.st_shndx < ehdr->e_shnum) {
                    resolvedSyms[si] = reinterpret_cast<virt_addr_t>(codeBase) +
                                       secOffsets[sym.st_shndx] + sym.st_value;
                }

                if (symName[0]) {
                    if (str::compare(symName, "main") == 0) {
                        img.mainAddr = resolvedSyms[si];
                    } else if (str::compare(symName, "module_init") == 0) {
                        img.initAddr = resolvedSyms[si];
                    } else if (str::compare(symName, "module_exit") == 0) {
                        img.exitAddr = resolvedSyms[si];
                    }
                }
            }
        }
    }

    for (usize i = 0; i < ehdr->e_shnum; ++i) {
        const auto& sh = shdrs[i];
        if (sh.sh_type == elf::SHT_RELA) {
            usize targetSec = sh.sh_info;
            if (targetSec >= ehdr->e_shnum || !(shdrs[targetSec].sh_flags & elf::SHF_ALLOC)) {
                continue;
            }
            if (sh.sh_offset + sh.sh_size > elfSize) continue;

            u8* targetSecBase = static_cast<u8*>(codeBase) + secOffsets[targetSec];
            usize relaCount = sh.sh_size / sizeof(elf::Elf64_Rela);
            const auto* relas = reinterpret_cast<const elf::Elf64_Rela*>(
                static_cast<const u8*>(elfData) + sh.sh_offset);

            for (usize ri = 0; ri < relaCount; ++ri) {
                const auto& rela = relas[ri];
                u32 symIdx  = static_cast<u32>(ELF64_R_SYM(rela.r_info));
                u32 relType = static_cast<u32>(ELF64_R_TYPE(rela.r_info));

                u8* patchAddr = targetSecBase + rela.r_offset;
                virt_addr_t symAddr = (resolvedSyms && symIdx < symCount) ? resolvedSyms[symIdx] : 0;

                switch (relType) {
                    case elf::R_X86_64_NONE:
                        break;
                    case elf::R_X86_64_64: {
                        u64 val = static_cast<u64>(symAddr + rela.r_addend);
                        memcpy(patchAddr, &val, 8);
                        break;
                    }
                    case elf::R_X86_64_32: {
                        u32 val = static_cast<u32>(symAddr + rela.r_addend);
                        memcpy(patchAddr, &val, 4);
                        break;
                    }
                    case elf::R_X86_64_32S: {
                        i32 val = static_cast<i32>(symAddr + rela.r_addend);
                        memcpy(patchAddr, &val, 4);
                        break;
                    }
                    case elf::R_X86_64_PC32:
                    case elf::R_X86_64_PLT32: {
                        i64 disp = static_cast<i64>(symAddr + rela.r_addend - reinterpret_cast<virt_addr_t>(patchAddr));
                        i32 d32 = static_cast<i32>(disp);
                        memcpy(patchAddr, &d32, 4);
                        break;
                    }
                    default:
                        pr_err("elf", "unhandled relocation type %u at offset 0x%llx",
                               relType, rela.r_offset);
                        break;
                }
            }
        }
    }

    if (img.mainAddr != 0) {
        img.entryAddr = img.mainAddr;
    } else if (img.initAddr != 0) {
        img.entryAddr = img.initAddr;
    } else if (ehdr->e_entry != 0) {
        img.entryAddr = reinterpret_cast<virt_addr_t>(codeBase) + ehdr->e_entry;
    }

    if (resolvedSyms) klibc_free(resolvedSyms);
    klibc_free(secOffsets);

    return img;
}

void ElfLoader::unload(const ElfLoadedImage& img) noexcept {
    if (img.codeBase && img.totalBytes > 0) {
        (void)memory::ModuleArea::instance().free(img.codeBase, img.totalBytes);
    }
}

} // namespace cortez::module
