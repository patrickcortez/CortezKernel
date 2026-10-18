/**
 * @file gdt.hpp
 * @brief x86_64 Global Descriptor Table (GDT) and Task State Segment (TSS) manager.
 */

#pragma once

#include "../../lib/types.hpp"

namespace cortez::arch {

struct CORTEZ_PACKED TssEntry {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iomapBase;
};

struct CORTEZ_PACKED GdtDescriptor {
    u16 limit;
    u64 base;
};

class GdtManager final {
public:
    static constexpr u16 KERNEL_CS = 0x08;
    static constexpr u16 KERNEL_DS = 0x10;
    static constexpr u16 TSS_SEL   = 0x28;

    static GdtManager& instance() noexcept;

    void init() noexcept;
    void setKernelStack(virt_addr_t rsp0) noexcept;

private:
    GdtManager() noexcept = default;

    CORTEZ_ALIGNED(16) u64      m_gdt[7]{};
    CORTEZ_ALIGNED(16) TssEntry m_tss{};
    CORTEZ_ALIGNED(16) u8       m_ist1Stack[16384]{};
    CORTEZ_ALIGNED(16) u8       m_ist2Stack[16384]{};
};

} // namespace cortez::arch
