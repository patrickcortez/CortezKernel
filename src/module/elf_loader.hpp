/**
 * @file elf_loader.hpp
 * @brief Freestanding in-kernel ELF64 Relocatable Object & Binary Loader.
 */

#pragma once

#include "elf64.hpp"
#include "../lib/result.hpp"
#include "../lib/types.hpp"

namespace cortez::module {

struct ElfLoadedImage {
    void*       codeBase   = nullptr;
    usize       totalBytes = 0;
    virt_addr_t entryAddr  = 0;
    virt_addr_t mainAddr   = 0;
    virt_addr_t initAddr   = 0;
    virt_addr_t exitAddr   = 0;
    char        name[32]{};
};

class ElfLoader final {
public:
    static bool isElf(const void* data, usize len) noexcept;
    static Result<ElfLoadedImage> load(const void* elfData, usize elfSize, const char* nameHint = nullptr) noexcept;
    static void unload(const ElfLoadedImage& img) noexcept;
};

} // namespace cortez::module
