/**
 * @file ahci_port.cpp
 * @brief Implementation of AHCI Port driver, PHY reset, and synchronous ATA command execution.
 */

#include "ahci_port.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::storage {

u32 AhciPort::readReg(u32 offset) const noexcept {
    return *reinterpret_cast<volatile const u32*>(m_portMmio + offset);
}

void AhciPort::writeReg(u32 offset, u32 val) noexcept {
    *reinterpret_cast<volatile u32*>(m_portMmio + offset) = val;
}

const char* AhciPort::getSpeedString() const noexcept {
    switch (m_sataGen) {
        case 1:  return "1.5 Gbps (Gen 1)";
        case 2:  return "3.0 Gbps (Gen 2)";
        case 3:  return "6.0 Gbps (Gen 3)";
        default: return "Unknown";
    }
}

bool AhciPort::stopEngine() noexcept {
    u32 cmd = readReg(AHCI_PORT_CMD);
    if (!(cmd & (AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE))) {
        return true;
    }

    // Clear Start (ST) bit to halt DMA engine
    writeReg(AHCI_PORT_CMD, cmd & ~AHCI_PORT_CMD_ST);

    // Wait until Command List Running (CR) is cleared (max 500ms)
    for (u32 elapsed = 0; elapsed < 500; ++elapsed) {
        if (!(readReg(AHCI_PORT_CMD) & AHCI_PORT_CMD_CR)) break;
        arch::ApicController::instance().sleepMs(1);
    }
    if (readReg(AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) {
        pr_warn("ahci", "Port %u: Timeout waiting for PxCMD.CR=0 during stop", m_portIndex);
        return false;
    }

    // Clear FIS Receive Enable (FRE) bit
    cmd = readReg(AHCI_PORT_CMD);
    writeReg(AHCI_PORT_CMD, cmd & ~AHCI_PORT_CMD_FRE);

    // Wait until FIS Receive Running (FR) is cleared (max 500ms)
    for (u32 elapsed = 0; elapsed < 500; ++elapsed) {
        if (!(readReg(AHCI_PORT_CMD) & AHCI_PORT_CMD_FR)) break;
        arch::ApicController::instance().sleepMs(1);
    }
    if (readReg(AHCI_PORT_CMD) & AHCI_PORT_CMD_FR) {
        pr_warn("ahci", "Port %u: Timeout waiting for PxCMD.FR=0 during stop", m_portIndex);
        return false;
    }

    return true;
}

bool AhciPort::startEngine() noexcept {
    // Wait until CR is 0 before starting
    for (u32 elapsed = 0; elapsed < 500; ++elapsed) {
        if (!(readReg(AHCI_PORT_CMD) & AHCI_PORT_CMD_CR)) break;
        arch::ApicController::instance().sleepMs(1);
    }

    u32 cmd = readReg(AHCI_PORT_CMD);
    // Enable FIS reception first (FRE)
    writeReg(AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_FRE);

    // Set Start bit (ST)
    cmd = readReg(AHCI_PORT_CMD);
    writeReg(AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_ST);

    return true;
}

bool AhciPort::resetPhy() noexcept {
    // Drive COMRESET on the port via SControl
    u32 sctl = readReg(AHCI_PORT_SCTL);
    sctl = (sctl & ~AHCI_SSTS_DET_MASK) | AHCI_SCTL_DET_RESET;
    writeReg(AHCI_PORT_SCTL, sctl);

    arch::ApicController::instance().sleepMs(2);

    // De-assert reset
    sctl = readReg(AHCI_PORT_SCTL);
    sctl &= ~AHCI_SSTS_DET_MASK;
    writeReg(AHCI_PORT_SCTL, sctl);

    // Poll for PHY communication established (PxSSTS.DET == 3)
    for (u32 elapsed = 0; elapsed < 300; ++elapsed) {
        u32 ssts = readReg(AHCI_PORT_SSTS);
        if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_PRESENT) {
            m_sataGen = static_cast<u8>((ssts & AHCI_SSTS_SPD_MASK) >> 4);
            return true;
        }
        arch::ApicController::instance().sleepMs(1);
    }

    return false;
}

Status AhciPort::init(u32 portIndex,
                      volatile u8* abarVirt,
                      bool staggeredSpinup,
                      memory::IPhysicalAllocator* pmm,
                      u64 hhdmOffset) noexcept {
    m_portIndex  = portIndex;
    m_portMmio   = abarVirt + AHCI_PORT_OFFSET_BASE + (portIndex * AHCI_PORT_OFFSET_STRIDE);
    m_pmm        = pmm;
    m_hhdmOffset = hhdmOffset;

    // Check device detection state
    u32 ssts = readReg(AHCI_PORT_SSTS);
    u32 det  = ssts & AHCI_SSTS_DET_MASK;

    // Handle Staggered Spin-Up or Cold-Boot drive power
    u32 cmd = readReg(AHCI_PORT_CMD);
    if (staggeredSpinup || !(cmd & AHCI_PORT_CMD_POD) || !(cmd & AHCI_PORT_CMD_SUD)) {
        cmd |= (AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD | AHCI_PORT_CMD_ICC_ACTIVE);
        writeReg(AHCI_PORT_CMD, cmd);
        arch::ApicController::instance().sleepMs(10);
    }

    // If no device or unestablished PHY, attempt COMRESET
    if (det != AHCI_SSTS_DET_PRESENT) {
        if (!resetPhy()) {
            return Status::DeviceNotFound;
        }
    } else {
        m_sataGen = static_cast<u8>((ssts & AHCI_SSTS_SPD_MASK) >> 4);
    }

    // Stop port engine before reprogramming DMA pointers
    if (!stopEngine()) {
        return Status::Busy;
    }

    // Allocate 1 physical page (4 KiB) for DMA command headers, received FIS, and command table
    auto dmaRes = m_pmm->allocPages(1);
    if (dmaRes.isError()) return dmaRes.status();
    m_dmaPagePhys = dmaRes.value();
    m_dmaPageVirt = reinterpret_cast<u8*>(m_dmaPagePhys + m_hhdmOffset);
    memset(m_dmaPageVirt, 0, PAGE_SIZE_4K);

    // Layout within 4 KiB page:
    // Offset 0x000 (1024 B): Command List (32 slots)
    // Offset 0x400 (256 B):  Received FIS
    // Offset 0x500 (384 B):  Command Table for Slot 0
    phys_addr_t clbPhys      = m_dmaPagePhys;
    phys_addr_t fbPhys       = m_dmaPagePhys + 1024ULL;
    phys_addr_t cmdTablePhys = m_dmaPagePhys + 1280ULL;

    writeReg(AHCI_PORT_CLB,  static_cast<u32>(clbPhys & 0xFFFFFFFFULL));
    writeReg(AHCI_PORT_CLBU, static_cast<u32>(clbPhys >> 32));
    writeReg(AHCI_PORT_FB,   static_cast<u32>(fbPhys & 0xFFFFFFFFULL));
    writeReg(AHCI_PORT_FBU,  static_cast<u32>(fbPhys >> 32));

    // Pre-initialize Command Header 0 to point to Command Table
    auto* headers = reinterpret_cast<AhciCommandHeader*>(m_dmaPageVirt);
    headers[0].ctba  = static_cast<u32>(cmdTablePhys & 0xFFFFFFFFULL);
    headers[0].ctbau = static_cast<u32>(cmdTablePhys >> 32);

    // Allocate 64 KiB (16 pages) contiguous DMA bounce buffer
    auto bounceRes = m_pmm->allocPages(16);
    if (bounceRes.isError()) return bounceRes.status();
    m_bouncePhys = bounceRes.value();
    m_bounceVirt = reinterpret_cast<u8*>(m_bouncePhys + m_hhdmOffset);
    memset(m_bounceVirt, 0, BOUNCE_SIZE);

    // Clear any pending errors & interrupts
    writeReg(AHCI_PORT_SERR, 0xFFFFFFFFU);
    writeReg(AHCI_PORT_IS,   0xFFFFFFFFU);

    // Start port DMA engine
    if (!startEngine()) {
        return Status::Busy;
    }

    // Wait up to 50ms for initial signature to stabilize
    for (u32 elapsed = 0; elapsed < 50; ++elapsed) {
        m_signature = readReg(AHCI_PORT_SIG);
        if (m_signature != 0 && m_signature != 0xFFFFFFFFU) break;
        arch::ApicController::instance().sleepMs(1);
    }

    m_connected = true;
    if (m_signature == SATA_SIG_ATA) {
        m_isSataDisk = true;
    } else if (m_signature == SATA_SIG_ATAPI) {
        m_isAtapi = true;
    }

    return Status::Ok;
}

Status AhciPort::executeCommand(u8 command,
                               u64 lba,
                               u16 count,
                               void* buffer,
                               bool isWrite) noexcept {
    IrqLockGuard guard(m_lock);

    if (!m_connected) return Status::DeviceNotFound;

    // Calculate transfer size
    u32 sectorCount = (count > 0) ? count : 1U;
    u32 byteCount   = (buffer != nullptr) ? (sectorCount * 512U) : 0U;
    if (byteCount > BOUNCE_SIZE) {
        return Status::InvalidArgument;
    }

    // Set up Command Table at offset 1280 (Slot 0)
    auto* cmdTable = reinterpret_cast<AhciCommandTable*>(m_dmaPageVirt + 1280ULL);
    memset(cmdTable, 0, sizeof(AhciCommandTable));

    // Construct Host to Device Register FIS (0x27)
    auto* cfis = reinterpret_cast<FisRegH2D*>(cmdTable->cfis);
    cfis->fisType    = FIS_TYPE_REG_H2D;
    cfis->pmPortAndC = 0x80U; // bit 7: C = 1 (Command)
    cfis->command    = command;
    cfis->device     = 1U << 6; // LBA mode

    cfis->lba0       = static_cast<u8>(lba & 0xFFU);
    cfis->lba1       = static_cast<u8>((lba >> 8) & 0xFFU);
    cfis->lba2       = static_cast<u8>((lba >> 16) & 0xFFU);
    cfis->lba3       = static_cast<u8>((lba >> 24) & 0xFFU);
    cfis->lba4       = static_cast<u8>((lba >> 32) & 0xFFU);
    cfis->lba5       = static_cast<u8>((lba >> 40) & 0xFFU);

    cfis->countLow   = static_cast<u8>(count & 0xFFU);
    cfis->countHigh  = static_cast<u8>((count >> 8) & 0xFFU);

    // If writing, copy source buffer into contiguous DMA bounce buffer
    if (isWrite && buffer && byteCount > 0) {
        memcpy(m_bounceVirt, buffer, byteCount);
    }

    // Setup PRDT Entry 0
    if (buffer && byteCount > 0) {
        cmdTable->prdt[0].dba              = static_cast<u32>(m_bouncePhys & 0xFFFFFFFFULL);
        cmdTable->prdt[0].dbau             = static_cast<u32>(m_bouncePhys >> 32);
        cmdTable->prdt[0].dbcAndInterrupt  = (byteCount - 1U) | (1U << 31); // bit 31 = Interrupt on completion
    }

    // Setup Command Header 0
    auto* headers = reinterpret_cast<AhciCommandHeader*>(m_dmaPageVirt);
    u16 cfl = static_cast<u16>(sizeof(FisRegH2D) / sizeof(u32)); // 5 DWORDs
    headers[0].flagsAndCfl = cfl | (isWrite ? (1U << 6) : 0U);
    headers[0].prdtl       = (buffer && byteCount > 0) ? 1U : 0U;
    headers[0].prdbc       = 0;

    // Clear interrupt and error registers before issuing
    writeReg(AHCI_PORT_IS,   0xFFFFFFFFU);
    writeReg(AHCI_PORT_SERR, 0xFFFFFFFFU);

    // Issue command on Slot 0
    writeReg(AHCI_PORT_CI, 1U << 0);

    // Synchronously poll for completion with timeout (3 seconds)
    constexpr u32 TIMEOUT_MS = 3000;
    bool completed = false;
    for (u32 elapsed = 0; elapsed < TIMEOUT_MS; ++elapsed) {
        u32 ci = readReg(AHCI_PORT_CI);
        if (!(ci & 1U)) {
            completed = true;
            break;
        }

        u32 is = readReg(AHCI_PORT_IS);
        if (is & AHCI_PORT_IS_FATAL_ERRORS) {
            pr_err("ahci", "Port %u: Fatal interrupt error 0x%08x on cmd 0x%02x", m_portIndex, is, command);
            writeReg(AHCI_PORT_IS, is);
            return Status::IoError;
        }

        u32 tfd = readReg(AHCI_PORT_TFD);
        if ((tfd & AHCI_TFD_STS_ERR) && !(tfd & AHCI_TFD_STS_BSY)) {
            pr_err("ahci", "Port %u: Task File error 0x%08x on cmd 0x%02x", m_portIndex, tfd, command);
            return Status::IoError;
        }

        arch::ApicController::instance().sleepMs(1);
    }

    if (!completed) {
        pr_err("ahci", "Port %u: Timeout waiting for cmd 0x%02x completion (CI=0x%08x)",
               m_portIndex, command, readReg(AHCI_PORT_CI));
        return Status::Timeout;
    }

    u32 tfd = readReg(AHCI_PORT_TFD);
    if (tfd & AHCI_TFD_STS_ERR) {
        return Status::IoError;
    }

    // If reading, copy back from contiguous DMA bounce buffer to destination buffer
    if (!isWrite && buffer && byteCount > 0) {
        memcpy(buffer, m_bounceVirt, byteCount);
    }

    return Status::Ok;
}

Status AhciPort::flush() noexcept {
    return executeCommand(ATA_CMD_FLUSH_EXT, 0, 0, nullptr, false);
}

} // namespace cortez::drivers::storage
