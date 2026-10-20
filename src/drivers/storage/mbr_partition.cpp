/**
 * @file mbr_partition.cpp
 * @brief Implementation of Master Boot Record (MBR) partition scanner and device slices.
 */

#include "mbr_partition.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::storage {

namespace {
const char* mbrTypeToString(u8 type) noexcept {
    switch (type) {
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 (<32M)";
        case 0x06: return "FAT16";
        case 0x07: return "exFAT/NTFS";
        case 0x0B: return "FAT32 (CHS)";
        case 0x0C: return "FAT32 (LBA)";
        case 0x0E: return "FAT16 (LBA)";
        case 0x0F: return "Extended (LBA)";
        case 0x82: return "Linux Swap";
        case 0x83: return "Linux Native";
        default:   return "Unknown";
    }
}
} // namespace

void MbrPartitionDevice::init(IBlockDevice* parentDisk,
                              u32 partIndex,
                              u64 startLba,
                              u64 blockCount,
                              u8 partitionType,
                              const char* label) noexcept {
    m_parent     = parentDisk;
    m_startLba   = startLba;
    m_blockCount = blockCount;
    m_partType   = partitionType;

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

Status MbrPartitionDevice::readBlocks(u64 lba, u32 count, void* buffer) {
    if (!m_parent || !buffer) return Status::InvalidArgument;
    if (lba + count > m_blockCount) return Status::InvalidArgument;
    return m_parent->readBlocks(m_startLba + lba, count, buffer);
}

Status MbrPartitionDevice::writeBlocks(u64 lba, u32 count, const void* buffer) {
    if (!m_parent || !buffer) return Status::InvalidArgument;
    if (lba + count > m_blockCount) return Status::InvalidArgument;
    return m_parent->writeBlocks(m_startLba + lba, count, buffer);
}

Status MbrPartitionDevice::flush() {
    return m_parent ? m_parent->flush() : Status::DeviceNotFound;
}

MbrPartitionScanner& MbrPartitionScanner::instance() noexcept {
    static MbrPartitionScanner s_scanner;
    return s_scanner;
}

Status MbrPartitionScanner::scanDisk(IBlockDevice* disk) noexcept {
    if (!disk) return Status::InvalidArgument;

    u32 blockSize = disk->getBlockSize();
    if (blockSize < 512 || blockSize > 4096) return Status::NotSupported;

    CORTEZ_ALIGNED(16) u8 sectorBuf[4096]{};
    // Read LBA 0 (MBR Sector)
    Status st = disk->readBlocks(0, 1, sectorBuf);
    if (st != Status::Ok) return st;

    const auto* mbr = reinterpret_cast<const MbrSector*>(sectorBuf);
    if (mbr->signature != 0xAA55U) {
        return Status::NotFound;
    }

    // Protective MBR check for GPT disks (Type 0xEE in first partition entry)
    if (mbr->entries[0].partitionType == 0xEEU) {
        return Status::NotFound;
    }

    u32 foundCount = 0;
    for (u32 i = 0; i < 4 && m_partCount < MAX_PARTITIONS; ++i) {
        const auto& entry = mbr->entries[i];
        if (entry.partitionType != 0 && entry.sectorCount > 0 && entry.startLba > 0) {
            u32 partNumber = i + 1;
            MbrPartitionDevice& part = m_partitions[m_partCount++];
            part.init(disk, partNumber, entry.startLba, entry.sectorCount, entry.partitionType, "");
            DeviceRegistry::instance().registerBlockDevice(&part);

            u64 sizeMiB = (part.getBlockCount() * blockSize) / (1024ULL * 1024ULL);
            pr_dbg("mbr", "/dev/%s: LBA %u..%u (%llu MiB) Type=0x%02x [%s]%s",
                  part.getName(),
                  entry.startLba,
                  entry.startLba + entry.sectorCount - 1,
                  sizeMiB,
                  entry.partitionType,
                  mbrTypeToString(entry.partitionType),
                  (entry.status & 0x80U) ? " (bootable)" : "");
            ++foundCount;
        }
    }

    return (foundCount > 0) ? Status::Ok : Status::NotFound;
}

void MbrPartitionScanner::unregisterDiskPartitions(IBlockDevice* parentDisk) noexcept {
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
