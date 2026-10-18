/**
 * @file idt.cpp
 * @brief Implementation of 64-bit IDT setup, CPU exception handler, and IRQ dispatcher.
 */

#include "idt.hpp"
#include "gdt.hpp"
#include "apic.hpp"
#include "cpu.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

extern "C" const uint64_t g_isrStubTable[256];

extern "C" void cortez_interrupt_dispatch(cortez::arch::InterruptFrame* frame) {
    cortez::arch::IdtManager::instance().dispatch(frame);
}

namespace cortez::arch {

namespace {
constexpr const char* kExceptionNames[32] = {
    "Divide-by-Zero (#DE)",
    "Debug (#DB)",
    "Non-Maskable Interrupt (NMI)",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "Bound Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved",
    "x87 Floating-Point (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD Floating-Point (#XM)",
    "Virtualization (#VE)",
    "Control Protection (#CP)",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection (#HV)",
    "VMM Communication (#VC)",
    "Security Exception (#SX)",
    "Reserved"
};
} // namespace

IdtManager& IdtManager::instance() noexcept {
    static IdtManager s_idt;
    return s_idt;
}

void IdtManager::setGate(u8 vector, virt_addr_t isrAddr, u8 ist, u8 typeAttr) noexcept {
    IdtEntry& entry      = m_idt[vector];
    entry.offsetLow      = static_cast<u16>(isrAddr & 0xFFFFULL);
    entry.selector       = GdtManager::KERNEL_CS;
    entry.ist            = ist & 0x07U;
    entry.typeAttributes = typeAttr;
    entry.offsetMid      = static_cast<u16>((isrAddr >> 16) & 0xFFFFULL);
    entry.offsetHigh     = static_cast<u32>((isrAddr >> 32) & 0xFFFFFFFFULL);
    entry.reserved       = 0;
}

void IdtManager::init() noexcept {
    memset(m_idt, 0, sizeof(m_idt));
    memset(m_handlers, 0, sizeof(m_handlers));

    for (u32 vec = 0; vec < IDT_ENTRIES; ++vec) {
        u8 ist = 0;
        if (vec == 8)  ist = 1; // Double Fault -> IST1
        if (vec == 14) ist = 2; // Page Fault   -> IST2
        setGate(static_cast<u8>(vec), g_isrStubTable[vec], ist, 0x8EU);
    }

    IdtDescriptor idtr {
        .limit = static_cast<u16>(sizeof(m_idt) - 1),
        .base  = reinterpret_cast<u64>(&m_idt[0])
    };

    asm volatile("lidt (%0)" :: "r"(&idtr) : "memory");
    pr_ok("idt", "Loaded 256-entry 64-bit IDT (IST1=#DF, IST2=#PF)");
}

void IdtManager::registerHandler(u8 vector, IInterruptHandler* handler) noexcept {
    m_handlers[vector] = handler;
}

void IdtManager::dispatch(InterruptFrame* frame) noexcept {
    u8 vec = static_cast<u8>(frame->vector & 0xFFULL);

    if (vec < 32) {
        u64 cr2 = readCr2();
        pr_err("PANIC", "CPU Exception %u: %s", vec, kExceptionNames[vec]);
        pr_err("PANIC", "RIP=0x%016llx RSP=0x%016llx ERR=0x%llx CR2=0x%016llx",
               frame->rip, frame->rsp, frame->errorCode, cr2);
        pr_err("PANIC", "RAX=0x%016llx RBX=0x%016llx RCX=0x%016llx RDX=0x%016llx",
               frame->rax, frame->rbx, frame->rcx, frame->rdx);
        haltForever();
    }

    if (m_handlers[vec]) {
        m_handlers[vec]->onInterrupt(frame);
    }

    if (vec != VEC_SPURIOUS) {
        ApicController::instance().sendEoi();
    }
}

} // namespace cortez::arch
