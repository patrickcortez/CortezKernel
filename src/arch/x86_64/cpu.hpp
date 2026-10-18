/**
 * @file cpu.hpp
 * @brief x86_64 CPU control registers, MSRs, interrupts, and barrier intrinsics.
 */

#pragma once

#include "../../lib/types.hpp"

namespace cortez::arch {

constexpr u32 MSR_APIC_BASE        = 0x0000001BU;
constexpr u32 MSR_EFER             = 0xC0000080U;
constexpr u32 MSR_PAT              = 0x00000277U;
constexpr u32 MSR_FS_BASE          = 0xC0000100U;
constexpr u32 MSR_GS_BASE          = 0xC0000101U;
constexpr u32 MSR_KERNEL_GS_BASE   = 0xC0000102U;

struct CpuidResult {
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
};

CORTEZ_ALWAYS_INLINE void cli() noexcept {
    asm volatile("cli" ::: "memory");
}

CORTEZ_ALWAYS_INLINE void sti() noexcept {
    asm volatile("sti" ::: "memory");
}

CORTEZ_ALWAYS_INLINE void hlt() noexcept {
    asm volatile("hlt" ::: "memory");
}

CORTEZ_ALWAYS_INLINE void pause() noexcept {
    asm volatile("pause" ::: "memory");
}

CORTEZ_ALWAYS_INLINE void memoryBarrier() noexcept {
    asm volatile("mfence" ::: "memory");
}

CORTEZ_ALWAYS_INLINE u64 readRflags() noexcept {
    u64 rflags = 0;
    asm volatile("pushfq; popq %0" : "=r"(rflags) :: "memory");
    return rflags;
}

CORTEZ_ALWAYS_INLINE void writeRflags(u64 rflags) noexcept {
    asm volatile("pushq %0; popfq" :: "r"(rflags) : "memory", "cc");
}

CORTEZ_ALWAYS_INLINE bool interruptsEnabled() noexcept {
    return (readRflags() & (1ULL << 9)) != 0;
}

CORTEZ_ALWAYS_INLINE u64 disableInterruptsSave() noexcept {
    u64 flags = readRflags();
    cli();
    return flags;
}

CORTEZ_ALWAYS_INLINE void restoreInterrupts(u64 flags) noexcept {
    if (flags & (1ULL << 9)) {
        sti();
    }
}

CORTEZ_ALWAYS_INLINE u64 readCr2() noexcept {
    u64 val = 0;
    asm volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

CORTEZ_ALWAYS_INLINE u64 readCr3() noexcept {
    u64 val = 0;
    asm volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

CORTEZ_ALWAYS_INLINE void writeCr3(u64 val) noexcept {
    asm volatile("mov %0, %%cr3" :: "r"(val) : "memory");
}

CORTEZ_ALWAYS_INLINE void invlpg(virt_addr_t vaddr) noexcept {
    asm volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

CORTEZ_ALWAYS_INLINE u64 readMsr(u32 msr) noexcept {
    u32 low = 0, high = 0;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<u64>(high) << 32) | low;
}

CORTEZ_ALWAYS_INLINE void writeMsr(u32 msr, u64 value) noexcept {
    u32 low  = static_cast<u32>(value & 0xFFFFFFFFULL);
    u32 high = static_cast<u32>(value >> 32);
    asm volatile("wrmsr" :: "c"(msr), "a"(low), "d"(high) : "memory");
}

CORTEZ_ALWAYS_INLINE u64 rdtsc() noexcept {
    u32 low = 0, high = 0;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<u64>(high) << 32) | low;
}

CORTEZ_ALWAYS_INLINE CpuidResult cpuid(u32 leaf, u32 subleaf = 0) noexcept {
    CpuidResult res{};
    asm volatile("cpuid"
                 : "=a"(res.eax), "=b"(res.ebx), "=c"(res.ecx), "=d"(res.edx)
                 : "a"(leaf), "c"(subleaf));
    return res;
}

CORTEZ_NORETURN CORTEZ_ALWAYS_INLINE void haltForever() noexcept {
    cli();
    for (;;) {
        hlt();
    }
}

} // namespace cortez::arch
