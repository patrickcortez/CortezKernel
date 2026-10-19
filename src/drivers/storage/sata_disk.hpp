/**
 * @file sata_disk.hpp
 * @brief SATA Disk Block Device driver implementing IBlockDevice for AHCI-connected drives.
 */

#pragma once

#include "../i_device.hpp"
#include "ahci_port.hpp"

namespace cortez::drivers::storage {

class SataDisk final : public IBlockDevice {
public:
    SataDisk() noexcept = default;
    ~SataDisk() override = default;

    Status init(AhciPort* port, u32 diskIndex) noexcept;

    const char*    getName() const noexcept override { return m_name; }
    StorageBusType getBusType() const noexcept override { return StorageBusType::SATA; }
    u32            getBlockSize() const noexcept override { return m_blockSize; }
    u64            getBlockCount() const noexcept override { return m_blockCount; }

    Status readBlocks(u64 lba, u32 count, void* buffer) override;
    Status writeBlocks(u64 lba, u32 count, const void* buffer) override;
    Status flush() override;

    const char* getModel() const noexcept { return m_model; }
    const char* getSerial() const noexcept { return m_serial; }
    const char* getFirmware() const noexcept { return m_firmware; }
    AhciPort*   getPort() const noexcept { return m_port; }

private:
    Status identify() noexcept;

    AhciPort* m_port       = nullptr;
    u32       m_diskIndex  = 0;
    u32       m_blockSize  = 512;
    u64       m_blockCount = 0;
    bool      m_lba48      = true;

    char      m_name[16]{};
    char      m_model[44]{};
    char      m_serial[24]{};
    char      m_firmware[12]{};
};

} // namespace cortez::drivers::storage
