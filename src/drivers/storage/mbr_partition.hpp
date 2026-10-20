/**
 * @file mbr_partition.hpp
 * @brief Master Boot Record (MBR) partition scanner and partition block-device slice wrapper.
 */

#pragma once

#include "../i_device.hpp"

namespace cortez::drivers::storage {

struct CORTEZ_PACKED MbrPartitionEntry {
    u8  status;         // 0x80 = bootable, 0x00 = inactive
    u8  startChs[3];
    u8  partitionType;  // 0x0B/0x0C = FAT32, 0x07 = exFAT/NTFS, 0x04/0x06/0x0E = FAT16, 0x83 = Linux
    u8  endChs[3];
    u32 startLba;
    u32 sectorCount;
};

struct CORTEZ_PACKED MbrSector {
    u8                bootCode[446];
    MbrPartitionEntry entries[4];
    u16               signature; // 0xAA55
};

/**
 * @brief Logical block device slice representing a single MBR partition (e.g., usb0p1).
 */
class MbrPartitionDevice final : public IBlockDevice {
public:
    MbrPartitionDevice() noexcept = default;

    void init(IBlockDevice* parentDisk,
              u32 partIndex,
              u64 startLba,
              u64 blockCount,
              u8 partitionType,
              const char* label) noexcept;

    const char*    getName() const noexcept override { return m_name; }
    StorageBusType getBusType() const noexcept override { return m_parent ? m_parent->getBusType() : StorageBusType::Unknown; }
    u32            getBlockSize() const noexcept override { return m_parent ? m_parent->getBlockSize() : 512; }
    u64            getBlockCount() const noexcept override { return m_blockCount; }

    Status readBlocks(u64 lba, u32 count, void* buffer) override;
    Status writeBlocks(u64 lba, u32 count, const void* buffer) override;
    Status flush() override;

    u64           getStartLba() const noexcept { return m_startLba; }
    u8            getPartitionType() const noexcept { return m_partType; }
    const char*   getLabel() const noexcept override { return m_label; }
    IBlockDevice* getParentDisk() const noexcept { return m_parent; }

private:
    IBlockDevice* m_parent     = nullptr;
    u64           m_startLba   = 0;
    u64           m_blockCount = 0;
    u8            m_partType   = 0;
    char          m_name[32]{};
    char          m_label[40]{};
};

class MbrPartitionScanner final {
public:
    static constexpr usize MAX_PARTITIONS = 32;

    static MbrPartitionScanner& instance() noexcept;

    Status scanDisk(IBlockDevice* disk) noexcept;
    void   unregisterDiskPartitions(IBlockDevice* parentDisk) noexcept;

    usize               getPartitionCount() const noexcept { return m_partCount; }
    MbrPartitionDevice* getPartition(usize idx) noexcept {
        return (idx < m_partCount) ? &m_partitions[idx] : nullptr;
    }

private:
    MbrPartitionScanner() noexcept = default;

    MbrPartitionDevice m_partitions[MAX_PARTITIONS]{};
    usize              m_partCount = 0;
};

} // namespace cortez::drivers::storage
