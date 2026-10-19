/**
 * @file ahci_controller.cpp
 * @brief Implementation of the AHCI Host Controller driver.
 */

#include "ahci_controller.hpp"
#include "gpt_partition.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::storage {

AhciController& AhciController::instance() noexcept {
    static AhciController s_ahci;
    return s_ahci;
}

u32 AhciController::readReg32(u32 offset) const noexcept {
    return *reinterpret_cast<volatile const u32*>(m_abarVirt + offset);
}

void AhciController::writeReg32(u32 offset, u32 val) noexcept {
    *reinterpret_cast<volatile u32*>(m_abarVirt + offset) = val;
}

bool AhciController::performBiosHandoff() noexcept {
    u32 cap2 = readReg32(AHCI_REG_CAP2);
    if (!(cap2 & AHCI_CAP2_BOH)) {
        return true; // BIOS/OS Handoff not required by controller
    }

    u32 bohc = readReg32(AHCI_REG_BOHC);
    if (bohc & AHCI_BOHC_BOS) {
        pr_info("ahci", "Requesting OS ownership handoff from BIOS (BOHC=0x%08x)...", bohc);

        // Assert OS Owned Semaphore (OOS)
        writeReg32(AHCI_REG_BOHC, bohc | AHCI_BOHC_OOS);

        // Poll for BIOS to release ownership (BOHC.BOS == 0) within 50ms
        for (u32 elapsed = 0; elapsed < 50; ++elapsed) {
            if (!(readReg32(AHCI_REG_BOHC) & AHCI_BOHC_BOS)) {
                break;
            }
            arch::ApicController::instance().sleepMs(1);
        }

        // If BIOS is still busy, wait up to an additional 2000ms
        if (readReg32(AHCI_REG_BOHC) & AHCI_BOHC_BB) {
            pr_info("ahci", "BIOS Busy asserted, waiting up to 2 seconds for handoff...");
            for (u32 elapsed = 0; elapsed < 2000; ++elapsed) {
                if (!(readReg32(AHCI_REG_BOHC) & AHCI_BOHC_BB)) {
                    break;
                }
                arch::ApicController::instance().sleepMs(1);
            }
        }

        if (readReg32(AHCI_REG_BOHC) & AHCI_BOHC_BOS) {
            pr_warn("ahci", "BIOS did not release BOS ownership within timeout; forcing OS ownership");
        } else {
            pr_ok("ahci", "BIOS/OS ownership handoff completed successfully");
        }
    }

    return true;
}

bool AhciController::resetHba() noexcept {
    u32 ghc = readReg32(AHCI_REG_GHC);
    writeReg32(AHCI_REG_GHC, ghc | AHCI_GHC_HR);

    for (u32 elapsed = 0; elapsed < 1000; ++elapsed) {
        if (!(readReg32(AHCI_REG_GHC) & AHCI_GHC_HR)) {
            return true;
        }
        arch::ApicController::instance().sleepMs(1);
    }

    pr_err("ahci", "Timeout waiting for AHCI HBA Reset (GHC.HR) to clear");
    return false;
}

void AhciController::onInterrupt(arch::InterruptFrame*) noexcept {
    if (!m_abarVirt) return;
    u32 is = readReg32(AHCI_REG_IS);
    if (is != 0) {
        writeReg32(AHCI_REG_IS, is);
    }
}

Status AhciController::init(memory::IPhysicalAllocator* pmm,
                            memory::IVirtualMapper* vmm,
                            u64 hhdmOffset) noexcept {
    if (!pmm || !vmm) return Status::InvalidArgument;
    m_pmm        = pmm;
    m_hhdmOffset = hhdmOffset;

    // Locate SATA AHCI Controller via PCIe (Class 0x01 Mass Storage, Subclass 0x06 SATA, ProgIF 0x01 AHCI)
    const arch::PciDeviceInfo* pciDev = arch::PcieManager::instance().findByClass(0x01, 0x06, 0x01);
    if (!pciDev || pciDev->bar5Phys == 0) {
        // Fallback: check AHCI-compatible RAID controllers (Class 0x01, Subclass 0x04)
        pciDev = arch::PcieManager::instance().findByClass(0x01, 0x04, 0x00);
        if (!pciDev || pciDev->bar5Phys == 0) {
            pr_info("ahci", "No PCIe SATA AHCI controller (01:06:01) detected");
            return Status::DeviceNotFound;
        }
    }

    pr_info("ahci", "Found PCIe SATA AHCI controller at %02x:%02x.%u [VID:DID %04x:%04x] BAR5=0x%llx",
            pciDev->bus, pciDev->device, pciDev->function,
            pciDev->vendorId, pciDev->deviceId, pciDev->bar5Phys);

    arch::PcieManager::instance().enableBusMastering(*pciDev);

    // Map BAR5 (ABAR) MMIO uncached (PCD | PWT)
    auto mapRes = vmm->mapMmio(pciDev->bar5Phys, pciDev->bar5Size ? pciDev->bar5Size : 131072);
    if (mapRes.isError()) return mapRes.status();
    m_abarVirt = reinterpret_cast<volatile u8*>(mapRes.value());

    m_version = readReg32(AHCI_REG_VS);

    // Step 1: Perform BIOS/OS Handoff if AHCI 1.2+
    if (m_version >= 0x00010200U) {
        performBiosHandoff();
    }

    // Step 2: Global HBA Reset
    if (!resetHba()) {
        return Status::Timeout;
    }

    // Step 3: Enable AHCI mode (GHC.AE = 1)
    writeReg32(AHCI_REG_GHC, readReg32(AHCI_REG_GHC) | AHCI_GHC_AE);

    // Step 4: Read capabilities and version
    u32 cap     = readReg32(AHCI_REG_CAP);
    bool sss    = (cap & AHCI_CAP_SSS) != 0;
    u32 pi      = readReg32(AHCI_REG_PI);

    u32 verMajor = (m_version >> 16) & 0xFFFFU;
    u32 verMinor = (m_version >> 8) & 0xFFU;
    pr_ok("ahci", "AHCI Controller v%u.%u: CAP=0x%08x PI=0x%08x (64-bit=%s, SSS=%s)",
          verMajor, verMinor, cap, pi,
          (cap & AHCI_CAP_S64A) ? "Yes" : "No",
          sss ? "Yes" : "No");

    // Configure MSI interrupt routing if supported
    arch::IdtManager::instance().registerHandler(arch::IdtManager::VEC_AHCI_MSI, this);
    bool msiEnabled = arch::PcieManager::instance().enableMsiOrMsix(
        *pciDev,
        arch::IdtManager::VEC_AHCI_MSI,
        arch::ApicController::instance().getApicId());

    if (msiEnabled) {
        writeReg32(AHCI_REG_GHC, readReg32(AHCI_REG_GHC) | AHCI_GHC_IE);
    }

    // Step 5: Enumerate implemented ports
    m_activePortCount = 0;
    m_diskCount       = 0;

    for (u32 p = 0; p < 32 && m_activePortCount < MAX_PORTS; ++p) {
        if (!(pi & (1U << p))) continue;

        AhciPort& port = m_ports[m_activePortCount];
        Status st = port.init(p, m_abarVirt, sss, m_pmm, m_hhdmOffset);
        if (st == Status::Ok && port.isDriveConnected()) {
            if (port.isSataDisk() && m_diskCount < MAX_DISKS) {
                SataDisk& disk = m_disks[m_diskCount];
                if (disk.init(&port, static_cast<u32>(m_diskCount)) == Status::Ok) {
                    DeviceRegistry::instance().registerBlockDevice(&disk);
                    GptPartitionScanner::instance().scanDisk(&disk);
                    ++m_diskCount;
                }
            } else if (port.isAtapi()) {
                pr_info("ahci", "Port %u: Detected SATAPI optical drive", p);
            }
            ++m_activePortCount;
        }
    }

    m_ready = true;
    pr_ok("ahci", "AHCI initialization complete: %zu active port(s), %zu SATA disk(s) registered",
          m_activePortCount, m_diskCount);

    return Status::Ok;
}

} // namespace cortez::drivers::storage
