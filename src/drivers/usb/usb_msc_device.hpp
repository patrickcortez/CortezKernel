/**
 * @file usb_msc_device.hpp
 * @brief USB Mass Storage Class (Bulk-Only Transport / SCSI) Block Device driver.
 */

#pragma once

#include "../i_device.hpp"
#include "xhci_regs.hpp"
#include "xhci_rings.hpp"
#include "../../lib/spinlock.hpp"

namespace cortez::drivers::usb {

class XhciController;

/**
 * @brief Represents a physical USB Mass Storage drive (/dev/usb0, /dev/usb1, ...)
 *        implementing IBlockDevice via USB Bulk-Only Transport (BOT) and SCSI commands.
 */
class UsbMscDevice final : public IBlockDevice {
public:
    UsbMscDevice() noexcept = default;

    Status init(XhciController* ctrl,
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
                u64 hhdmOffset) noexcept;

    const char*    getName() const noexcept override { return m_name; }
    StorageBusType getBusType() const noexcept override { return StorageBusType::USB; }
    u32            getBlockSize() const noexcept override { return m_blockSize; }
    u64            getBlockCount() const noexcept override { return m_blockCount; }

    Status readBlocks(u64 lba, u32 count, void* buffer) override;
    Status writeBlocks(u64 lba, u32 count, const void* buffer) override;
    Status flush() override;

    bool        isReady() const noexcept { return m_ready; }
    u8          getSlotId() const noexcept { return m_slotId; }
    u8          getPortNumber() const noexcept { return m_portNum; }
    u8          getSpeedId() const noexcept { return m_speedId; }
    u16         getUsbVendorId() const noexcept { return m_usbVid; }
    u16         getUsbProductId() const noexcept { return m_usbPid; }
    const char* getVendorString() const noexcept { return m_vendorStr; }
    const char* getProductString() const noexcept { return m_productStr; }
    const char* getRevisionString() const noexcept { return m_revisionStr; }

    XhciTrbRing& getBulkInRing() noexcept { return m_bulkInRing; }
    XhciTrbRing& getBulkOutRing() noexcept { return m_bulkOutRing; }

private:
    Status executeScsiCommand(const u8* cdb,
                              u8 cdbLen,
                              void* dataBuffer,
                              u32 dataLength,
                              bool isDataIn) noexcept;

    Status scsiInquiry() noexcept;
    Status scsiWaitReady() noexcept;
    Status scsiRequestSense() noexcept;
    Status scsiReadCapacity() noexcept;
    Status rwChunkBlocks(u64 lba, u32 blockCount, void* buffer, bool isWrite) noexcept;

    Spinlock        m_lock;
    XhciController* m_ctrl          = nullptr;
    u8              m_slotId        = 0;
    u8              m_portNum       = 0;
    u8              m_speedId       = 0;
    u16             m_usbVid        = 0;
    u16             m_usbPid        = 0;
    u8              m_interfaceNum  = 0;
    u8              m_bulkInDci     = 0;
    u8              m_bulkOutDci    = 0;
    u8              m_bulkInEpAddr  = 0;
    u8              m_bulkOutEpAddr = 0;

    u32             m_nextTag       = 1;
    u32             m_blockSize     = 512;
    u64             m_blockCount    = 0;
    bool            m_ready         = false;

    XhciTrbRing     m_bulkInRing{};
    XhciTrbRing     m_bulkOutRing{};

    phys_addr_t     m_dmaPagePhys   = 0;
    u8*             m_dmaPageVirt   = nullptr;

    char            m_name[16]{};
    char            m_vendorStr[12]{};
    char            m_productStr[20]{};
    char            m_revisionStr[8]{};
};

} // namespace cortez::drivers::usb
