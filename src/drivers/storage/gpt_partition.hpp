/**
 * @file gpt_partition.hpp
 * @brief GUID Partition Table (GPT) scanner and partition block-device slice wrapper.
 */

#pragma once

#include "../i_device.hpp"

namespace cortez::drivers::storage {

struct CORTEZ_PACKED GptHeader {
    char signature[8]; // "EFI PART"
    u32  revision;
    u32  headerSize;
    u32  headerCrc32;
    u32  reserved;
    u64  myLba;
    u64  alternateLba;
    u64  firstUsableLba;
    u64  lastUsableLba;
    u8   diskGuid[16];
    u64  partitionEntryLba;
    u32  numPartitionEntries;
    u32  sizeOfPartitionEntry;
    u32  partitionEntryArrayCrc32;
};

struct CORTEZ_PACKED GptPartitionEntry {
    u8  typeGuid[16];
    u8  uniqueGuid[16];
    u64 startingLba;
    u64 endingLba;
    u64 attributes;
    u16 partitionName[36]; // UTF-16LE
};

/**
 * @brief Logical block device slice representing a single GPT partition (e.g., nvme0n1p2).
 */
class GptPartitionDevice final : public IBlockDevice {
public:
    GptPartitionDevice() noexcept = default;

    void init(IBlockDevice* parentDisk,
              u32 partIndex,
              u64 startLba,
              u64 endLba,
              const char* label) noexcept;

    const char* getName() const noexcept override { return m_name; }
    u32         getBlockSize() const noexcept override { return m_parent ? m_parent->getBlockSize() : 512; }
    u64         getBlockCount() const noexcept override { return m_blockCount; }

    Status readBlocks(u64 lba, u32 count, void* buffer) override;
    Status writeBlocks(u64 lba, u32 count, const void* buffer) override;
    Status flush() override;

    u64         getStartLba() const noexcept { return m_startLba; }
    const char* getLabel() const noexcept override { return m_label; }

private:
    IBlockDevice* m_parent     = nullptr;
    u64           m_startLba   = 0;
    u64           m_blockCount = 0;
    char          m_name[32]{};
    char          m_label[40]{};
};

class GptPartitionScanner final {
public:
    static constexpr usize MAX_PARTITIONS = 32;

    static GptPartitionScanner& instance() noexcept;

    Status scanDisk(IBlockDevice* disk) noexcept;

    usize                     getPartitionCount() const noexcept { return m_partCount; }
    GptPartitionDevice*       getPartition(usize idx) noexcept {
        return (idx < m_partCount) ? &m_partitions[idx] : nullptr;
    }

private:
    GptPartitionScanner() noexcept = default;

    GptPartitionDevice m_partitions[MAX_PARTITIONS]{};
    usize              m_partCount = 0;
};

} // namespace cortez::drivers::storage
