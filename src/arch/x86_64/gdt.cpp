/**
 * @file gdt.cpp
 * @brief Implementation of 64-bit GDT and TSS initialization.
 */

#include "gdt.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

extern "C" void cortez_gdt_load(const cortez::arch::GdtDescriptor* gdtr,
                                uint16_t cs,
                                uint16_t ds,
                                uint16_t tssSel);

namespace cortez::arch {

GdtManager& GdtManager::instance() noexcept {
    static GdtManager s_gdt;
    return s_gdt;
}

void GdtManager::init() noexcept {
    memset(m_gdt, 0, sizeof(m_gdt));
    memset(&m_tss, 0, sizeof(m_tss));

    m_tss.ist1      = reinterpret_cast<u64>(&m_ist1Stack[sizeof(m_ist1Stack)]);
    m_tss.ist2      = reinterpret_cast<u64>(&m_ist2Stack[sizeof(m_ist2Stack)]);
    m_tss.iomapBase = sizeof(TssEntry);

    // 0x00: Null Descriptor
    m_gdt[0] = 0x0000000000000000ULL;
    // 0x08: 64-bit Kernel Code Segment (Present, Ring 0, Executable, Long Mode)
    m_gdt[1] = 0x00AF9A000000FFFFULL;
    // 0x10: 64-bit Kernel Data Segment (Present, Ring 0, Writable)
    m_gdt[2] = 0x00CF92000000FFFFULL;
    // 0x18: 64-bit User Data Segment
    m_gdt[3] = 0x00CFF2000000FFFFULL;
    // 0x20: 64-bit User Code Segment
    m_gdt[4] = 0x00AFFA000000FFFFULL;

    // 0x28..0x30: 16-byte 64-bit Available TSS Descriptor
    u64 tssBase  = reinterpret_cast<u64>(&m_tss);
    u64 tssLimit = sizeof(TssEntry) - 1;

    u64 tssLow = (tssLimit & 0xFFFFULL) |
                 ((tssBase & 0xFFFFFFULL) << 16) |
                 (0x89ULL << 40) |
                 (((tssLimit >> 16) & 0xFULL) << 48) |
                 (((tssBase >> 24) & 0xFFULL) << 56);
    u64 tssHigh = (tssBase >> 32) & 0xFFFFFFFFULL;

    m_gdt[5] = tssLow;
    m_gdt[6] = tssHigh;

    GdtDescriptor gdtr {
        .limit = static_cast<u16>(sizeof(m_gdt) - 1),
        .base  = reinterpret_cast<u64>(&m_gdt[0])
    };

    cortez_gdt_load(&gdtr, KERNEL_CS, KERNEL_DS, TSS_SEL);
    pr_ok("gdt", "Loaded 64-bit GDT & TSS (CS=0x%02x, DS=0x%02x, TSS=0x%02x)",
          KERNEL_CS, KERNEL_DS, TSS_SEL);
}

void GdtManager::setKernelStack(virt_addr_t rsp0) noexcept {
    m_tss.rsp0 = rsp0;
}

} // namespace cortez::arch
