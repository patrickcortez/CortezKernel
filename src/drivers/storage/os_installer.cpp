/**
 * @file os_installer.cpp
 * @brief Implementation of the Autonomous Cortez OS Installer.
 */

#include "os_installer.hpp"
#include "gpt_partition.hpp"
#include "mbr_partition.hpp"
#include "storage_manager.hpp"
#include "../../fs/vfs_manager.hpp"
#include "../../fs/exfat/exfat_fs.hpp"
#include "../../fs/fat/fat32_fs.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../klibc/klibc.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::storage {

namespace {

// EFI System Partition (ESP) Type GUID: c12a7328-f81f-11d2-ba4b-00a0c93ec93b
constexpr u8 GUID_ESP[16] = {
    0x28, 0x73, 0x2A, 0xC1,
    0x1F, 0xF8,
    0xD2, 0x11,
    0xBA, 0x4B,
    0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};

// Microsoft Basic Data Partition (used for RootFS) Type GUID: ebd0a0a2-b9e5-4433-87c0-68b6b72699c7
constexpr u8 GUID_BASIC_DATA[16] = {
    0xA2, 0xA0, 0xD0, 0xEB,
    0xE5, 0xB9,
    0x33, 0x44,
    0x87, 0xC0,
    0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

bool isGuidEqual(const u8 a[16], const u8 b[16]) noexcept {
    for (usize i = 0; i < 16; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

bool isGuidEmpty(const u8 guid[16]) noexcept {
    for (usize i = 0; i < 16; ++i) {
        if (guid[i] != 0) return false;
    }
    return true;
}

u64 alignUp(u64 val, u64 alignment) noexcept {
    if (alignment == 0) return val;
    return (val + alignment - 1ULL) & ~(alignment - 1ULL);
}

bool isRawDisk(IBlockDevice* dev) noexcept {
    if (!dev) return false;
    const char* name = dev->getName();
    usize len = str::length(name);
    if (len == 0) return false;

    // Reject partition devices registered in GptPartitionScanner or MbrPartitionScanner
    for (usize i = 0; i < GptPartitionScanner::instance().getPartitionCount(); ++i) {
        if (GptPartitionScanner::instance().getPartition(i) == dev) return false;
    }
    for (usize i = 0; i < MbrPartitionScanner::instance().getPartitionCount(); ++i) {
        if (MbrPartitionScanner::instance().getPartition(i) == dev) return false;
    }

    if (str::compareN(name, "nvme", 4) == 0) {
        const char* p = str::findChar(name, 'p');
        if (p && p > name + 4 && p[1] >= '0' && p[1] <= '9') return false;
        return true;
    }
    if (str::compareN(name, "sd", 2) == 0) {
        if (name[len - 1] >= '0' && name[len - 1] <= '9') return false;
        return true;
    }
    if (str::compareN(name, "usb", 3) == 0) {
        const char* p = str::findChar(name, 'p');
        if (p && p[1] >= '0' && p[1] <= '9') return false;
        return true;
    }
    return false;
}

bool isLiveBootDisk(IBlockDevice* disk) noexcept {
    if (!disk) return false;
    IBlockDevice* liveRoot = fs::VfsManager::instance().getRootBlockDevice();
    if (!liveRoot) return false;
    if (liveRoot == disk) return true;

    const char* rootName = liveRoot->getName();
    const char* diskName = disk->getName();
    usize dLen = str::length(diskName);
    if (str::compareN(rootName, diskName, dLen) == 0) {
        return true;
    }
    return false;
}

Status copyFile(const char* srcPath, const char* dstPath) noexcept {
    auto srcRes = fs::VfsManager::instance().open("/", srcPath, 0); // O_RDONLY = 0
    if (srcRes.isError()) return srcRes.status();
    i32 srcFd = srcRes.value();

    auto dstRes = fs::VfsManager::instance().open("/", dstPath, 0x01 | 0x0200 | 0x0400); // O_WRONLY | O_CREAT | O_TRUNC
    if (dstRes.isError()) {
        fs::VfsManager::instance().close(srcFd);
        return dstRes.status();
    }
    i32 dstFd = dstRes.value();

    constexpr usize CHUNK_SIZE = 4096;
    u8* buf = new u8[CHUNK_SIZE];
    if (!buf) {
        fs::VfsManager::instance().close(srcFd);
        fs::VfsManager::instance().close(dstFd);
        return Status::OutOfMemory;
    }

    Status st = Status::Ok;
    for (;;) {
        auto rRes = fs::VfsManager::instance().read(srcFd, buf, CHUNK_SIZE);
        if (rRes.isError()) {
            st = rRes.status();
            break;
        }
        usize bytesRead = rRes.value();
        if (bytesRead == 0) break; // EOF

        auto wRes = fs::VfsManager::instance().write(dstFd, buf, bytesRead);
        if (wRes.isError()) {
            st = wRes.status();
            break;
        }
    }

    delete[] buf;
    fs::VfsManager::instance().close(srcFd);
    fs::VfsManager::instance().close(dstFd);
    return st;
}

Status copyDirRecursive(const char* srcDir, const char* dstDir) noexcept {
    (void)fs::VfsManager::instance().mkdir("/", dstDir);

    for (usize idx = 0; idx < 128; ++idx) {
        fs::DirEntry entry{};
        Status st = fs::VfsManager::instance().readDir("/", srcDir, idx, &entry);
        if (st != Status::Ok) break;

        char subSrc[128]{};
        char subDst[128]{};
        klibc_snprintf(subSrc, sizeof(subSrc), "%s/%s", srcDir, entry.name);
        klibc_snprintf(subDst, sizeof(subDst), "%s/%s", dstDir, entry.name);

        if (entry.type == fs::NodeType::Directory) {
            copyDirRecursive(subSrc, subDst);
        } else {
            copyFile(subSrc, subDst);
        }
    }
    return Status::Ok;
}

} // namespace

OsInstaller& OsInstaller::instance() noexcept {
    static OsInstaller s_installer;
    return s_installer;
}

u32 OsInstaller::calculateCrc32(const void* data, usize length) noexcept {
    const auto* bytes = static_cast<const u8*>(data);
    u32 crc = 0xFFFFFFFFU;
    for (usize i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (u32 b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320U & (-(crc & 1U)));
        }
    }
    return ~crc;
}

void OsInstaller::generateGuid(u8 outGuid[16]) noexcept {
    u64 tsc = arch::rdtsc();
    u64 uptime = arch::ApicController::instance().getUptimeMs();
    memcpy(outGuid, &tsc, 8);
    memcpy(outGuid + 8, &uptime, 8);
    outGuid[6] = static_cast<u8>((outGuid[6] & 0x0FU) | 0x40U); // UUID Version 4
    outGuid[8] = static_cast<u8>((outGuid[8] & 0x3FU) | 0x80U); // RFC 4122 Variant
}

void OsInstaller::asciiToUtf16(u16 outName[36], const char* ascii) noexcept {
    memset(outName, 0, 36 * sizeof(u16));
    if (!ascii) return;
    for (usize i = 0; i < 35 && ascii[i]; ++i) {
        outName[i] = static_cast<u16>(static_cast<u8>(ascii[i]));
    }
}

Status OsInstaller::scanCandidateDisks(DiskInstallInfo* outDisks,
                                       usize maxDisks,
                                       usize* outCount) noexcept {
    if (!outDisks || maxDisks == 0) return Status::InvalidArgument;

    usize count = 0;
    auto& reg = DeviceRegistry::instance();

    for (usize i = 0; i < reg.getBlockDeviceCount() && count < maxDisks; ++i) {
        auto* dev = reg.getBlockDeviceAt(i);
        if (!dev) continue;

        // Target must be a whole disk (not a partition slice)
        if (!isRawDisk(dev)) continue;

        // Exclude the live boot disk from installation targets
        if (isLiveBootDisk(dev)) continue;

        DiskInstallInfo info{};
        if (findUnallocatedSpace(dev, &info) == Status::Ok) {
            outDisks[count++] = info;
        }
    }

    if (outCount) *outCount = count;
    return Status::Ok;
}

Status OsInstaller::findUnallocatedSpace(IBlockDevice* disk, DiskInstallInfo* outInfo) noexcept {
    if (!disk || !outInfo) return Status::InvalidArgument;

    outInfo->disk               = disk;
    str::copy(outInfo->name, disk->getName(), sizeof(outInfo->name));
    outInfo->totalSectors       = disk->getBlockCount();
    outInfo->blockSize          = disk->getBlockSize();
    outInfo->totalSizeMiB       = (outInfo->totalSectors * outInfo->blockSize) / (1024ULL * 1024ULL);
    outInfo->isGpt              = false;
    outInfo->hasEsp             = false;
    outInfo->espPartName[0]     = '\0';
    outInfo->existingPartCount  = 0;
    outInfo->largestFree        = {};
    outInfo->hasEnoughSpace     = false;

    if (outInfo->totalSectors < 4096) {
        return Status::NoSpace;
    }

    CORTEZ_ALIGNED(16) u8 sectorBuf[4096]{};
    Status st = disk->readBlocks(1, 1, sectorBuf);
    if (st != Status::Ok) return st;

    const auto* hdr = reinterpret_cast<const GptHeader*>(sectorBuf);
    if (str::compareN(hdr->signature, "EFI PART", 8) == 0 && hdr->headerSize >= 92) {
        outInfo->isGpt = true;
        u64 firstUsable = hdr->firstUsableLba;
        u64 lastUsable  = hdr->lastUsableLba;
        u64 entryLba    = hdr->partitionEntryLba;
        u32 numEntries  = hdr->numPartitionEntries;
        u32 entrySize   = hdr->sizeOfPartitionEntry;

        u32 entriesPerBlock = outInfo->blockSize / entrySize;
        u32 blocksToRead    = (numEntries + entriesPerBlock - 1) / entriesPerBlock;
        if (blocksToRead > 32) blocksToRead = 32;

        struct Span {
            u64 start;
            u64 end;
        } spans[64]{};
        usize spanCount = 0;

        for (u32 b = 0; b < blocksToRead && spanCount < 64; ++b) {
            st = disk->readBlocks(entryLba + b, 1, sectorBuf);
            if (st != Status::Ok) break;

            for (u32 i = 0; i < entriesPerBlock && spanCount < 64; ++i) {
                const auto* entry = reinterpret_cast<const GptPartitionEntry*>(sectorBuf + i * entrySize);
                if (!isGuidEmpty(entry->typeGuid) && entry->startingLba > 0 && entry->endingLba >= entry->startingLba) {
                    spans[spanCount].start = entry->startingLba;
                    spans[spanCount].end   = entry->endingLba;
                    ++spanCount;
                    ++outInfo->existingPartCount;

                    if (isGuidEqual(entry->typeGuid, GUID_ESP)) {
                        outInfo->hasEsp = true;
                        if (str::compareN(outInfo->name, "nvme", 4) == 0 || str::compareN(outInfo->name, "usb", 3) == 0) {
                            klibc_snprintf(outInfo->espPartName, sizeof(outInfo->espPartName), "%sp%zu", outInfo->name, spanCount);
                        } else {
                            klibc_snprintf(outInfo->espPartName, sizeof(outInfo->espPartName), "%s%zu", outInfo->name, spanCount);
                        }
                    }
                }
            }
        }

        // Sort allocated partition extents by start LBA
        for (usize i = 0; i < spanCount; ++i) {
            for (usize j = i + 1; j < spanCount; ++j) {
                if (spans[j].start < spans[i].start) {
                    auto tmp = spans[i];
                    spans[i] = spans[j];
                    spans[j] = tmp;
                }
            }
        }

        // Find largest contiguous unallocated gap
        u64 cursor = (firstUsable < 2048) ? 2048 : firstUsable;
        u64 maxGapSectors = 0;
        FreeSpaceSpan largestGap{};

        for (usize i = 0; i < spanCount; ++i) {
            if (spans[i].start > cursor) {
                u64 gapStart = cursor;
                u64 gapEnd   = spans[i].start - 1;
                u64 gapCount = gapEnd - gapStart + 1;
                if (gapCount > maxGapSectors) {
                    maxGapSectors = gapCount;
                    largestGap.startLba    = gapStart;
                    largestGap.endLba      = gapEnd;
                    largestGap.sectorCount = gapCount;
                    largestGap.sizeMiB     = (gapCount * outInfo->blockSize) / (1024ULL * 1024ULL);
                }
            }
            if (spans[i].end + 1 > cursor) {
                cursor = spans[i].end + 1;
            }
        }

        if (lastUsable >= cursor) {
            u64 gapStart = cursor;
            u64 gapEnd   = lastUsable;
            u64 gapCount = gapEnd - gapStart + 1;
            if (gapCount > maxGapSectors) {
                maxGapSectors = gapCount;
                largestGap.startLba    = gapStart;
                largestGap.endLba      = gapEnd;
                largestGap.sectorCount = gapCount;
                largestGap.sizeMiB     = (gapCount * outInfo->blockSize) / (1024ULL * 1024ULL);
            }
        }

        outInfo->largestFree = largestGap;
        outInfo->hasEnoughSpace = (largestGap.sectorCount >= MIN_INSTALL_SECTORS);
        return Status::Ok;
    }

    // Blank or unpartitioned disk
    outInfo->isGpt = false;
    outInfo->hasEsp = false;
    outInfo->existingPartCount = 0;

    u64 gapStart = 2048;
    u64 gapEnd   = (outInfo->totalSectors > 34) ? (outInfo->totalSectors - 34) : 0;
    if (gapEnd > gapStart) {
        u64 gapCount = gapEnd - gapStart + 1;
        outInfo->largestFree.startLba    = gapStart;
        outInfo->largestFree.endLba      = gapEnd;
        outInfo->largestFree.sectorCount = gapCount;
        outInfo->largestFree.sizeMiB     = (gapCount * outInfo->blockSize) / (1024ULL * 1024ULL);
        outInfo->hasEnoughSpace          = (gapCount >= MIN_INSTALL_SECTORS);
    }
    return Status::Ok;
}

Status OsInstaller::installToDisk(IBlockDevice* targetDisk) noexcept {
    if (!targetDisk) return Status::InvalidArgument;
    if (!isRawDisk(targetDisk)) {
        pr_err("installer", "/dev/%s is a partition, not a whole disk", targetDisk->getName());
        return Status::InvalidArgument;
    }
    if (isLiveBootDisk(targetDisk)) {
        pr_err("installer", "Cannot install onto the live boot drive (/dev/%s)", targetDisk->getName());
        return Status::InvalidArgument;
    }

    DiskInstallInfo info{};
    Status st = findUnallocatedSpace(targetDisk, &info);
    if (st != Status::Ok) return st;

    if (!info.hasEnoughSpace) {
        pr_err("installer", "Insufficient unallocated space on /dev/%s (%llu MiB available; minimum %llu MiB required)",
               targetDisk->getName(), info.largestFree.sizeMiB, (MIN_INSTALL_SECTORS * targetDisk->getBlockSize()) / (1024ULL * 1024ULL));
        return Status::NoSpace;
    }

    pr_ok("installer", "Selected target disk /dev/%s (%llu MiB total, %llu MiB unallocated space)",
          targetDisk->getName(), info.totalSizeMiB, info.largestFree.sizeMiB);

    // Step 1: Read/prepare GPT Partition Entries and Headers
    CORTEZ_ALIGNED(16) u8 hdrBuf[4096]{};
    CORTEZ_ALIGNED(16) u8 entriesBuf[16384]{}; // 128 entries * 128 bytes = 32 sectors

    if (info.isGpt) {
        st = targetDisk->readBlocks(1, 1, hdrBuf);
        if (st != Status::Ok) return st;

        auto* hdr = reinterpret_cast<GptHeader*>(hdrBuf);
        st = targetDisk->readBlocks(hdr->partitionEntryLba, 32, entriesBuf);
        if (st != Status::Ok) return st;
    } else {
        // Initialize Protective MBR at LBA 0
        CORTEZ_ALIGNED(16) u8 mbrBuf[512]{};
        mbrBuf[446 + 4] = 0xEE; // OS Type = Protective GPT
        mbrBuf[446 + 8] = 0x01; // Starting LBA = 1
        u32 mbrCount = (targetDisk->getBlockCount() > 0xFFFFFFFFULL)
                           ? 0xFFFFFFFFU
                           : static_cast<u32>(targetDisk->getBlockCount() - 1);
        memcpy(&mbrBuf[446 + 12], &mbrCount, 4);
        *reinterpret_cast<u16*>(mbrBuf + 510) = 0xAA55U;
        (void)targetDisk->writeBlocks(0, 1, mbrBuf);

        // Initialize fresh GPT Header at LBA 1
        auto* hdr = reinterpret_cast<GptHeader*>(hdrBuf);
        memcpy(hdr->signature, "EFI PART", 8);
        hdr->revision             = 0x00010000;
        hdr->headerSize           = 92;
        hdr->headerCrc32          = 0;
        hdr->reserved             = 0;
        hdr->myLba                = 1;
        hdr->alternateLba         = targetDisk->getBlockCount() - 1;
        hdr->firstUsableLba       = 2048;
        hdr->lastUsableLba        = targetDisk->getBlockCount() - 34;
        generateGuid(hdr->diskGuid);
        hdr->partitionEntryLba    = 2;
        hdr->numPartitionEntries  = 128;
        hdr->sizeOfPartitionEntry = 128;
        memset(entriesBuf, 0, sizeof(entriesBuf));
    }

    auto* gptEntries = reinterpret_cast<GptPartitionEntry*>(entriesBuf);

    u64 freeStart = alignUp(info.largestFree.startLba, 2048);
    u64 freeEnd   = info.largestFree.endLba;

    // Step 2: Allocate ESP Partition if disk does not already have an existing EFI partition
    if (!info.hasEsp) {
        constexpr u64 ESP_SECTORS = 131072; // 64 MiB
        u64 espStart = freeStart;
        u64 espEnd   = espStart + ESP_SECTORS - 1;

        int espSlot = -1;
        for (u32 i = 0; i < 128; ++i) {
            if (isGuidEmpty(gptEntries[i].typeGuid)) {
                espSlot = static_cast<int>(i);
                break;
            }
        }
        if (espSlot >= 0) {
            memcpy(gptEntries[espSlot].typeGuid, GUID_ESP, 16);
            generateGuid(gptEntries[espSlot].uniqueGuid);
            gptEntries[espSlot].startingLba = espStart;
            gptEntries[espSlot].endingLba   = espEnd;
            gptEntries[espSlot].attributes  = 0;
            asciiToUtf16(gptEntries[espSlot].partitionName, "ESP");
        }

        freeStart = alignUp(espEnd + 1, 2048);
    }

    // Step 3: Allocate Cortez RootFS Partition in the remaining unallocated space
    u64 rootStart = freeStart;
    u64 rootEnd   = freeEnd;

    int rootSlot = -1;
    for (u32 i = 0; i < 128; ++i) {
        if (isGuidEmpty(gptEntries[i].typeGuid)) {
            rootSlot = static_cast<int>(i);
            break;
        }
    }
    if (rootSlot < 0) return Status::NoSpace;

    memcpy(gptEntries[rootSlot].typeGuid, GUID_BASIC_DATA, 16);
    generateGuid(gptEntries[rootSlot].uniqueGuid);
    gptEntries[rootSlot].startingLba = rootStart;
    gptEntries[rootSlot].endingLba   = rootEnd;
    gptEntries[rootSlot].attributes  = 0;
    asciiToUtf16(gptEntries[rootSlot].partitionName, "Cortez-RootFS");

    // Step 4: Write Primary and Backup GPT headers and entry tables
    auto* hdr = reinterpret_cast<GptHeader*>(hdrBuf);
    hdr->partitionEntryArrayCrc32 = calculateCrc32(entriesBuf, 128 * 128);
    hdr->headerCrc32 = 0;
    hdr->headerCrc32 = calculateCrc32(hdr, hdr->headerSize);

    targetDisk->writeBlocks(1, 1, hdrBuf);
    targetDisk->writeBlocks(hdr->partitionEntryLba, 32, entriesBuf);

    CORTEZ_ALIGNED(16) u8 backupHdrBuf[4096]{};
    auto* backupHdr = reinterpret_cast<GptHeader*>(backupHdrBuf);
    memcpy(backupHdr, hdr, sizeof(GptHeader));
    backupHdr->myLba               = hdr->alternateLba;
    backupHdr->alternateLba        = hdr->myLba;
    backupHdr->partitionEntryLba   = hdr->alternateLba - 32;
    backupHdr->headerCrc32         = 0;
    backupHdr->headerCrc32         = calculateCrc32(backupHdr, backupHdr->headerSize);

    targetDisk->writeBlocks(hdr->alternateLba - 32, 32, entriesBuf);
    targetDisk->writeBlocks(hdr->alternateLba, 1, backupHdrBuf);
    targetDisk->flush();

    // Step 5: Rescan partition slices on target disk
    GptPartitionScanner::instance().unregisterDiskPartitions(targetDisk);
    GptPartitionScanner::instance().scanDisk(targetDisk);

    // Step 6: Identify newly created target partition devices in DeviceRegistry
    IBlockDevice* targetRootDev = nullptr;
    IBlockDevice* targetEspDev  = nullptr;

    auto& reg = DeviceRegistry::instance();
    for (usize i = 0; i < reg.getBlockDeviceCount(); ++i) {
        auto* dev = reg.getBlockDeviceAt(i);
        if (!dev) continue;

        char prefix[32]{};
        klibc_snprintf(prefix, sizeof(prefix), "%sp", targetDisk->getName());
        bool isChild = (str::compareN(dev->getName(), prefix, str::length(prefix)) == 0) ||
                       (str::compareN(dev->getName(), targetDisk->getName(), str::length(targetDisk->getName())) == 0 && dev != targetDisk);

        if (isChild) {
            if (str::compare(dev->getLabel(), "Cortez-RootFS") == 0) {
                targetRootDev = dev;
            } else if (str::compare(dev->getLabel(), "ESP") == 0) {
                targetEspDev = dev;
            }
        }
    }

    if (!targetEspDev && info.hasEsp && info.espPartName[0]) {
        targetEspDev = reg.findBlockDevice(info.espPartName);
    }

    if (!targetRootDev || !targetEspDev) {
        pr_err("installer", "Failed to locate newly partitioned target devices on /dev/%s", targetDisk->getName());
        return Status::DeviceNotFound;
    }

    pr_ok("installer", "Target partitions configured: RootFS=/dev/%s (%llu MiB), ESP=/dev/%s",
          targetRootDev->getName(), (targetRootDev->getBlockCount() * targetRootDev->getBlockSize()) / (1024ULL * 1024ULL),
          targetEspDev->getName());

    // Step 7: Format RootFS with exFAT
    pr_ok("installer", "Formatting /dev/%s with physical exFAT ('Cortez-RootFS')...", targetRootDev->getName());
    auto* rootFs = new fs::exfat::ExFatFileSystem();
    st = rootFs->format(targetRootDev, "Cortez-RootFS");
    delete rootFs;
    if (st != Status::Ok) {
        pr_err("installer", "Failed to format /dev/%s as exFAT (%s)", targetRootDev->getName(), statusToString(st));
        return st;
    }

    // Step 8: If fresh ESP partition, format with FAT32
    if (!info.hasEsp) {
        pr_ok("installer", "Formatting /dev/%s with FAT32 EFI System Partition ('ESP')...", targetEspDev->getName());
        auto* espFs = new fs::fat::Fat32FileSystem();
        st = espFs->format(targetEspDev, "ESP");
        delete espFs;
        if (st != Status::Ok) {
            pr_err("installer", "Failed to format /dev/%s as FAT32 (%s)", targetEspDev->getName(), statusToString(st));
            return st;
        }
    }

    // Step 9: Discover live source ESP partition to copy Limine bootloader and kernel
    IBlockDevice* liveRoot = fs::VfsManager::instance().getRootBlockDevice();
    IBlockDevice* srcEspDev = nullptr;
    if (liveRoot) {
        char srcEspName[32]{};
        const char* p = str::findChar(liveRoot->getName(), 'p');
        if (p) {
            usize baseLen = static_cast<usize>(p - liveRoot->getName());
            char base[32]{};
            if (baseLen >= sizeof(base)) baseLen = sizeof(base) - 1;
            memcpy(base, liveRoot->getName(), baseLen);
            base[baseLen] = '\0';
            klibc_snprintf(srcEspName, sizeof(srcEspName), "%sp1", base);
            srcEspDev = reg.findBlockDevice(srcEspName);
        } else {
            usize rLen = str::length(liveRoot->getName());
            char base[32]{};
            str::copy(base, liveRoot->getName(), sizeof(base));
            if (rLen > 1 && base[rLen - 1] >= '0' && base[rLen - 1] <= '9') {
                base[rLen - 1] = '1';
                srcEspDev = reg.findBlockDevice(base);
            }
        }
    }

    auto* srcEspFs = new fs::fat::Fat32FileSystem();
    bool srcEspMounted = false;
    if (srcEspDev) {
        (void)fs::VfsManager::instance().mkdir("/", "/mnt");
        (void)fs::VfsManager::instance().mkdir("/mnt", "src_esp");
        if (srcEspFs->mount(srcEspDev) == Status::Ok) {
            (void)fs::VfsManager::instance().mount("/mnt/src_esp", srcEspFs, srcEspDev);
            srcEspMounted = true;
        }
    }

    // Step 10: Mount Target RootFS and Target ESP
    auto* targetRootFs = new fs::exfat::ExFatFileSystem();
    (void)fs::VfsManager::instance().mkdir("/", "/mnt");
    (void)fs::VfsManager::instance().mkdir("/mnt", "target_root");
    st = targetRootFs->mount(targetRootDev);
    if (st != Status::Ok) {
        pr_err("installer", "Failed to mount target RootFS /dev/%s (%s)", targetRootDev->getName(), statusToString(st));
        return st;
    }
    (void)fs::VfsManager::instance().mount("/mnt/target_root", targetRootFs, targetRootDev);

    auto* targetEspFs = new fs::fat::Fat32FileSystem();
    (void)fs::VfsManager::instance().mkdir("/mnt", "target_esp");
    st = targetEspFs->mount(targetEspDev);
    if (st != Status::Ok) {
        pr_err("installer", "Failed to mount target ESP /dev/%s (%s)", targetEspDev->getName(), statusToString(st));
        return st;
    }
    (void)fs::VfsManager::instance().mount("/mnt/target_esp", targetEspFs, targetEspDev);

    // Step 11: Copy RootFS system directory structure and kernel extensions
    pr_ok("installer", "Copying Cortez OS system directories and modules to /dev/%s...", targetRootDev->getName());
    (void)fs::VfsManager::instance().mkdir("/mnt/target_root", "boot");
    (void)fs::VfsManager::instance().mkdir("/mnt/target_root", "ext");
    (void)fs::VfsManager::instance().mkdir("/mnt/target_root/ext", "drivers");
    (void)fs::VfsManager::instance().mkdir("/mnt/target_root/ext", "commands");
    (void)fs::VfsManager::instance().mkdir("/mnt/target_root", "var");
    (void)fs::VfsManager::instance().mkdir("/mnt/target_root/var", "log");
    (void)fs::VfsManager::instance().mkdir("/mnt/target_root", "etc");
    (void)fs::VfsManager::instance().mkdir("/mnt/target_root", "home");

    copyDirRecursive("/ext/drivers", "/mnt/target_root/ext/drivers");
    copyDirRecursive("/ext/commands", "/mnt/target_root/ext/commands");

    // Step 12: Write Limine Bootloader, kernel binary, and boot entry to target ESP
    pr_ok("installer", "Writing Limine UEFI bootloader and boot entry to /dev/%s...", targetEspDev->getName());
    (void)fs::VfsManager::instance().mkdir("/mnt/target_esp", "EFI");
    (void)fs::VfsManager::instance().mkdir("/mnt/target_esp/EFI", "BOOT");
    (void)fs::VfsManager::instance().mkdir("/mnt/target_esp", "boot");

    if (srcEspMounted) {
        (void)copyFile("/mnt/src_esp/EFI/BOOT/BOOTX64.EFI", "/mnt/target_esp/EFI/BOOT/BOOTX64.EFI");
        (void)copyFile("/mnt/src_esp/boot/kernel.elf", "/mnt/target_esp/boot/kernel.elf");
        (void)copyFile("/mnt/src_esp/boot/kernel.elf", "/mnt/target_root/boot/kernel.elf");
    } else {
        (void)copyFile("/boot/kernel.elf", "/mnt/target_esp/boot/kernel.elf");
        (void)copyFile("/boot/kernel.elf", "/mnt/target_root/boot/kernel.elf");
    }

    char confBuf[512]{};
    klibc_snprintf(confBuf, sizeof(confBuf),
        "timeout: 3\n\n"
        "/Cortez Kernel (x86_64 UEFI)\n"
        "    protocol: limine\n"
        "    kernel_path: boot():/boot/kernel.elf\n"
        "    cmdline: root=/dev/%s rootfstype=exfat rw console=tty0\n\n"
        "/Cortez Kernel (Safe Mode - Skip /ext Autoload)\n"
        "    protocol: limine\n"
        "    kernel_path: boot():/boot/kernel.elf\n"
        "    cmdline: root=/dev/%s rootfstype=exfat rw console=tty0 cortez.noext\n",
        targetRootDev->getName(), targetRootDev->getName()
    );
    (void)fs::VfsManager::instance().writeFileAll("/", "/mnt/target_esp/limine.conf", confBuf, str::length(confBuf));

    // Step 13: Flush and unmount
    targetRootFs->sync();
    targetEspFs->sync();
    targetDisk->flush();

    (void)fs::VfsManager::instance().unmount("/mnt/target_esp");
    (void)fs::VfsManager::instance().unmount("/mnt/target_root");
    if (srcEspMounted) {
        (void)fs::VfsManager::instance().unmount("/mnt/src_esp");
    }

    delete targetRootFs;
    delete targetEspFs;
    delete srcEspFs;

    pr_ok("installer", "===============================================================");
    pr_ok("installer", "Cortez Kernel successfully installed to /dev/%s!", targetDisk->getName());
    pr_ok("installer", "RootFS partition: /dev/%s (exFAT, label='Cortez-RootFS')", targetRootDev->getName());
    pr_ok("installer", "EFI System Partition: /dev/%s (Limine UEFI configured)", targetEspDev->getName());
    pr_ok("installer", "Installation complete. You can now reboot and remove the live USB.");
    pr_ok("installer", "===============================================================");

    return Status::Ok;
}

Status OsInstaller::installByName(const char* diskName) noexcept {
    if (!diskName || !diskName[0]) return Status::InvalidArgument;

    const char* clean = diskName;
    if (str::compareN(clean, "/dev/", 5) == 0) {
        clean += 5;
    }

    auto* dev = DeviceRegistry::instance().findBlockDevice(clean);
    if (!dev) {
        pr_err("installer", "Device /dev/%s not found", clean);
        return Status::DeviceNotFound;
    }

    return installToDisk(dev);
}

} // namespace cortez::drivers::storage
