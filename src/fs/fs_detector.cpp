/**
 * @file fs_detector.cpp
 * @brief Implementation of filesystem auto-detection.
 */

#include "fs_detector.hpp"
#include "../lib/string_utils.hpp"
#include "../lib/printk.hpp"

namespace cortez::fs {

const char* FsDetector::typeToString(DetectedFsType type) noexcept {
    switch (type) {
        case DetectedFsType::Fat12: return "FAT12";
        case DetectedFsType::Fat16: return "FAT16";
        case DetectedFsType::Fat32: return "FAT32";
        case DetectedFsType::ExFat: return "exFAT";
        case DetectedFsType::Ext2:  return "ext2/3/4";
        default:                    return "Unknown";
    }
}

FsProbeInfo FsDetector::probe(drivers::IBlockDevice* dev) noexcept {
    FsProbeInfo info{};
    if (!dev) return info;

    CORTEZ_ALIGNED(16) u8 sector0[4096]{};
    Status st = dev->readBlocks(0, 1, sector0);
    if (st != Status::Ok) return info;

    // Check standard 0xAA55 boot signature at byte 510..511
    u16 bootSig = *reinterpret_cast<const u16*>(sector0 + 510);
    if (bootSig == 0xAA55U) {
        // 1. Check for exFAT ("EXFAT   " at byte 3)
        if (str::compareN(reinterpret_cast<const char*>(sector0 + 3), "EXFAT   ", 8) == 0) {
            info.type = DetectedFsType::ExFat;
            str::copy(info.fsName, "exfat", sizeof(info.fsName));
            u8 bpsShift = sector0[108];
            u8 spcShift = sector0[109];
            info.bytesPerSector    = (bpsShift >= 9 && bpsShift <= 12) ? (1U << bpsShift) : 512;
            info.sectorsPerCluster = (spcShift <= 25) ? (1U << spcShift) : 1;
            info.totalSectors      = *reinterpret_cast<const u64*>(sector0 + 72);
            return info;
        }

        // 2. Check for FAT32 / FAT16 / FAT12 BPB parameters
        u16 bytesPerSector    = *reinterpret_cast<const u16*>(sector0 + 11);
        u8  sectorsPerCluster = sector0[13];
        u16 reservedSectors   = *reinterpret_cast<const u16*>(sector0 + 14);
        u8  numFats           = sector0[16];
        u16 rootEntries       = *reinterpret_cast<const u16*>(sector0 + 17);
        u16 totalSectors16    = *reinterpret_cast<const u16*>(sector0 + 19);
        u16 fatSize16         = *reinterpret_cast<const u16*>(sector0 + 22);
        u32 totalSectors32    = *reinterpret_cast<const u32*>(sector0 + 32);

        bool validSectorSize = (bytesPerSector == 512 || bytesPerSector == 1024 ||
                                bytesPerSector == 2048 || bytesPerSector == 4096);
        bool validSpc        = (sectorsPerCluster != 0) && ((sectorsPerCluster & (sectorsPerCluster - 1)) == 0);
        bool validFats       = (numFats >= 1 && numFats <= 2);

        if (validSectorSize && validSpc && validFats && reservedSectors > 0) {
            u64 totalSectors = (totalSectors16 != 0) ? totalSectors16 : totalSectors32;
            u32 fatSize32    = *reinterpret_cast<const u32*>(sector0 + 36);
            u32 rootCluster  = *reinterpret_cast<const u32*>(sector0 + 44);

            // FAT32: fatSize16 == 0 and fatSize32 > 0 and rootCluster >= 2
            if (fatSize16 == 0 && fatSize32 > 0 && rootCluster >= 2) {
                info.type              = DetectedFsType::Fat32;
                info.bytesPerSector    = bytesPerSector;
                info.sectorsPerCluster = sectorsPerCluster;
                info.totalSectors      = totalSectors;
                str::copy(info.fsName, "fat32", sizeof(info.fsName));

                // Volume label in EBPB at offset 71 (11 bytes)
                char label[12]{};
                for (usize i = 0; i < 11; ++i) {
                    label[i] = static_cast<char>(sector0[71 + i]);
                }
                label[11] = '\0';
                // Trim trailing spaces
                for (int i = 10; i >= 0 && label[i] == ' '; --i) {
                    label[i] = '\0';
                }
                if (label[0] && str::compare(label, "NO NAME    ") != 0) {
                    str::copy(info.volumeLabel, label, sizeof(info.volumeLabel));
                }
                return info;
            }

            // FAT16 / FAT12: fatSize16 > 0 and rootEntries > 0
            if (fatSize16 > 0 && rootEntries > 0) {
                u32 rootDirSectors = ((rootEntries * 32U) + (bytesPerSector - 1U)) / bytesPerSector;
                u32 dataSectors    = static_cast<u32>(totalSectors) - (reservedSectors + (numFats * fatSize16) + rootDirSectors);
                u32 totalClusters  = dataSectors / sectorsPerCluster;

                info.type              = (totalClusters < 4085) ? DetectedFsType::Fat12 : DetectedFsType::Fat16;
                info.bytesPerSector    = bytesPerSector;
                info.sectorsPerCluster = sectorsPerCluster;
                info.totalSectors      = totalSectors;
                str::copy(info.fsName, (info.type == DetectedFsType::Fat12) ? "fat12" : "fat16", sizeof(info.fsName));

                // Volume label in EBPB at offset 43 (11 bytes)
                char label[12]{};
                for (usize i = 0; i < 11; ++i) {
                    label[i] = static_cast<char>(sector0[43 + i]);
                }
                label[11] = '\0';
                for (int i = 10; i >= 0 && label[i] == ' '; --i) {
                    label[i] = '\0';
                }
                if (label[0] && str::compare(label, "NO NAME    ") != 0) {
                    str::copy(info.volumeLabel, label, sizeof(info.volumeLabel));
                }
                return info;
            }
        }
    }

    // 3. Check for ext2/3/4 superblock at offset 1024 (LBA 2 for 512B sectors)
    CORTEZ_ALIGNED(16) u8 extBuf[1024]{};
    if (dev->readBlocks(2, 2, extBuf) == Status::Ok) {
        u16 extMagic = *reinterpret_cast<const u16*>(extBuf + 56); // 1024 + 56 = 1080
        if (extMagic == 0xEF53U) {
            info.type = DetectedFsType::Ext2;
            str::copy(info.fsName, "ext2", sizeof(info.fsName));
            info.bytesPerSector = dev->getBlockSize();
            return info;
        }
    }

    return info;
}

} // namespace cortez::fs
