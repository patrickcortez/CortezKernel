/**
 * @file nvme_controller.cpp
 * @brief Implementation of the physical PCIe NVMe Controller and Namespace driver.
 */

#include "nvme_controller.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::storage {

namespace {
void copyTrimmedAscii(char* dst, const char* src, usize len) noexcept {
    usize end = len;
    while (end > 0 && (src[end - 1] == ' ' || src[end - 1] == '\0')) {
        --end;
    }
    for (usize i = 0; i < end; ++i) {
        dst[i] = src[i];
    }
    dst[end] = '\0';
}
} // namespace

void NvmeNamespace::init(NvmeController* ctrl, u32 nsid, u32 blockSize, u64 blockCount) noexcept {
    m_ctrl       = ctrl;
    m_nsid       = nsid;
    m_blockSize  = blockSize;
    m_blockCount = blockCount;
}

Status NvmeNamespace::readBlocks(u64 lba, u32 count, void* buffer) {
    if (!m_ctrl || !buffer) return Status::InvalidArgument;
    if (count == 0) return Status::Ok;
    if (lba + count > m_blockCount) return Status::InvalidArgument;

    auto* dst = static_cast<u8*>(buffer);
    for (u32 i = 0; i < count; ++i) {
        Status st = m_ctrl->rwSingleBlock(m_nsid, lba + i, m_blockSize, dst + static_cast<usize>(i) * m_blockSize, false);
        if (st != Status::Ok) return st;
    }
    return Status::Ok;
}

Status NvmeNamespace::writeBlocks(u64 lba, u32 count, const void* buffer) {
    if (!m_ctrl || !buffer) return Status::InvalidArgument;
    if (count == 0) return Status::Ok;
    if (lba + count > m_blockCount) return Status::InvalidArgument;

    const auto* src = static_cast<const u8*>(buffer);
    for (u32 i = 0; i < count; ++i) {
        Status st = m_ctrl->rwSingleBlock(m_nsid,
                                          lba + i,
                                          m_blockSize,
                                          const_cast<u8*>(src + static_cast<usize>(i) * m_blockSize),
                                          true);
        if (st != Status::Ok) return st;
    }
    return Status::Ok;
}

Status NvmeNamespace::flush() {
    if (!m_ctrl) return Status::DeviceNotFound;
    return m_ctrl->flushNamespace(m_nsid);
}

// =============================================================================
// NvmeController Implementation
// =============================================================================

NvmeController& NvmeController::instance() noexcept {
    static NvmeController s_nvme;
    return s_nvme;
}

u32 NvmeController::readReg32(u32 offset) const noexcept {
    return *reinterpret_cast<volatile const u32*>(m_bar0Virt + offset);
}

void NvmeController::writeReg32(u32 offset, u32 value) noexcept {
    *reinterpret_cast<volatile u32*>(m_bar0Virt + offset) = value;
}

u64 NvmeController::readReg64(u32 offset) const noexcept {
    u32 low  = readReg32(offset);
    u32 high = readReg32(offset + 4);
    return (static_cast<u64>(high) << 32) | low;
}

void NvmeController::writeReg64(u32 offset, u64 value) noexcept {
    writeReg32(offset, static_cast<u32>(value & 0xFFFFFFFFULL));
    writeReg32(offset + 4, static_cast<u32>(value >> 32));
}

bool NvmeController::waitControllerReady(bool expectedRdy, u32 timeoutMs) const noexcept {
    u32 targetBit = expectedRdy ? 1U : 0U;
    for (u32 elapsed = 0; elapsed < timeoutMs; ++elapsed) {
        u32 csts = readReg32(NVME_REG_CSTS);
        if (csts & 0x02U) {
            // CFS (Controller Fatal Status) bit 1
            return false;
        }
        if ((csts & 0x01U) == targetBit) {
            return true;
        }
        arch::ApicController::instance().sleepMs(1);
    }
    return ((readReg32(NVME_REG_CSTS) & 0x01U) == targetBit);
}

Status NvmeController::init(memory::IPhysicalAllocator* pmm,
                            memory::IVirtualMapper* vmm,
                            u64 hhdmOffset) noexcept {
    if (!pmm || !vmm) return Status::InvalidArgument;

    m_pmm        = pmm;
    m_hhdmOffset = hhdmOffset;

    // Locate physical NVMe Controller via PCIe (Class 0x01 Mass Storage, Subclass 0x08 NVM, ProgIF 0x02 NVMe)
    const arch::PciDeviceInfo* pciDev = arch::PcieManager::instance().findByClass(0x01, 0x08, 0x02);
    if (!pciDev || pciDev->bar0Phys == 0) {
        pr_info("nvme", "No PCIe NVMe controller (01:08:02) detected");
        return Status::DeviceNotFound;
    }

    pr_info("nvme", "Found PCIe NVMe controller at %02x:%02x.%u [VID:DID %04x:%04x] BAR0=0x%llx",
            pciDev->bus, pciDev->device, pciDev->function,
            pciDev->vendorId, pciDev->deviceId, pciDev->bar0Phys);

    arch::PcieManager::instance().enableBusMastering(*pciDev);

    // Map BAR0 MMIO uncached (PCD | PWT)
    auto mapRes = vmm->mapMmio(pciDev->bar0Phys, pciDev->bar0Size ? pciDev->bar0Size : 65536);
    if (mapRes.isError()) return mapRes.status();
    m_bar0Virt = reinterpret_cast<volatile u8*>(mapRes.value());

    // Allocate 4 KiB physical DMA bounce buffer
    auto dmaRes = m_pmm->allocPages(1);
    if (dmaRes.isError()) return dmaRes.status();
    m_dmaPagePhys = dmaRes.value();
    m_dmaPageVirt = reinterpret_cast<u8*>(m_dmaPagePhys + m_hhdmOffset);

    u64 cap   = readReg64(NVME_REG_CAP);
    m_version = readReg32(NVME_REG_VS);

    u32 dstrd        = static_cast<u32>((cap >> 32) & 0x0FULL);
    m_doorbellStride = 4U << dstrd;

    u32 capTimeout500ms = static_cast<u32>((cap >> 24) & 0xFFULL);
    u32 resetTimeoutMs  = (capTimeout500ms > 0) ? (capTimeout500ms * 500U) : 2000U;

    // Step 1: UEFI-to-Kernel Controller Reset Hand-off (clear CC.EN if already enabled)
    u32 cc = readReg32(NVME_REG_CC);
    if (cc & 0x01U) {
        writeReg32(NVME_REG_CC, cc & ~0x01U);
    }
    if (!waitControllerReady(false, resetTimeoutMs)) {
        pr_err("nvme", "Timeout waiting for NVMe CSTS.RDY=0 during reset");
        return Status::Timeout;
    }

    // Step 2: Initialize Admin Queue Pair (QID 0, depth 32)
    constexpr u16 ADMIN_DEPTH = 32;
    Status st = m_adminQueue.init(0, ADMIN_DEPTH, m_bar0Virt, m_doorbellStride, m_pmm, m_hhdmOffset);
    if (st != Status::Ok) return st;

    u32 aqa = (static_cast<u32>(ADMIN_DEPTH - 1) << 16) | static_cast<u32>(ADMIN_DEPTH - 1);
    writeReg32(NVME_REG_AQA, aqa);
    writeReg64(NVME_REG_ASQ, m_adminQueue.getSqPhys());
    writeReg64(NVME_REG_ACQ, m_adminQueue.getCqPhys());

    // Configure PCI MSI / MSI-X interrupt routing
    arch::IdtManager::instance().registerHandler(arch::IdtManager::VEC_NVME_MSI, this);
    bool msiEnabled = arch::PcieManager::instance().enableMsiOrMsix(
        *pciDev,
        arch::IdtManager::VEC_NVME_MSI,
        arch::ApicController::instance().getApicId());

    // Step 3: Enable Controller (IOSQES=6 [64B], IOCQES=4 [16B], MPS=0 [4KiB], CSS=0, EN=1)
    u32 newCc = (6U << 16) | (4U << 20) | (0U << 7) | (0U << 4) | 1U;
    writeReg32(NVME_REG_CC, newCc);

    if (!waitControllerReady(true, resetTimeoutMs)) {
        pr_err("nvme", "Timeout waiting for NVMe CSTS.RDY=1 after enable");
        return Status::Timeout;
    }

    // Unmask controller interrupts
    writeReg32(NVME_REG_INTMC, 0xFFFFFFFFU);

    // Step 4: Identify Controller (CNS = 0x01)
    st = identifyController();
    if (st != Status::Ok) return st;

    // Step 5: Create I/O Completion Queue (QID 1) and I/O Submission Queue (QID 1)
    st = createIoQueues();
    if (st != Status::Ok) return st;

    // Step 6: Identify Namespace 1 (CNS = 0x00) and register /dev/nvme0n1
    st = identifyNamespace(1);
    if (st != Status::Ok) return st;

    m_ready = true;
    pr_ok("nvme", "NVMe %u.%u ready: Model='%s' Serial='%s' FW='%s' (%s)",
          (m_version >> 16) & 0xFFFFU, (m_version >> 8) & 0xFFU,
          m_model, m_serial, m_firmware,
          msiEnabled ? "MSI/MSI-X" : "Phase-Poll");

    return Status::Ok;
}

Status NvmeController::identifyController() noexcept {
    IrqLockGuard guard(m_dmaLock);
    memset(m_dmaPageVirt, 0, PAGE_SIZE_4K);

    NvmeSqe sqe{};
    sqe.opcode = NVME_ADMIN_IDENTIFY;
    sqe.nsid   = 0;
    sqe.prp1   = m_dmaPagePhys;
    sqe.cdw10  = 0x01U; // CNS = 0x01 (Identify Controller)

    Status st = m_adminQueue.submitAndWait(sqe);
    if (st != Status::Ok) {
        pr_err("nvme", "Admin Identify Controller command failed");
        return st;
    }

    const auto* idCtrl = reinterpret_cast<const NvmeIdentifyController*>(m_dmaPageVirt);
    copyTrimmedAscii(m_model, idCtrl->mn, sizeof(idCtrl->mn));
    copyTrimmedAscii(m_serial, idCtrl->sn, sizeof(idCtrl->sn));
    copyTrimmedAscii(m_firmware, idCtrl->fr, sizeof(idCtrl->fr));

    return Status::Ok;
}

Status NvmeController::createIoQueues() noexcept {
    constexpr u16 IO_QID   = 1;
    constexpr u16 IO_DEPTH = 64;

    Status st = m_ioQueue.init(IO_QID, IO_DEPTH, m_bar0Virt, m_doorbellStride, m_pmm, m_hhdmOffset);
    if (st != Status::Ok) return st;

    // Request at least 1 I/O SQ and 1 I/O CQ via Set Features (FID = 0x07, non-fatal if ignored)
    {
        NvmeSqe featSqe{};
        featSqe.opcode = NVME_ADMIN_SET_FEAT;
        featSqe.cdw10  = 0x07U; // Number of Queues
        featSqe.cdw11  = 0x00000000U; // 1 SQ (0-based) | 1 CQ (0-based)
        (void)m_adminQueue.submitAndWait(featSqe);
    }

    // Create I/O Completion Queue (Opcode 0x05)
    {
        NvmeSqe cqSqe{};
        cqSqe.opcode = NVME_ADMIN_CREATE_CQ;
        cqSqe.prp1   = m_ioQueue.getCqPhys();
        cqSqe.cdw10  = (static_cast<u32>(IO_DEPTH - 1) << 16) | IO_QID;
        cqSqe.cdw11  = 0x01U; // Bit 0 = PC (Physically Contiguous)
        st = m_adminQueue.submitAndWait(cqSqe);
        if (st != Status::Ok) {
            pr_err("nvme", "Failed to create I/O Completion Queue 1");
            return st;
        }
    }

    // Create I/O Submission Queue (Opcode 0x01)
    {
        NvmeSqe sqSqe{};
        sqSqe.opcode = NVME_ADMIN_CREATE_SQ;
        sqSqe.prp1   = m_ioQueue.getSqPhys();
        sqSqe.cdw10  = (static_cast<u32>(IO_DEPTH - 1) << 16) | IO_QID;
        sqSqe.cdw11  = (static_cast<u32>(IO_QID) << 16) | 0x01U; // CQID = 1, PC = 1
        st = m_adminQueue.submitAndWait(sqSqe);
        if (st != Status::Ok) {
            pr_err("nvme", "Failed to create I/O Submission Queue 1");
            return st;
        }
    }

    return Status::Ok;
}

Status NvmeController::identifyNamespace(u32 nsid) noexcept {
    IrqLockGuard guard(m_dmaLock);
    memset(m_dmaPageVirt, 0, PAGE_SIZE_4K);

    NvmeSqe sqe{};
    sqe.opcode = NVME_ADMIN_IDENTIFY;
    sqe.nsid   = nsid;
    sqe.prp1   = m_dmaPagePhys;
    sqe.cdw10  = 0x00U; // CNS = 0x00 (Identify Namespace)

    Status st = m_adminQueue.submitAndWait(sqe);
    if (st != Status::Ok) {
        pr_err("nvme", "Admin Identify Namespace %u failed", nsid);
        return st;
    }

    const auto* idNs = reinterpret_cast<const NvmeIdentifyNamespace*>(m_dmaPageVirt);
    if (idNs->nsze == 0) {
        pr_err("nvme", "Namespace %u reported 0 logical blocks", nsid);
        return Status::DeviceNotFound;
    }

    u8  lbafIdx   = idNs->flbas & 0x0FU;
    u8  lbads     = idNs->lbaf[lbafIdx].lbads;
    u32 blockSize = (lbads >= 9 && lbads <= 12) ? (1U << lbads) : 512U;
    u64 numBlocks = idNs->nsze;

    m_namespace.init(this, nsid, blockSize, numBlocks);
    DeviceRegistry::instance().registerBlockDevice(&m_namespace);

    u64 sizeMiB = (numBlocks * blockSize) / (1024ULL * 1024ULL);
    pr_ok("nvme", "Registered /dev/%s: %llu blocks x %u B (%llu MiB)",
          m_namespace.getName(), numBlocks, blockSize, sizeMiB);

    return Status::Ok;
}

Status NvmeController::rwSingleBlock(u32 nsid, u64 lba, u32 blockSize, void* buffer, bool isWrite) noexcept {
    if (!m_ready || !buffer || blockSize > PAGE_SIZE_4K) {
        return Status::InvalidArgument;
    }

    IrqLockGuard guard(m_dmaLock);

    if (isWrite) {
        memcpy(m_dmaPageVirt, buffer, blockSize);
    }

    NvmeSqe sqe{};
    sqe.opcode = isWrite ? NVME_IO_WRITE : NVME_IO_READ;
    sqe.nsid   = nsid;
    sqe.prp1   = m_dmaPagePhys;
    sqe.prp2   = 0;
    sqe.cdw10  = static_cast<u32>(lba & 0xFFFFFFFFULL);
    sqe.cdw11  = static_cast<u32>(lba >> 32);
    sqe.cdw12  = 0; // NLB = 0 (0-based -> 1 logical block)

    Status st = m_ioQueue.submitAndWait(sqe);
    if (st != Status::Ok) {
        return st;
    }

    if (!isWrite) {
        memcpy(buffer, m_dmaPageVirt, blockSize);
    }

    return Status::Ok;
}

Status NvmeController::flushNamespace(u32 nsid) noexcept {
    if (!m_ready) return Status::DeviceNotFound;

    NvmeSqe sqe{};
    sqe.opcode = NVME_IO_FLUSH;
    sqe.nsid   = nsid;
    return m_ioQueue.submitAndWait(sqe);
}

void NvmeController::onInterrupt(arch::InterruptFrame*) noexcept {
    // MSI/MSI-X wakeup notification
}

} // namespace cortez::drivers::storage
