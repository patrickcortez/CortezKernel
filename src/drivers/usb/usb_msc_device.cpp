/**
 * @file usb_msc_device.cpp
 * @brief Implementation of the USB Mass Storage Class (Bulk-Only Transport / SCSI) Block Device driver.
 */

#include "usb_msc_device.hpp"
#include "xhci_controller.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::usb {

namespace {
void copyTrimmedScsiAscii(char* dst, const char* src, usize len) noexcept {
    usize end = len;
    while (end > 0 && (src[end - 1] == ' ' || src[end - 1] == '\0')) {
        --end;
    }
    usize start = 0;
    while (start < end && src[start] == ' ') {
        ++start;
    }
    usize outIdx = 0;
    for (usize i = start; i < end; ++i) {
        char c = src[i];
        dst[outIdx++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    dst[outIdx] = '\0';
}
} // namespace

Status UsbMscDevice::init(XhciController* ctrl,
                          u32 deviceIndex,
                          u8 slotId,
                          u8 portNum,
                          u8 speedId,
                          u16 vendorId,
                          u16 productId,
                          u8 interfaceNum,
                          u8 bulkInDci,
                          u8 bulkOutDci,
                          u8 bulkInEpAddr,
                          u8 bulkOutEpAddr,
                          memory::IPhysicalAllocator* pmm,
                          u64 hhdmOffset) noexcept {
    if (!ctrl || !pmm) return Status::InvalidArgument;

    m_ctrl          = ctrl;
    m_slotId        = slotId;
    m_portNum       = portNum;
    m_speedId       = speedId;
    m_usbVid        = vendorId;
    m_usbPid        = productId;
    m_interfaceNum  = interfaceNum;
    m_bulkInDci     = bulkInDci;
    m_bulkOutDci    = bulkOutDci;
    m_bulkInEpAddr  = bulkInEpAddr;
    m_bulkOutEpAddr = bulkOutEpAddr;

    // Build block device name: "usb0", "usb1", ...
    str::copy(m_name, "usb", sizeof(m_name));
    char idxStr[8]{};
    str::uintToStr(deviceIndex, idxStr, 10, 0);
    str::concat(m_name, idxStr, sizeof(m_name));

    // Allocate 4 KiB physical DMA page for CBW, Data Stage, and CSW
    // Layout within page:
    //   [0x000 .. 0x03F]: 31-byte CBW (64-byte aligned)
    //   [0x040 .. 0x07F]: 13-byte CSW (64-byte aligned)
    //   [0x200 .. 0xFFF]: Data transfer buffer (up to 3584 bytes, or full sector 512..2048 B)
    // Wait: to support 4096-byte logical blocks cleanly, let's allocate 2 pages:
    //   Page 0: CBW + CSW control wrappers
    //   Page 1: Full 4096-byte sector data bounce buffer
    auto dmaRes = pmm->allocPages(2);
    if (dmaRes.isError()) return dmaRes.status();
    m_dmaPagePhys = dmaRes.value();
    m_dmaPageVirt = reinterpret_cast<u8*>(m_dmaPagePhys + hhdmOffset);
    memset(m_dmaPageVirt, 0, PAGE_SIZE_4K * 2);

    // Step 1: SCSI INQUIRY (0x12)
    Status st = scsiInquiry();
    if (st != Status::Ok) {
        pr_warn("usb-msc", "/dev/%s: SCSI INQUIRY failed", m_name);
        return st;
    }

    // Step 2: Poll SCSI TEST UNIT READY (0x00) + REQUEST SENSE (0x03) until flash media is ready
    st = scsiWaitReady();
    if (st != Status::Ok) {
        pr_warn("usb-msc", "/dev/%s: USB Mass Storage media did not become ready", m_name);
        return st;
    }

    // Step 3: SCSI READ CAPACITY (10)/(16)
    st = scsiReadCapacity();
    if (st != Status::Ok) {
        pr_warn("usb-msc", "/dev/%s: SCSI READ CAPACITY failed", m_name);
        return st;
    }

    m_ready = true;
    DeviceRegistry::instance().registerBlockDevice(this);

    u64 sizeMiB = (m_blockCount * m_blockSize) / (1024ULL * 1024ULL);
    pr_ok("usb-msc", "Registered /dev/%s: '%s %s' (%s) — %llu blocks x %u B (%llu MiB)",
          m_name, m_vendorStr, m_productStr, m_revisionStr,
          m_blockCount, m_blockSize, sizeMiB);

    return Status::Ok;
}

Status UsbMscDevice::executeScsiCommand(const u8* cdb,
                                        u8 cdbLen,
                                        void* dataBuffer,
                                        u32 dataLength,
                                        bool isDataIn) noexcept {
    if (!m_ctrl || !cdb || cdbLen == 0 || cdbLen > 16 || dataLength > PAGE_SIZE_4K) {
        return Status::InvalidArgument;
    }

    phys_addr_t cbwPhys  = m_dmaPagePhys;
    phys_addr_t cswPhys  = m_dmaPagePhys + 64ULL;
    phys_addr_t dataPhys = m_dmaPagePhys + PAGE_SIZE_4K;

    auto* cbwVirt  = reinterpret_cast<UsbMscCbw*>(m_dmaPageVirt);
    auto* cswVirt  = reinterpret_cast<UsbMscCsw*>(m_dmaPageVirt + 64ULL);
    u8*   dataVirt = m_dmaPageVirt + PAGE_SIZE_4K;

    u32 tag = m_nextTag++;
    if (m_nextTag == 0) m_nextTag = 1;

    memset(cbwVirt, 0, sizeof(UsbMscCbw));
    cbwVirt->signature          = USB_MSC_CBW_SIGNATURE;
    cbwVirt->tag                = tag;
    cbwVirt->dataTransferLength = dataLength;
    cbwVirt->flags              = (dataLength > 0 && isDataIn) ? USB_MSC_DIR_IN : USB_MSC_DIR_OUT;
    cbwVirt->lun                = 0;
    cbwVirt->cbLength           = cdbLen;
    memcpy(cbwVirt->cb, cdb, cdbLen);

    // Phase 1: Command Transport (Bulk OUT 31-byte CBW)
    Status st = m_ctrl->bulkTransfer(m_slotId, m_bulkOutDci, m_bulkOutEpAddr,
                                     m_bulkOutRing, cbwPhys, sizeof(UsbMscCbw), false);
    if (st != Status::Ok) {
        return st;
    }

    // Phase 2: Optional Data Transport (Bulk IN or Bulk OUT)
    if (dataLength > 0) {
        if (!isDataIn && dataBuffer) {
            memcpy(dataVirt, dataBuffer, dataLength);
        } else {
            memset(dataVirt, 0, dataLength);
        }

        u8           dci    = isDataIn ? m_bulkInDci : m_bulkOutDci;
        u8           epAddr = isDataIn ? m_bulkInEpAddr : m_bulkOutEpAddr;
        XhciTrbRing& ring   = isDataIn ? m_bulkInRing : m_bulkOutRing;

        Status dataSt = m_ctrl->bulkTransfer(m_slotId, dci, epAddr, ring, dataPhys, dataLength, isDataIn);
        if (dataSt != Status::Ok) {
            // Per USB MSC BOT §6.7.2/6.7.3: if data stage stalls, endpoint stall was cleared;
            // we must still attempt to read the CSW before returning the error.
            memset(cswVirt, 0, sizeof(UsbMscCsw));
            (void)m_ctrl->bulkTransfer(m_slotId, m_bulkInDci, m_bulkInEpAddr,
                                       m_bulkInRing, cswPhys, sizeof(UsbMscCsw), true);
            return dataSt;
        }

        if (isDataIn && dataBuffer) {
            memcpy(dataBuffer, dataVirt, dataLength);
        }
    }

    // Phase 3: Status Transport (Bulk IN 13-byte CSW)
    memset(cswVirt, 0, sizeof(UsbMscCsw));
    st = m_ctrl->bulkTransfer(m_slotId, m_bulkInDci, m_bulkInEpAddr,
                              m_bulkInRing, cswPhys, sizeof(UsbMscCsw), true);
    if (st != Status::Ok) {
        // Attempt one CSW retry after stall clearance per BOT spec Figure 2
        st = m_ctrl->bulkTransfer(m_slotId, m_bulkInDci, m_bulkInEpAddr,
                                  m_bulkInRing, cswPhys, sizeof(UsbMscCsw), true);
        if (st != Status::Ok) {
            return st;
        }
    }

    if (cswVirt->signature != USB_MSC_CSW_SIGNATURE || cswVirt->tag != tag) {
        return Status::IoError;
    }

    if (cswVirt->status != 0x00U) {
        return Status::IoError;
    }

    return Status::Ok;
}

Status UsbMscDevice::scsiInquiry() noexcept {
    u8 cdb[6]{};
    cdb[0] = SCSI_CMD_INQUIRY;
    cdb[4] = sizeof(ScsiInquiryData); // Allocation Length = 36 bytes

    ScsiInquiryData inq{};
    Status st = executeScsiCommand(cdb, sizeof(cdb), &inq, sizeof(inq), true);
    if (st != Status::Ok) return st;

    copyTrimmedScsiAscii(m_vendorStr, inq.vendorId, sizeof(inq.vendorId));
    copyTrimmedScsiAscii(m_productStr, inq.productId, sizeof(inq.productId));
    copyTrimmedScsiAscii(m_revisionStr, inq.productRev, sizeof(inq.productRev));

    if (m_vendorStr[0] == '\0')  str::copy(m_vendorStr, "USB", sizeof(m_vendorStr));
    if (m_productStr[0] == '\0') str::copy(m_productStr, "Flash Disk", sizeof(m_productStr));
    return Status::Ok;
}

Status UsbMscDevice::scsiRequestSense() noexcept {
    u8 cdb[6]{};
    cdb[0] = SCSI_CMD_REQUEST_SENSE;
    cdb[4] = 18; // Standard 18-byte fixed sense data

    u8 senseBuf[18]{};
    return executeScsiCommand(cdb, sizeof(cdb), senseBuf, sizeof(senseBuf), true);
}

Status UsbMscDevice::scsiWaitReady() noexcept {
    u8 turCdb[6]{};
    turCdb[0] = SCSI_CMD_TEST_UNIT_READY;

    for (u32 attempt = 0; attempt < 10; ++attempt) {
        if (executeScsiCommand(turCdb, sizeof(turCdb), nullptr, 0, false) == Status::Ok) {
            return Status::Ok;
        }
        // Clear UNIT ATTENTION / NOT READY sense condition and wait 50 ms
        (void)scsiRequestSense();
        arch::ApicController::instance().sleepMs(50);
    }
    return Status::Timeout;
}

Status UsbMscDevice::scsiReadCapacity() noexcept {
    u8 cdb10[10]{};
    cdb10[0] = SCSI_CMD_READ_CAPACITY10;

    u32 cap10[2]{};
    Status st = executeScsiCommand(cdb10, sizeof(cdb10), cap10, sizeof(cap10), true);
    if (st != Status::Ok) {
        // Retry once after clearing any residual sense
        (void)scsiRequestSense();
        st = executeScsiCommand(cdb10, sizeof(cdb10), cap10, sizeof(cap10), true);
        if (st != Status::Ok) return st;
    }

    u32 lastLba32 = bswap32(cap10[0]);
    u32 blkSize   = bswap32(cap10[1]);

    if (lastLba32 == 0xFFFFFFFFU) {
        // >2 TiB drive: issue SCSI READ CAPACITY (16) (Opcode 0x9E, Service Action 0x10)
        u8 cdb16[16]{};
        cdb16[0]  = SCSI_CMD_SERVICE_ACT_IN;
        cdb16[1]  = 0x10U;
        cdb16[13] = 32U;

        u8 cap16[32]{};
        st = executeScsiCommand(cdb16, sizeof(cdb16), cap16, sizeof(cap16), true);
        if (st != Status::Ok) return st;

        u64 lastLba64 = bswap64(*reinterpret_cast<const u64*>(&cap16[0]));
        blkSize       = bswap32(*reinterpret_cast<const u32*>(&cap16[8]));
        m_blockCount  = lastLba64 + 1ULL;
    } else {
        m_blockCount = static_cast<u64>(lastLba32) + 1ULL;
    }

    m_blockSize = (blkSize >= 512U && blkSize <= 4096U) ? blkSize : 512U;
    return Status::Ok;
}

Status UsbMscDevice::rwChunkBlocks(u64 lba, u32 blockCount, void* buffer, bool isWrite) noexcept {
    u32 transferBytes = blockCount * m_blockSize;
    if (!m_ready || !buffer || blockCount == 0 || transferBytes > PAGE_SIZE_4K) {
        return Status::InvalidArgument;
    }

    IrqLockGuard guard(m_lock);

    u8 cdb[16]{};
    u8 cdbLen = 10;

    if (lba <= 0xFFFFFFFFULL) {
        cdb[0] = isWrite ? SCSI_CMD_WRITE10 : SCSI_CMD_READ10;
        u32 lbaBe = bswap32(static_cast<u32>(lba));
        u16 cntBe = bswap16(static_cast<u16>(blockCount));
        memcpy(&cdb[2], &lbaBe, 4);
        memcpy(&cdb[7], &cntBe, 2);
        cdbLen = 10;
    } else {
        cdb[0] = isWrite ? SCSI_CMD_WRITE16 : SCSI_CMD_READ16;
        u64 lbaBe = bswap64(lba);
        u32 cntBe = bswap32(blockCount);
        memcpy(&cdb[2], &lbaBe, 8);
        memcpy(&cdb[10], &cntBe, 4);
        cdbLen = 16;
    }

    Status st = executeScsiCommand(cdb, cdbLen, buffer, transferBytes, !isWrite);
    if (st != Status::Ok) {
        // Clear any transient UNIT ATTENTION sense condition and retry once
        (void)scsiRequestSense();
        st = executeScsiCommand(cdb, cdbLen, buffer, transferBytes, !isWrite);
    }
    return st;
}

Status UsbMscDevice::readBlocks(u64 lba, u32 count, void* buffer) {
    if (!m_ready || !buffer) return Status::InvalidArgument;
    if (count == 0) return Status::Ok;
    if (lba + count > m_blockCount) return Status::InvalidArgument;

    u32 maxBlocksPerCmd = static_cast<u32>(PAGE_SIZE_4K / m_blockSize);
    if (maxBlocksPerCmd == 0) maxBlocksPerCmd = 1;

    auto* dst = static_cast<u8*>(buffer);
    u32 done = 0;
    while (done < count) {
        u32 chunk = count - done;
        if (chunk > maxBlocksPerCmd) chunk = maxBlocksPerCmd;

        Status st = rwChunkBlocks(lba + done, chunk, dst + static_cast<usize>(done) * m_blockSize, false);
        if (st != Status::Ok) return st;
        done += chunk;
    }
    return Status::Ok;
}

Status UsbMscDevice::writeBlocks(u64 lba, u32 count, const void* buffer) {
    if (!m_ready || !buffer) return Status::InvalidArgument;
    if (count == 0) return Status::Ok;
    if (lba + count > m_blockCount) return Status::InvalidArgument;

    u32 maxBlocksPerCmd = static_cast<u32>(PAGE_SIZE_4K / m_blockSize);
    if (maxBlocksPerCmd == 0) maxBlocksPerCmd = 1;

    const auto* src = static_cast<const u8*>(buffer);
    u32 done = 0;
    while (done < count) {
        u32 chunk = count - done;
        if (chunk > maxBlocksPerCmd) chunk = maxBlocksPerCmd;

        Status st = rwChunkBlocks(lba + done,
                                  chunk,
                                  const_cast<u8*>(src + static_cast<usize>(done) * m_blockSize),
                                  true);
        if (st != Status::Ok) return st;
        done += chunk;
    }
    return Status::Ok;
}

Status UsbMscDevice::flush() {
    if (!m_ready) return Status::DeviceNotFound;
    // USB BOT WRITE(10)/WRITE(16) transfers are synchronous uncached block writes;
    // skipping SCSI SYNCHRONIZE CACHE (0x35) avoids firmware hangs on USB flash drives.
    return Status::Ok;
}

} // namespace cortez::drivers::usb
