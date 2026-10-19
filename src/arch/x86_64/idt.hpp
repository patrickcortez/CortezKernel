/**
 * @file idt.hpp
 * @brief x86_64 Interrupt Descriptor Table (IDT) and SOLID IInterruptHandler dispatcher.
 */

#pragma once

#include "../../lib/types.hpp"

namespace cortez::arch {

struct CORTEZ_PACKED InterruptFrame {
    u64 r15;
    u64 r14;
    u64 r13;
    u64 r12;
    u64 r11;
    u64 r10;
    u64 r9;
    u64 r8;
    u64 rbp;
    u64 rdi;
    u64 rsi;
    u64 rdx;
    u64 rcx;
    u64 rbx;
    u64 rax;
    u64 vector;
    u64 errorCode;
    u64 rip;
    u64 cs;
    u64 rflags;
    u64 rsp;
    u64 ss;
};

struct CORTEZ_PACKED IdtEntry {
    u16 offsetLow;
    u16 selector;
    u8  ist;
    u8  typeAttributes;
    u16 offsetMid;
    u32 offsetHigh;
    u32 reserved;
};

struct CORTEZ_PACKED IdtDescriptor {
    u16 limit;
    u64 base;
};

/**
 * @brief SOLID interface for hardware interrupt subscribers.
 */
class IInterruptHandler {
public:
    virtual ~IInterruptHandler() = default;
    virtual void onInterrupt(InterruptFrame* frame) noexcept = 0;
};

class IdtManager final {
public:
    static constexpr usize IDT_ENTRIES    = 256;
    static constexpr u8    VEC_TIMER      = 0x20;
    static constexpr u8    VEC_KEYBOARD   = 0x21;
    static constexpr u8    VEC_NVME_MSI   = 0x40;
    static constexpr u8    VEC_XHCI_MSI   = 0x41;
    static constexpr u8    VEC_AHCI_MSI   = 0x42;
    static constexpr u8    VEC_DYN_START  = 0x50;
    static constexpr u8    VEC_DYN_END    = 0xEF;
    static constexpr u8    VEC_SPURIOUS   = 0xFF;

    static IdtManager& instance() noexcept;

    void init() noexcept;
    void registerHandler(u8 vector, IInterruptHandler* handler) noexcept;
    void unregisterHandler(u8 vector) noexcept;
    i32  allocateVector() noexcept;
    void freeVector(u8 vector) noexcept;
    void dispatch(InterruptFrame* frame) noexcept;

private:
    IdtManager() noexcept = default;

    void setGate(u8 vector, virt_addr_t isrAddr, u8 ist, u8 typeAttr) noexcept;

    CORTEZ_ALIGNED(16) IdtEntry m_idt[IDT_ENTRIES]{};
    IInterruptHandler*          m_handlers[IDT_ENTRIES]{};
    bool                        m_vectorUsed[IDT_ENTRIES]{};
};

} // namespace cortez::arch
