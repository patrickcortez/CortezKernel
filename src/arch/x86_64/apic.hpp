/**
 * @file apic.hpp
 * @brief Local APIC (xAPIC & x2APIC), I/O APIC, and 1000 Hz System Timer controller.
 */

#pragma once

#include "idt.hpp"
#include "../../memory/i_memory.hpp"
#include "../../lib/result.hpp"

namespace cortez::arch {

class ApicController final : public IInterruptHandler {
public:
    static ApicController& instance() noexcept;

    Status init(memory::IVirtualMapper* vmm) noexcept;
    void   routeIsaIrq(u8 isaIrq, u8 vector, bool masked = false) noexcept;
    void   sendEoi() noexcept;

    void   onInterrupt(InterruptFrame* frame) noexcept override;

    u64    getUptimeMs() const noexcept { return m_ticksMs; }
    u32    getApicId() const noexcept { return m_apicId; }
    bool   isX2Apic() const noexcept { return m_x2ApicMode; }
    void   sleepMs(u64 milliseconds) const noexcept;

private:
    ApicController() noexcept = default;

    void disableLegacyPic() noexcept;
    u32  readLapic(u32 regOffset) const noexcept;
    void writeLapic(u32 regOffset, u32 value) const noexcept;
    u32  readIoApic(u8 reg) const noexcept;
    void writeIoApic(u8 reg, u32 value) noexcept;
    void calibrateAndStartTimer() noexcept;

    volatile u32* m_lapicMmio  = nullptr;
    volatile u32* m_ioApicMmio = nullptr;
    bool          m_x2ApicMode = false;
    u32           m_apicId     = 0;
    volatile u64  m_ticksMs    = 0;
    u64           m_tscPerMs   = 2500000ULL;
};

} // namespace cortez::arch
