/**
 * @file apic.cpp
 * @brief Implementation of xAPIC/x2APIC, I/O APIC, and Local APIC 1000 Hz timer.
 */

#include "apic.hpp"
#include "acpi.hpp"
#include "cpu.hpp"
#include "io_ports.hpp"
#include "../../lib/printk.hpp"

namespace cortez::arch {

namespace {
constexpr u32 LAPIC_REG_ID          = 0x020;
constexpr u32 LAPIC_REG_EOI         = 0x0B0;
constexpr u32 LAPIC_REG_SVR         = 0x0F0;
constexpr u32 LAPIC_REG_LVT_TIMER   = 0x320;
constexpr u32 LAPIC_REG_TIMER_INIT  = 0x380;
constexpr u32 LAPIC_REG_TIMER_CURR  = 0x390;
constexpr u32 LAPIC_REG_TIMER_DIV   = 0x3E0;

constexpr u32 APIC_BASE_ENABLE_BIT  = (1U << 11);
constexpr u32 APIC_BASE_X2APIC_BIT  = (1U << 10);
} // namespace

ApicController& ApicController::instance() noexcept {
    static ApicController s_apic;
    return s_apic;
}

void ApicController::disableLegacyPic() noexcept {
    // Remap 8259 PIC to vectors 0x20..0x2F so spurious IRQs do not hit CPU exceptions
    outb(0x20, 0x11); ioWait();
    outb(0xA0, 0x11); ioWait();
    outb(0x21, 0x20); ioWait();
    outb(0xA1, 0x28); ioWait();
    outb(0x21, 0x04); ioWait();
    outb(0xA1, 0x02); ioWait();
    outb(0x21, 0x01); ioWait();
    outb(0xA1, 0x01); ioWait();

    // Mask all legacy PIC lines (I/O APIC handles interrupt routing)
    outb(0x21, 0xFF); ioWait();
    outb(0xA1, 0xFF); ioWait();
}

u32 ApicController::readLapic(u32 regOffset) const noexcept {
    if (m_x2ApicMode) {
        u32 msr = 0x800U + (regOffset >> 4);
        return static_cast<u32>(readMsr(msr));
    }
    if (!m_lapicMmio) return 0;
    return m_lapicMmio[regOffset / 4];
}

void ApicController::writeLapic(u32 regOffset, u32 value) const noexcept {
    if (m_x2ApicMode) {
        u32 msr = 0x800U + (regOffset >> 4);
        writeMsr(msr, value);
        return;
    }
    if (m_lapicMmio) {
        m_lapicMmio[regOffset / 4] = value;
    }
}

u32 ApicController::readIoApic(u8 reg) const noexcept {
    if (!m_ioApicMmio) return 0;
    m_ioApicMmio[0] = reg;
    return m_ioApicMmio[4]; // Offset 0x10 / 4
}

void ApicController::writeIoApic(u8 reg, u32 value) noexcept {
    if (!m_ioApicMmio) return;
    m_ioApicMmio[0] = reg;
    m_ioApicMmio[4] = value;
}

Status ApicController::init(memory::IVirtualMapper* vmm) noexcept {
    if (!vmm) return Status::InvalidArgument;

    disableLegacyPic();

    u64 apicBaseMsr = readMsr(MSR_APIC_BASE);
    if (!(apicBaseMsr & APIC_BASE_ENABLE_BIT)) {
        apicBaseMsr |= APIC_BASE_ENABLE_BIT;
        writeMsr(MSR_APIC_BASE, apicBaseMsr);
    }

    m_x2ApicMode = (apicBaseMsr & APIC_BASE_X2APIC_BIT) != 0;

    if (!m_x2ApicMode) {
        phys_addr_t lapicPhys = AcpiManager::instance().getLocalApicPhys();
        auto lapicRes = vmm->mapMmio(lapicPhys, PAGE_SIZE_4K);
        if (lapicRes.isError()) return lapicRes.status();
        m_lapicMmio = reinterpret_cast<volatile u32*>(lapicRes.value());
        m_apicId    = readLapic(LAPIC_REG_ID) >> 24;
    } else {
        m_apicId = readLapic(LAPIC_REG_ID);
    }

    // Enable Local APIC via Spurious Interrupt Vector Register (bit 8 = Software Enable)
    writeLapic(LAPIC_REG_SVR, 0x100U | IdtManager::VEC_SPURIOUS);

    // Map I/O APIC MMIO
    phys_addr_t ioApicPhys = AcpiManager::instance().getIoApicPhys();
    auto ioApicRes = vmm->mapMmio(ioApicPhys, PAGE_SIZE_4K);
    if (ioApicRes.isOk()) {
        m_ioApicMmio = reinterpret_cast<volatile u32*>(ioApicRes.value());
    }

    IdtManager::instance().registerHandler(IdtManager::VEC_TIMER, this);
    calibrateAndStartTimer();

    pr_ok("apic", "Initialized %s (APIC ID=%u, IOAPIC=0x%llx, Timer=1000 Hz)",
          m_x2ApicMode ? "x2APIC (MSR)" : "xAPIC (MMIO)",
          m_apicId, ioApicPhys);

    return Status::Ok;
}

void ApicController::routeIsaIrq(u8 isaIrq, u8 vector, bool masked) noexcept {
    if (!m_ioApicMmio) return;

    u16 isoFlags = 0;
    u32 gsi      = AcpiManager::instance().resolveIrqToGsi(isaIrq, &isoFlags);

    u64 redir = vector;
    // Polarity: bits 0..1 of isoFlags (0b11 = Active Low -> bit 13)
    if ((isoFlags & 0x03U) == 0x03U) {
        redir |= (1ULL << 13);
    }
    // Trigger Mode: bits 2..3 of isoFlags (0b11 = Level Triggered -> bit 15)
    if (((isoFlags >> 2) & 0x03U) == 0x03U) {
        redir |= (1ULL << 15);
    }
    if (masked) {
        redir |= (1ULL << 16);
    }

    // Destination APIC ID in bits 56..63 (low 8 bits of APIC ID for physical destination)
    redir |= (static_cast<u64>(m_apicId & 0xFFU) << 56);

    u8 regLow  = static_cast<u8>(0x10U + gsi * 2U);
    u8 regHigh = static_cast<u8>(0x10U + gsi * 2U + 1U);

    writeIoApic(regHigh, static_cast<u32>(redir >> 32));
    writeIoApic(regLow, static_cast<u32>(redir & 0xFFFFFFFFULL));
}

void ApicController::calibrateAndStartTimer() noexcept {
    // Divide by 16
    writeLapic(LAPIC_REG_TIMER_DIV, 0x03U);

    // Use PIT Channel 2 for a 10ms one-shot reference window (1193182 Hz / 100 = 11932 ticks)
    u8 gate = inb(0x61);
    outb(0x61, static_cast<u8>((gate & 0xFD) | 0x01));
    outb(0x43, 0xB0); // Channel 2, lobyte/hibyte, mode 0
    constexpr u16 pitCount = 11932;
    outb(0x42, static_cast<u8>(pitCount & 0xFF));
    ioWait();
    outb(0x42, static_cast<u8>((pitCount >> 8) & 0xFF));

    // Reset PIT gate
    u8 tmp = inb(0x61) & 0xFE;
    outb(0x61, tmp);
    outb(0x61, static_cast<u8>(tmp | 1));

    u64 tscStart = rdtsc();
    writeLapic(LAPIC_REG_TIMER_INIT, 0xFFFFFFFFU);

    // Poll PIT Channel 2 output bit (bit 5 of port 0x61) with bounded spin guard for laptops without PIT
    for (u32 spin = 0; spin < 2000000; ++spin) {
        if (inb(0x61) & 0x20) {
            break;
        }
    }

    u32 elapsedApicTicks = 0xFFFFFFFFU - readLapic(LAPIC_REG_TIMER_CURR);
    u64 tscElapsed       = rdtsc() - tscStart;

    if (tscElapsed > 100000ULL) {
        m_tscPerMs = tscElapsed / 10ULL;
    }

    u32 ticksPerMs = elapsedApicTicks / 10U;
    if (ticksPerMs < 1000U || ticksPerMs > 0x0FFFFFFFU) {
        ticksPerMs = 250000U; // Safe fallback if laptop gates legacy PIT channel 2
    }

    // Start Periodic Timer (bit 17 = 0x20000) on VEC_TIMER (0x20) at 1000 Hz (1 ms)
    writeLapic(LAPIC_REG_LVT_TIMER, (1U << 17) | IdtManager::VEC_TIMER);
    writeLapic(LAPIC_REG_TIMER_DIV, 0x03U);
    writeLapic(LAPIC_REG_TIMER_INIT, ticksPerMs);
}

void ApicController::sendEoi() noexcept {
    writeLapic(LAPIC_REG_EOI, 0);
}

void ApicController::onInterrupt(InterruptFrame*) noexcept {
    m_ticksMs = m_ticksMs + 1;
}

void ApicController::sleepMs(u64 milliseconds) const noexcept {
    if (milliseconds == 0) return;
    if (interruptsEnabled() && m_ticksMs > 0) {
        u64 target = m_ticksMs + milliseconds;
        while (m_ticksMs < target) {
            hlt();
        }
        return;
    }

    // Interrupt-free TSC delay (used during early driver init when IRQs may be masked)
    u64 startTsc  = rdtsc();
    u64 targetTsc = startTsc + (milliseconds * m_tscPerMs);
    while (rdtsc() < targetTsc) {
        pause();
    }
}

} // namespace cortez::arch
