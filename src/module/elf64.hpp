/**
 * @file elf64.hpp
 * @brief Standard ELF64 Specification and Data Structures for Cortez Kernel.
 */

#pragma once

#include "../lib/types.hpp"

namespace cortez::elf {

using Elf64_Addr   = u64;
using Elf64_Off    = u64;
using Elf64_Half   = u16;
using Elf64_Word   = u32;
using Elf64_Sword  = i32;
using Elf64_Xword  = u64;
using Elf64_Sxword = i64;

constexpr usize EI_NIDENT = 16;

// e_ident indices
constexpr usize EI_MAG0       = 0;
constexpr usize EI_MAG1       = 1;
constexpr usize EI_MAG2       = 2;
constexpr usize EI_MAG3       = 3;
constexpr usize EI_CLASS      = 4;
constexpr usize EI_DATA       = 5;
constexpr usize EI_VERSION    = 6;
constexpr usize EI_OSABI      = 7;
constexpr usize EI_ABIVERSION = 8;

// Identification constants
constexpr u8 ELFMAG0        = 0x7f;
constexpr u8 ELFMAG1        = 'E';
constexpr u8 ELFMAG2        = 'L';
constexpr u8 ELFMAG3        = 'F';
constexpr u8 ELFCLASSNONE   = 0;
constexpr u8 ELFCLASS32     = 1;
constexpr u8 ELFCLASS64     = 2;
constexpr u8 ELFDATANONE    = 0;
constexpr u8 ELFDATA2LSB    = 1; // 2's complement, little endian
constexpr u8 ELFDATA2MSB    = 2; // 2's complement, big endian
constexpr u8 EV_CURRENT     = 1;
constexpr u8 ELFOSABI_SYSV  = 0;

// Object file types (e_type)
constexpr Elf64_Half ET_NONE = 0;
constexpr Elf64_Half ET_REL  = 1; // Relocatable file
constexpr Elf64_Half ET_EXEC = 2; // Executable file
constexpr Elf64_Half ET_DYN  = 3; // Shared object / PIE
constexpr Elf64_Half ET_CORE = 4;

// Machine architectures (e_machine)
constexpr Elf64_Half EM_NONE   = 0;
constexpr Elf64_Half EM_X86_64 = 62; // AMD x86-64

// Special Section Indices
constexpr Elf64_Half SHN_UNDEF  = 0;
constexpr Elf64_Half SHN_ABS    = 0xfff1;
constexpr Elf64_Half SHN_COMMON = 0xfff2;

// Section types (sh_type)
constexpr Elf64_Word SHT_NULL     = 0;
constexpr Elf64_Word SHT_PROGBITS = 1;
constexpr Elf64_Word SHT_SYMTAB   = 2;
constexpr Elf64_Word SHT_STRTAB   = 3;
constexpr Elf64_Word SHT_RELA     = 4;
constexpr Elf64_Word SHT_HASH     = 5;
constexpr Elf64_Word SHT_DYNAMIC  = 6;
constexpr Elf64_Word SHT_NOTE     = 7;
constexpr Elf64_Word SHT_NOBITS   = 8;
constexpr Elf64_Word SHT_REL      = 9;
constexpr Elf64_Word SHT_SHLIB    = 10;
constexpr Elf64_Word SHT_DYNSYM   = 11;

// Section flags (sh_flags)
constexpr Elf64_Xword SHF_WRITE            = 0x1;
constexpr Elf64_Xword SHF_ALLOC            = 0x2;
constexpr Elf64_Xword SHF_EXECINSTR        = 0x4;
constexpr Elf64_Xword SHF_MERGE            = 0x10;
constexpr Elf64_Xword SHF_STRINGS          = 0x20;
constexpr Elf64_Xword SHF_INFO_LINK        = 0x40;
constexpr Elf64_Xword SHF_LINK_ORDER       = 0x80;
constexpr Elf64_Xword SHF_OS_NONCONFORMING = 0x100;
constexpr Elf64_Xword SHF_GROUP            = 0x200;
constexpr Elf64_Xword SHF_TLS              = 0x400;

// Symbol Bindings (st_info >> 4)
constexpr u8 STB_LOCAL  = 0;
constexpr u8 STB_GLOBAL = 1;
constexpr u8 STB_WEAK   = 2;

// Symbol Types (st_info & 0xf)
constexpr u8 STT_NOTYPE  = 0;
constexpr u8 STT_OBJECT  = 1;
constexpr u8 STT_FUNC    = 2;
constexpr u8 STT_SECTION = 3;
constexpr u8 STT_FILE    = 4;
constexpr u8 STT_COMMON  = 5;
constexpr u8 STT_TLS     = 6;

#define ELF64_ST_BIND(val)        (((unsigned char)(val)) >> 4)
#define ELF64_ST_TYPE(val)        ((val) & 0xf)
#define ELF64_ST_INFO(bind, type) (((bind) << 4) + ((type) & 0xf))

// x86_64 Relocation Types (r_info & 0xffffffff)
constexpr Elf64_Word R_X86_64_NONE      = 0;
constexpr Elf64_Word R_X86_64_64        = 1;  // Direct 64 bit (S + A)
constexpr Elf64_Word R_X86_64_PC32      = 2;  // PC relative 32 bit signed (S + A - P)
constexpr Elf64_Word R_X86_64_GOT32     = 3;  // 32 bit GOT entry
constexpr Elf64_Word R_X86_64_PLT32     = 4;  // 32 bit PLT address (L + A - P)
constexpr Elf64_Word R_X86_64_COPY      = 5;
constexpr Elf64_Word R_X86_64_GLOB_DAT  = 6;
constexpr Elf64_Word R_X86_64_JUMP_SLOT = 7;
constexpr Elf64_Word R_X86_64_RELATIVE  = 8;
constexpr Elf64_Word R_X86_64_GOTPCREL  = 9;
constexpr Elf64_Word R_X86_64_32        = 10; // Direct 32 bit zero extended
constexpr Elf64_Word R_X86_64_32S       = 11; // Direct 32 bit sign extended
constexpr Elf64_Word R_X86_64_16        = 12;
constexpr Elf64_Word R_X86_64_PC16      = 13;
constexpr Elf64_Word R_X86_64_8         = 14;
constexpr Elf64_Word R_X86_64_PC8       = 15;

#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xffffffffL)
#define ELF64_R_INFO(s,t) (((cortez::elf::Elf64_Xword)(s) << 32) + ((cortez::elf::Elf64_Xword)(t) & 0xffffffffULL))

// ELF Header (64 bytes)
struct CORTEZ_PACKED Elf64_Ehdr {
    u8         e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff;
    Elf64_Off  e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};
static_assert(sizeof(Elf64_Ehdr) == 64, "Elf64_Ehdr must be exactly 64 bytes");

// Section Header (64 bytes)
struct CORTEZ_PACKED Elf64_Shdr {
    Elf64_Word  sh_name;
    Elf64_Word  sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link;
    Elf64_Word  sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
};
static_assert(sizeof(Elf64_Shdr) == 64, "Elf64_Shdr must be exactly 64 bytes");

// Symbol Table Entry (24 bytes)
struct CORTEZ_PACKED Elf64_Sym {
    Elf64_Word  st_name;
    u8          st_info;
    u8          st_other;
    Elf64_Half  st_shndx;
    Elf64_Addr  st_value;
    Elf64_Xword st_size;
};
static_assert(sizeof(Elf64_Sym) == 24, "Elf64_Sym must be exactly 24 bytes");

// Relocation Entry with Addend (24 bytes)
struct CORTEZ_PACKED Elf64_Rela {
    Elf64_Addr   r_offset;
    Elf64_Xword  r_info;
    Elf64_Sxword r_addend;
};
static_assert(sizeof(Elf64_Rela) == 24, "Elf64_Rela must be exactly 24 bytes");

// Program Header (56 bytes)
struct CORTEZ_PACKED Elf64_Phdr {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};
static_assert(sizeof(Elf64_Phdr) == 56, "Elf64_Phdr must be exactly 56 bytes");

} // namespace cortez::elf
