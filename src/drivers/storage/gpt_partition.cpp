/**
 * @file gpt_partition.cpp
 * @brief Implementation of GUID Partition Table (GPT) discovery and partition block devices.
 */

#include "gpt_partition.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::storage {

namespace {
bool isGuidZero(const u8 guid[16]) noexcept {
    for (usize i = 0; i < 16; ++i) {
        if (guid[i] != 0) return false;
    }
    return true;
}

void utf16ToAscii(char* dst, usize maxDst, const u16* src, usize maxChars) noexcept {
    usize pos = 0;
    for (usize i = 0; i < maxChars && pos + 1 < maxDst; ++i) {
        u16 ch = src[i];
        if (ch == 0) break;
        dst[pos++] = (ch < 128) ? static_cast<char>(ch) : '?';
    }
    dst[pos] = '\0';
}
} // namespace

void GptPartitionDevice::init(IBlockDevice* parentDisk,
                              u32 partIndex,
                              u64 startLba,
                              u64 endLba,
                              const char* label) noexcept {
    m_parent     = parentDisk;
    m_startLba   = startLba;
    m_blockCount = (endLba >= startLba) ? (endLba - startLba + 1ULL) : 0ULL;

    str::copy(m_name, parentDisk->getName(), sizeof(m_name));
    usize nameLen = str::length(m_name);
    if (nameLen > 0 && m_name[nameLen - 1] >= '0' && m_name[nameLen - 1] <= '9') {
        str::concat(m_name, "p", sizeof(m_name));
    }
    char numBuf[12];
    str::uintToStr(partIndex, numBuf, 10, 0);
    str::concat(m_name, numBuf, sizeof(m_name));

    str::copy(m_label, label ? label : "", sizeof(m_label));
}

Status GptPartitionDevice::readBlocks(u64 lba, u32 count, void* buffer) {
    if (!m_parent || !buffer) return Status::InvalidArgument;
    if (lba + count > m_blockCount) return Status::InvalidArgument;
    return m_parent->readBlocks(m_startLba + lba, count, buffer);
}

Status GptPartitionDevice::writeBlocks(u64 lba, u32 count, const void* buffer) {
    if (!m_parent || !buffer) return Status::InvalidArgument;
    if (lba + count > m_blockCount) return Status::InvalidArgument;
    return m_parent->writeBlocks(m_startLba + lba, count, buffer);
}

Status GptPartitionDevice::flush() {
    return m_parent ? m_parent->flush() : Status::DeviceNotFound;
}

GptPartitionScanner& GptPartitionScanner::instance() noexcept {
    static GptPartitionScanner s_scanner;
    return s_scanner;
}

Status GptPartitionScanner::scanDisk(IBlockDevice* disk) noexcept {
    if (!disk) return Status::InvalidArgument;

    u32 blockSize = disk->getBlockSize();
    if (blockSize < 512 || blockSize > 4096) return Status::NotSupported;

    CORTEZ_ALIGNED(16) u8 sectorBuf[4096]{};
    // Read LBA 1 (Primary GPT Header)
    Status st = disk->readBlocks(1, 1, sectorBuf);
    if (st != Status::Ok) return st;

    const auto* hdr = reinterpret_cast<const GptHeader*>(sectorBuf);
    if (str::compareN(hdr->signature, "EFI PART", 8) != 0) {
        pr_dbg("gpt", "No valid GPT signature ('EFI PART') on /dev/%s LBA 1", disk->getName());
        return Status::NotFound;
    }

    u64 entryLba   = hdr->partitionEntryLba;
    u32 numEntries = hdr->numPartitionEntries;
    u32 entrySize  = hdr->sizeOfPartitionEntry;
    if (entrySize < sizeof(GptPartitionEntry) || entrySize > 512) {
        return Status::CorruptedFs;
    }

    u32 entriesPerBlock = blockSize / entrySize;
    if (entriesPerBlock == 0) return Status::CorruptedFs;

    u32 blocksToScan = (numEntries + entriesPerBlock - 1) / entriesPerBlock;
    if (blocksToScan > 32) blocksToScan = 32;

    u32 partNumber = 1;
    for (u32 blk = 0; blk < blocksToScan && m_partCount < MAX_PARTITIONS; ++blk) {
        st = disk->readBlocks(entryLba + blk, 1, sectorBuf);
        if (st != Status::Ok) break;

        for (u32 i = 0; i < entriesPerBlock && m_partCount < MAX_PARTITIONS; ++i) {
            const auto* entry = reinterpret_cast<const GptPartitionEntry*>(sectorBuf + i * entrySize);
            if (!isGuidZero(entry->typeGuid) && entry->endingLba >= entry->startingLba && entry->startingLba > 0) {
                char label[40]{};
                utf16ToAscii(label, sizeof(label), entry->partitionName, 36);

                GptPartitionDevice& part = m_partitions[m_partCount++];
                part.init(disk, partNumber, entry->startingLba, entry->endingLba, label);
                DeviceRegistry::instance().registerBlockDevice(&part);

                u64 sizeMiB = (part.getBlockCount() * blockSize) / (1024ULL * 1024ULL);
                pr_dbg("gpt", " /dev/%s: LBA %llu..%llu (%llu MiB) Label='%s'",
                      part.getName(), entry->startingLba, entry->endingLba, sizeMiB, label);
            }
            ++partNumber;
        }
    }

    return (m_partCount > 0) ? Status::Ok : Status::NotFound;
}

void GptPartitionScanner::unregisterDiskPartitions(IBlockDevice* parentDisk) noexcept {
    if (!parentDisk) return;
    for (usize i = 0; i < m_partCount; ) {
        if (m_partitions[i].getParentDisk() == parentDisk) {
            DeviceRegistry::instance().unregisterBlockDevice(&m_partitions[i]);
            for (usize j = i; j + 1 < m_partCount; ++j) {
                m_partitions[j] = m_partitions[j + 1];
            }
            --m_partCount;
        } else {
            ++i;
        }
    }
}

} // namespace cortez::drivers::storage
