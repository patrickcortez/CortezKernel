/**
 * @file ahci_controller.hpp
 * @brief AHCI Host Controller driver managing BIOS handoff, port enumeration, and SATA disks.
 */

#pragma once

#include "ahci_port.hpp"
#include "sata_disk.hpp"
#include "../../arch/x86_64/pcie.hpp"
#include "../../arch/x86_64/idt.hpp"

namespace cortez::drivers::storage {

class AhciController final : public arch::IInterruptHandler {
public:
    static constexpr usize MAX_PORTS = 32;
    static constexpr usize MAX_DISKS = 16;

    static AhciController& instance() noexcept;

    Status init(memory::IPhysicalAllocator* pmm,
                memory::IVirtualMapper* vmm,
                u64 hhdmOffset) noexcept;

    void onInterrupt(arch::InterruptFrame* frame) noexcept override;

    bool      isReady() const noexcept { return m_ready; }
    u32       getVersion() const noexcept { return m_version; }
    usize     getDiskCount() const noexcept { return m_diskCount; }
    SataDisk* getDisk(usize idx) noexcept {
        return (idx < m_diskCount) ? &m_disks[idx] : nullptr;
    }

    u32  readReg32(u32 offset) const noexcept;
    void writeReg32(u32 offset, u32 val) noexcept;

private:
    AhciController() noexcept = default;

    bool performBiosHandoff() noexcept;
    bool resetHba() noexcept;

    memory::IPhysicalAllocator* m_pmm             = nullptr;
    u64                         m_hhdmOffset      = 0;
    volatile u8*                m_abarVirt        = nullptr;
    u32                         m_version         = 0;
    bool                        m_ready           = false;

    AhciPort                    m_ports[MAX_PORTS]{};
    usize                       m_activePortCount = 0;

    SataDisk                    m_disks[MAX_DISKS]{};
    usize                       m_diskCount       = 0;
};

} // namespace cortez::drivers::storage
