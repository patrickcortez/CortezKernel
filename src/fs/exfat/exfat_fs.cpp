/**
 * @file exfat_fs.cpp
 * @brief Implementation of the physical exFAT Filesystem driver.
 */

#include "exfat_fs.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::fs::exfat {

namespace {
void asciiToUtf16(u16* dst, const char* src, usize maxChars) noexcept {
    for (usize i = 0; i < maxChars; ++i) {
        if (*src) {
            dst[i] = static_cast<u8>(*src++);
        } else {
            dst[i] = 0;
        }
    }
}

void asciiToUpperUtf16(u16* dst, const char* src, usize count) noexcept {
    for (usize i = 0; i < count; ++i) {
        dst[i] = static_cast<u8>(str::toUpper(src[i]));
    }
}
} // namespace

// =============================================================================
// ExFatNode Implementation
// =============================================================================

ExFatNode::ExFatNode(ExFatFileSystem* fs,
                     const char* name,
                     NodeType type,
                     u32 firstCluster,
                     u64 dataSize,
                     u8 streamFlags,
                     u32 parentDirCluster,
                     u32 dirEntrySlotIndex) noexcept
    : m_fs(fs),
      m_type(type),
      m_firstCluster(firstCluster),
      m_size(dataSize),
      m_streamFlags(streamFlags),
      m_parentCluster(parentDirCluster),
      m_dirSlotIndex(dirEntrySlotIndex) {
    str::copy(m_name, name ? name : "", sizeof(m_name));
}

Result<usize> ExFatNode::read(u64 offset, void* buffer, usize size) {
    if (m_type != NodeType::File) return Status::IsDirectory;
    if (!buffer) return Status::InvalidArgument;
    if (offset >= m_size || size == 0 || m_firstCluster < EXFAT_CLUSTER_FIRST) {
        return usize(0);
    }

    u64 remainingFile = m_size - offset;
    usize toRead = (size > remainingFile) ? static_cast<usize>(remainingFile) : size;

    u32 clusterSize = m_fs->getClusterSize();
    u8* clusterBuf  = new u8[clusterSize];
    if (!clusterBuf) return Status::OutOfMemory;

    u32 currCluster = m_firstCluster;
    u64 skipClusters = offset / clusterSize;
    for (u64 i = 0; i < skipClusters; ++i) {
        currCluster = m_fs->getNextCluster(currCluster, m_streamFlags);
        if (currCluster < EXFAT_CLUSTER_FIRST || currCluster >= EXFAT_CLUSTER_BAD) {
            delete[] clusterBuf;
            return Status::IoError;
        }
    }

    usize bytesRead  = 0;
    u32   inCluster  = static_cast<u32>(offset % clusterSize);
    u8*   outPtr     = static_cast<u8*>(buffer);

    while (bytesRead < toRead && currCluster >= EXFAT_CLUSTER_FIRST && currCluster < EXFAT_CLUSTER_BAD) {
        Status st = m_fs->readCluster(currCluster, clusterBuf);
        if (st != Status::Ok) {
            delete[] clusterBuf;
            return st;
        }

        usize chunk = clusterSize - inCluster;
        if (chunk > (toRead - bytesRead)) {
            chunk = toRead - bytesRead;
        }

        memcpy(outPtr + bytesRead, clusterBuf + inCluster, chunk);
        bytesRead += chunk;
        inCluster  = 0;

        if (bytesRead < toRead) {
            currCluster = m_fs->getNextCluster(currCluster, m_streamFlags);
        }
    }

    delete[] clusterBuf;
    return bytesRead;
}

Result<usize> ExFatNode::write(u64 offset, const void* buffer, usize size) {
    if (m_type != NodeType::File) return Status::IsDirectory;
    if (!buffer) return Status::InvalidArgument;
    if (size == 0) return usize(0);

    u32 clusterSize = m_fs->getClusterSize();
    u8* clusterBuf  = new u8[clusterSize];
    if (!clusterBuf) return Status::OutOfMemory;

    // Allocate first cluster if file is currently empty
    if (m_firstCluster < EXFAT_CLUSTER_FIRST) {
        auto allocRes = m_fs->allocateCluster(0);
        if (allocRes.isError()) {
            delete[] clusterBuf;
            return allocRes.status();
        }
        m_firstCluster = allocRes.value();
        m_streamFlags  = EXFAT_FLAG_ALLOC_POSSIBLE; // Use FAT chain for dynamic growth
    }

    // Ensure clusters exist up to the starting offset
    u32 currCluster  = m_firstCluster;
    u64 targetClusterIdx = offset / clusterSize;
    for (u64 i = 0; i < targetClusterIdx; ++i) {
        u32 next = m_fs->getNextCluster(currCluster, m_streamFlags);
        if (next < EXFAT_CLUSTER_FIRST || next >= EXFAT_CLUSTER_BAD) {
            m_streamFlags &= ~EXFAT_FLAG_NO_FAT_CHAIN;
            auto allocRes = m_fs->allocateCluster(currCluster);
            if (allocRes.isError()) {
                delete[] clusterBuf;
                return allocRes.status();
            }
            next = allocRes.value();
        }
        currCluster = next;
    }

    usize bytesWritten = 0;
    u32   inCluster    = static_cast<u32>(offset % clusterSize);
    const u8* inPtr    = static_cast<const u8*>(buffer);

    while (bytesWritten < size) {
        usize chunk = clusterSize - inCluster;
        if (chunk > (size - bytesWritten)) {
            chunk = size - bytesWritten;
        }

        if (chunk < clusterSize) {
            Status st = m_fs->readCluster(currCluster, clusterBuf);
            if (st != Status::Ok) {
                memset(clusterBuf, 0, clusterSize);
            }
        }

        memcpy(clusterBuf + inCluster, inPtr + bytesWritten, chunk);

        Status st = m_fs->writeCluster(currCluster, clusterBuf);
        if (st != Status::Ok) {
            delete[] clusterBuf;
            return st;
        }

        bytesWritten += chunk;
        inCluster     = 0;

        if (bytesWritten < size) {
            u32 next = m_fs->getNextCluster(currCluster, m_streamFlags);
            if (next < EXFAT_CLUSTER_FIRST || next >= EXFAT_CLUSTER_BAD) {
                m_streamFlags &= ~EXFAT_FLAG_NO_FAT_CHAIN;
                auto allocRes = m_fs->allocateCluster(currCluster);
                if (allocRes.isError()) {
                    delete[] clusterBuf;
                    return allocRes.status();
                }
                next = allocRes.value();
            }
            currCluster = next;
        }
    }

    delete[] clusterBuf;

    if (offset + bytesWritten > m_size) {
        m_size = offset + bytesWritten;
    }

    Status st = m_fs->updateDirectoryEntryOnDisk(this);
    if (st != Status::Ok) return st;

    m_fs->sync();
    return bytesWritten;
}

Status ExFatNode::truncate(u64 newSize) {
    if (m_type != NodeType::File) return Status::IsDirectory;
    if (newSize == 0) {
        if (m_firstCluster >= EXFAT_CLUSTER_FIRST) {
            m_fs->freeClusterChain(m_firstCluster, m_size, m_streamFlags);
            m_firstCluster = 0;
        }
        m_size = 0;
        Status st = m_fs->updateDirectoryEntryOnDisk(this);
        m_fs->sync();
        return st;
    }
    m_size = newSize;
    Status st = m_fs->updateDirectoryEntryOnDisk(this);
    m_fs->sync();
    return st;
}

Result<IVfsNode*> ExFatNode::lookup(const char* name) {
    if (m_type != NodeType::Directory) return Status::NotDirectory;
    return m_fs->lookupInDir(this, name);
}

Result<IVfsNode*> ExFatNode::createFile(const char* name) {
    if (m_type != NodeType::Directory) return Status::NotDirectory;
    return m_fs->createEntryInDir(this, name, false);
}

Result<IVfsNode*> ExFatNode::createDir(const char* name) {
    if (m_type != NodeType::Directory) return Status::NotDirectory;
    return m_fs->createEntryInDir(this, name, true);
}

Status ExFatNode::remove(const char* name) {
    if (m_type != NodeType::Directory) return Status::NotDirectory;
    return m_fs->removeEntryInDir(this, name);
}

Status ExFatNode::readDir(usize index, DirEntry* outEntry) {
    if (m_type != NodeType::Directory) return Status::NotDirectory;
    return m_fs->readDirEntryAt(this, index, outEntry);
}

Status ExFatNode::stat(FileStat* outStat) const noexcept {
    if (!outStat) return Status::InvalidArgument;
    str::copy(outStat->name, m_name, sizeof(outStat->name));
    outStat->type         = m_type;
    outStat->size         = m_size;
    outStat->firstCluster = m_firstCluster;
    u32 clSize            = m_fs->getClusterSize();
    outStat->blocks       = static_cast<u32>((m_size + clSize - 1) / clSize);
    return Status::Ok;
}

Status ExFatNode::sync() {
    return m_fs->sync();
}

// =============================================================================
// ExFatFileSystem Implementation
// =============================================================================

u64 ExFatFileSystem::clusterToLba(u32 cluster) const noexcept {
    if (cluster < EXFAT_CLUSTER_FIRST) return m_heapOffsetSectors;
    return static_cast<u64>(m_heapOffsetSectors) +
           static_cast<u64>(cluster - EXFAT_CLUSTER_FIRST) * m_sectorsPerCluster;
}

Status ExFatFileSystem::readCluster(u32 cluster, void* buffer) noexcept {
    if (!m_device || !buffer || cluster < EXFAT_CLUSTER_FIRST) {
        return Status::InvalidArgument;
    }
    return m_device->readBlocks(clusterToLba(cluster), m_sectorsPerCluster, buffer);
}

Status ExFatFileSystem::writeCluster(u32 cluster, const void* buffer) noexcept {
    if (!m_device || !buffer || cluster < EXFAT_CLUSTER_FIRST) {
        return Status::InvalidArgument;
    }
    return m_device->writeBlocks(clusterToLba(cluster), m_sectorsPerCluster, buffer);
}

bool ExFatFileSystem::isClusterAllocated(u32 cluster) const noexcept {
    if (!m_bitmap || cluster < EXFAT_CLUSTER_FIRST) return true;
    u32 bitIdx  = cluster - EXFAT_CLUSTER_FIRST;
    u32 byteIdx = bitIdx / 8U;
    if (byteIdx >= m_bitmapByteLen) return true;
    return (m_bitmap[byteIdx] & (1U << (bitIdx % 8U))) != 0;
}

void ExFatFileSystem::setClusterAllocated(u32 cluster, bool allocated) noexcept {
    if (!m_bitmap || cluster < EXFAT_CLUSTER_FIRST) return;
    u32 bitIdx  = cluster - EXFAT_CLUSTER_FIRST;
    u32 byteIdx = bitIdx / 8U;
    if (byteIdx >= m_bitmapByteLen) return;

    if (allocated) {
        m_bitmap[byteIdx] |= static_cast<u8>(1U << (bitIdx % 8U));
    } else {
        m_bitmap[byteIdx] &= static_cast<u8>(~(1U << (bitIdx % 8U)));
    }
    m_bitmapDirty = true;
}

u32 ExFatFileSystem::readFatEntry(u32 cluster) noexcept {
    if (!m_device || cluster >= m_clusterCount + EXFAT_CLUSTER_FIRST) {
        return EXFAT_CLUSTER_EOC;
    }
    u64 byteOffset   = static_cast<u64>(cluster) * sizeof(u32);
    u64 fatSectorLba = static_cast<u64>(m_fatOffsetSectors) + (byteOffset / m_bytesPerSector);
    u32 offsetInSec  = static_cast<u32>(byteOffset % m_bytesPerSector);

    CORTEZ_ALIGNED(16) u8 sectorBuf[4096]{};
    if (m_device->readBlocks(fatSectorLba, 1, sectorBuf) != Status::Ok) {
        return EXFAT_CLUSTER_EOC;
    }
    return *reinterpret_cast<const u32*>(sectorBuf + offsetInSec);
}

Status ExFatFileSystem::writeFatEntry(u32 cluster, u32 value) noexcept {
    if (!m_device || cluster >= m_clusterCount + EXFAT_CLUSTER_FIRST) {
        return Status::InvalidArgument;
    }
    u64 byteOffset   = static_cast<u64>(cluster) * sizeof(u32);
    u64 fatSectorLba = static_cast<u64>(m_fatOffsetSectors) + (byteOffset / m_bytesPerSector);
    u32 offsetInSec  = static_cast<u32>(byteOffset % m_bytesPerSector);

    CORTEZ_ALIGNED(16) u8 sectorBuf[4096]{};
    Status st = m_device->readBlocks(fatSectorLba, 1, sectorBuf);
    if (st != Status::Ok) return st;

    *reinterpret_cast<u32*>(sectorBuf + offsetInSec) = value;
    return m_device->writeBlocks(fatSectorLba, 1, sectorBuf);
}

u32 ExFatFileSystem::getNextCluster(u32 currentCluster, u8 streamFlags) noexcept {
    if (currentCluster < EXFAT_CLUSTER_FIRST) return EXFAT_CLUSTER_EOC;
    if (streamFlags & EXFAT_FLAG_NO_FAT_CHAIN) {
        return currentCluster + 1U;
    }
    return readFatEntry(currentCluster);
}

Result<u32> ExFatFileSystem::allocateCluster(u32 prevCluster) noexcept {
    for (u32 c = EXFAT_CLUSTER_FIRST; c < m_clusterCount + EXFAT_CLUSTER_FIRST; ++c) {
        if (!isClusterAllocated(c)) {
            setClusterAllocated(c, true);
            writeFatEntry(c, EXFAT_CLUSTER_EOC);
            if (prevCluster >= EXFAT_CLUSTER_FIRST) {
                writeFatEntry(prevCluster, c);
            }

            // Zero out newly allocated cluster on disk
            u8* zeroBuf = new u8[m_clusterBytes];
            if (zeroBuf) {
                memset(zeroBuf, 0, m_clusterBytes);
                writeCluster(c, zeroBuf);
                delete[] zeroBuf;
            }
            return c;
        }
    }
    return Status::NoSpace;
}

Status ExFatFileSystem::freeClusterChain(u32 startCluster, u64 byteSize, u8 streamFlags) noexcept {
    if (startCluster < EXFAT_CLUSTER_FIRST) return Status::Ok;

    if (streamFlags & EXFAT_FLAG_NO_FAT_CHAIN) {
        u32 count = static_cast<u32>((byteSize + m_clusterBytes - 1) / m_clusterBytes);
        if (count == 0) count = 1;
        for (u32 i = 0; i < count; ++i) {
            setClusterAllocated(startCluster + i, false);
            writeFatEntry(startCluster + i, EXFAT_CLUSTER_FREE);
        }
        return Status::Ok;
    }

    u32 curr = startCluster;
    u32 guard = 0;
    while (curr >= EXFAT_CLUSTER_FIRST && curr < EXFAT_CLUSTER_BAD && guard++ < m_clusterCount) {
        u32 next = readFatEntry(curr);
        setClusterAllocated(curr, false);
        writeFatEntry(curr, EXFAT_CLUSTER_FREE);
        curr = next;
    }
    return Status::Ok;
}

Status ExFatFileSystem::flushBitmapToDisk() noexcept {
    if (!m_bitmapDirty || !m_bitmap || !m_device) return Status::Ok;

    u8* clusterBuf = new u8[m_clusterBytes];
    if (!clusterBuf) return Status::OutOfMemory;

    u32 clustersNeeded = (m_bitmapByteLen + m_clusterBytes - 1) / m_clusterBytes;
    for (u32 i = 0; i < clustersNeeded; ++i) {
        memset(clusterBuf, 0, m_clusterBytes);
        u32 offset = i * m_clusterBytes;
        u32 copyLen = (m_bitmapByteLen - offset > m_clusterBytes) ? m_clusterBytes : (m_bitmapByteLen - offset);
        memcpy(clusterBuf, m_bitmap + offset, copyLen);
        Status st = writeCluster(m_bitmapCluster + i, clusterBuf);
        if (st != Status::Ok) {
            delete[] clusterBuf;
            return st;
        }
    }

    delete[] clusterBuf;
    m_bitmapDirty = false;
    return Status::Ok;
}

ExFatNode* ExFatFileSystem::cacheOrCreateNode(const char* name,
                                              NodeType type,
                                              u32 firstCluster,
                                              u64 size,
                                              u8 flags,
                                              u32 parentCluster,
                                              u32 slotIndex) noexcept {
    for (usize i = 0; i < m_nodeCacheCount; ++i) {
        ExFatNode* existing = m_nodeCache[i];
        if (existing &&
            existing->getParentDirCluster() == parentCluster &&
            existing->getDirEntrySlot() == slotIndex &&
            str::compareCaseInsensitive(existing->getName(), name) == 0) {
            existing->updateMetadata(firstCluster, size, flags);
            return existing;
        }
    }

    auto* node = new ExFatNode(this, name, type, firstCluster, size, flags, parentCluster, slotIndex);
    if (node && m_nodeCacheCount < MAX_OPEN_NODES) {
        m_nodeCache[m_nodeCacheCount++] = node;
    }
    return node;
}

Status ExFatFileSystem::mount(drivers::IBlockDevice* dev) {
    if (!dev) return Status::InvalidArgument;

    m_device         = dev;
    m_bytesPerSector = dev->getBlockSize();

    CORTEZ_ALIGNED(16) u8 bootBuf[4096]{};
    Status st = m_device->readBlocks(0, 1, bootBuf);
    if (st != Status::Ok) return st;

    const auto* bs = reinterpret_cast<const ExFatBootSector*>(bootBuf);
    if (str::compareN(bs->fsName, "EXFAT   ", 8) != 0 || bs->bootSignature != 0xAA55U) {
        return Status::CorruptedFs;
    }

    m_bytesPerSector    = 1U << bs->bytesPerSectorShift;
    m_sectorsPerCluster = 1U << bs->sectorsPerClusterShift;
    m_clusterBytes      = m_bytesPerSector * m_sectorsPerCluster;
    m_fatOffsetSectors  = bs->fatOffset;
    m_fatLengthSectors  = bs->fatLength;
    m_heapOffsetSectors = bs->clusterHeapOffset;
    m_clusterCount      = bs->clusterCount;
    m_rootFirstCluster  = bs->rootDirFirstCluster;

    m_bitmapCluster = EXFAT_CLUSTER_FIRST;
    m_bitmapByteLen = (m_clusterCount + 7U) / 8U;

    // Scan root directory cluster for 0x81 Allocation Bitmap entry & 0x83 Volume Label entry
    u8* rootBuf = new u8[m_clusterBytes];
    if (!rootBuf) return Status::OutOfMemory;

    if (readCluster(m_rootFirstCluster, rootBuf) == Status::Ok) {
        u32 entriesCount = m_clusterBytes / sizeof(ExFatDirEntryRaw);
        const auto* entries = reinterpret_cast<const ExFatDirEntryRaw*>(rootBuf);
        for (u32 i = 0; i < entriesCount; ++i) {
            if (entries[i].entryType == EXFAT_ENTRY_EOD) break;
            if (entries[i].entryType == EXFAT_ENTRY_BITMAP) {
                const auto* bm = reinterpret_cast<const ExFatBitmapEntry*>(&entries[i]);
                if (bm->firstCluster >= EXFAT_CLUSTER_FIRST) {
                    m_bitmapCluster = bm->firstCluster;
                }
                if (bm->dataLength > 0 && bm->dataLength <= m_clusterCount) {
                    m_bitmapByteLen = static_cast<u32>(bm->dataLength);
                }
            } else if (entries[i].entryType == EXFAT_ENTRY_LABEL) {
                const auto* lbl = reinterpret_cast<const ExFatLabelEntry*>(&entries[i]);
                u8 count = (lbl->charCount > 11) ? 11 : lbl->charCount;
                for (u8 c = 0; c < count; ++c) {
                    m_volumeLabel[c] = static_cast<char>(lbl->volumeLabel[c] & 0x7FU);
                }
                m_volumeLabel[count] = '\0';
            }
        }
    }

    // Load Allocation Bitmap into memory cache (capped at 128 KiB to prevent heap starvation)
    u32 bitmapAllocBytes = static_cast<u32>(alignUp(m_bitmapByteLen, m_clusterBytes));
    if (bitmapAllocBytes > 128 * 1024) {
        bitmapAllocBytes = 128 * 1024;
    }
    m_bitmap = new u8[bitmapAllocBytes];
    if (!m_bitmap) {
        delete[] rootBuf;
        return Status::OutOfMemory;
    }

    u32 bitmapClusters = bitmapAllocBytes / m_clusterBytes;
    if (bitmapClusters > 16) bitmapClusters = 16;
    for (u32 i = 0; i < bitmapClusters; ++i) {
        readCluster(m_bitmapCluster + i, m_bitmap + i * m_clusterBytes);
    }
    delete[] rootBuf;

    m_rootNode = new ExFatNode(this, "/", NodeType::Directory, m_rootFirstCluster,
                               m_clusterBytes, EXFAT_FLAG_ALLOC_POSSIBLE, 0, 0);

    pr_ok("exfat", "Mounted physical exFAT volume '%s' on /dev/%s (%u clusters x %u B)",
          m_volumeLabel, m_device->getName(), m_clusterCount, m_clusterBytes);
    return Status::Ok;
}

Status ExFatFileSystem::unmount() {
    return sync();
}

Status ExFatFileSystem::sync() {
    Status st = flushBitmapToDisk();
    if (st != Status::Ok) return st;
    return m_device ? m_device->flush() : Status::Ok;
}

Status ExFatFileSystem::statFs(FsStat* outStat) const noexcept {
    if (!outStat) return Status::InvalidArgument;

    str::copy(outStat->fsType, "exfat", sizeof(outStat->fsType));
    str::copy(outStat->volumeLabel, m_volumeLabel, sizeof(outStat->volumeLabel));
    outStat->clusterSize   = m_clusterBytes;
    outStat->totalClusters = m_clusterCount;

    u32 freeCount = 0;
    if (m_bitmap) {
        u32 maxBytes = (m_bitmapByteLen < 128 * 1024) ? m_bitmapByteLen : (128 * 1024);
        for (u32 b = 0; b < maxBytes; ++b) {
            u8 byteVal = m_bitmap[b];
            if (byteVal == 0x00U) {
                freeCount += 8;
            } else if (byteVal != 0xFFU) {
                for (u32 bit = 0; bit < 8; ++bit) {
                    if (!(byteVal & (1U << bit))) ++freeCount;
                }
            }
        }
    }

    outStat->freeClusters = freeCount;
    outStat->totalBytes   = static_cast<u64>(m_clusterCount) * m_clusterBytes;
    outStat->freeBytes    = static_cast<u64>(freeCount) * m_clusterBytes;
    outStat->usedBytes    = outStat->totalBytes - outStat->freeBytes;
    return Status::Ok;
}

Status ExFatFileSystem::updateDirectoryEntryOnDisk(ExFatNode* node) noexcept {
    if (!node || node->getParentDirCluster() < EXFAT_CLUSTER_FIRST) {
        return Status::Ok; // Root directory itself has no parent entry
    }

    u8* dirBuf = new u8[m_clusterBytes];
    if (!dirBuf) return Status::OutOfMemory;

    u32 parentCluster = node->getParentDirCluster();
    Status st = readCluster(parentCluster, dirBuf);
    if (st != Status::Ok) {
        delete[] dirBuf;
        return st;
    }

    u32 slot = node->getDirEntrySlot();
    u32 maxSlots = m_clusterBytes / sizeof(ExFatDirEntryRaw);
    if (slot + 2 >= maxSlots) {
        delete[] dirBuf;
        return Status::CorruptedFs;
    }

    auto* rawEntries  = reinterpret_cast<ExFatDirEntryRaw*>(dirBuf);
    auto* fileEntry   = reinterpret_cast<ExFatFileEntry*>(&rawEntries[slot]);
    auto* streamEntry = reinterpret_cast<ExFatStreamEntry*>(&rawEntries[slot + 1]);

    if (fileEntry->entryType != EXFAT_ENTRY_FILE || streamEntry->entryType != EXFAT_ENTRY_STREAM) {
        delete[] dirBuf;
        return Status::CorruptedFs;
    }

    streamEntry->flags           = node->getStreamFlags();
    streamEntry->firstCluster    = node->getFirstCluster();
    streamEntry->validDataLength = node->getSize();
    streamEntry->dataLength      = node->getSize();

    usize totalEntrySetCount = 1U + fileEntry->secondaryCount;
    fileEntry->setChecksum   = computeEntrySetChecksum(fileEntry, totalEntrySetCount);

    st = writeCluster(parentCluster, dirBuf);
    delete[] dirBuf;
    return st;
}

Result<IVfsNode*> ExFatFileSystem::lookupInDir(ExFatNode* dirNode, const char* targetName) noexcept {
    if (!dirNode || !targetName || !targetName[0]) return Status::InvalidArgument;

    u8* dirBuf = new u8[m_clusterBytes];
    if (!dirBuf) return Status::OutOfMemory;

    u32 dirCluster = dirNode->getFirstCluster();
    if (readCluster(dirCluster, dirBuf) != Status::Ok) {
        delete[] dirBuf;
        return Status::IoError;
    }

    u32 maxEntries = m_clusterBytes / sizeof(ExFatDirEntryRaw);
    const auto* entries = reinterpret_cast<const ExFatDirEntryRaw*>(dirBuf);

    for (u32 i = 0; i + 2 < maxEntries; ++i) {
        if (entries[i].entryType == EXFAT_ENTRY_EOD) break;
        if (entries[i].entryType != EXFAT_ENTRY_FILE) continue;

        const auto* fileEntry   = reinterpret_cast<const ExFatFileEntry*>(&entries[i]);
        const auto* streamEntry = reinterpret_cast<const ExFatStreamEntry*>(&entries[i + 1]);
        if (streamEntry->entryType != EXFAT_ENTRY_STREAM) continue;

        char nameBuf[64]{};
        usize namePos = 0;
        u8 secCount   = fileEntry->secondaryCount;
        for (u8 s = 2; s <= secCount && (i + s) < maxEntries; ++s) {
            if (entries[i + s].entryType != EXFAT_ENTRY_FILENAME) break;
            const auto* fn = reinterpret_cast<const ExFatNameEntry*>(&entries[i + s]);
            for (usize c = 0; c < 15 && namePos < streamEntry->nameLength && namePos + 1 < sizeof(nameBuf); ++c) {
                u16 ch = fn->fileName[c];
                if (ch == 0) break;
                nameBuf[namePos++] = static_cast<char>(ch & 0x7FU);
            }
        }
        nameBuf[namePos] = '\0';

        if (str::compareCaseInsensitive(nameBuf, targetName) == 0) {
            NodeType ntype = (fileEntry->fileAttributes & EXFAT_ATTR_DIRECTORY)
                                 ? NodeType::Directory
                                 : NodeType::File;
            ExFatNode* found = cacheOrCreateNode(nameBuf,
                                                 ntype,
                                                 streamEntry->firstCluster,
                                                 streamEntry->validDataLength,
                                                 streamEntry->flags,
                                                 dirCluster,
                                                 i);
            delete[] dirBuf;
            return found ? Result<IVfsNode*>(found) : Result<IVfsNode*>(Status::OutOfMemory);
        }

        i += secCount;
    }

    delete[] dirBuf;
    return Status::NotFound;
}

Result<IVfsNode*> ExFatFileSystem::createEntryInDir(ExFatNode* dirNode,
                                                    const char* name,
                                                    bool isDirectory) noexcept {
    if (!dirNode || !name || !name[0]) return Status::InvalidArgument;

    // Fail if an entry with this name already exists
    if (lookupInDir(dirNode, name).isOk()) {
        return Status::AlreadyExists;
    }

    usize nameLen = str::length(name);
    if (nameLen > 45) nameLen = 45;
    u8 nameEntriesCount = static_cast<u8>((nameLen + 14) / 15);
    u8 totalEntriesNeeded = static_cast<u8>(2 + nameEntriesCount);

    u8* dirBuf = new u8[m_clusterBytes];
    if (!dirBuf) return Status::OutOfMemory;

    u32 dirCluster = dirNode->getFirstCluster();
    Status st = readCluster(dirCluster, dirBuf);
    if (st != Status::Ok) {
        delete[] dirBuf;
        return st;
    }

    u32 maxEntries = m_clusterBytes / sizeof(ExFatDirEntryRaw);
    auto* entries  = reinterpret_cast<ExFatDirEntryRaw*>(dirBuf);

    // Find contiguous free/EOD slots in directory cluster
    u32 foundSlot = maxEntries;
    u32 freeRun   = 0;
    for (u32 i = 0; i < maxEntries; ++i) {
        u8 et = entries[i].entryType;
        if (et == EXFAT_ENTRY_EOD || (et & 0x80U) == 0) {
            if (freeRun == 0) foundSlot = i;
            if (++freeRun >= totalEntriesNeeded) break;
        } else {
            freeRun = 0;
        }
    }

    if (freeRun < totalEntriesNeeded || foundSlot + totalEntriesNeeded > maxEntries) {
        delete[] dirBuf;
        return Status::NoSpace;
    }

    u32 newFirstCluster = 0;
    u64 initialSize     = 0;
    u8  streamFlags     = EXFAT_FLAG_ALLOC_POSSIBLE;

    if (isDirectory) {
        auto clRes = allocateCluster(0);
        if (clRes.isError()) {
            delete[] dirBuf;
            return clRes.status();
        }
        newFirstCluster = clRes.value();
        initialSize     = m_clusterBytes;
    }

    memset(&entries[foundSlot], 0, totalEntriesNeeded * sizeof(ExFatDirEntryRaw));

    auto* fileEntry   = reinterpret_cast<ExFatFileEntry*>(&entries[foundSlot]);
    auto* streamEntry = reinterpret_cast<ExFatStreamEntry*>(&entries[foundSlot + 1]);

    fileEntry->entryType      = EXFAT_ENTRY_FILE;
    fileEntry->secondaryCount = static_cast<u8>(1 + nameEntriesCount);
    fileEntry->fileAttributes = isDirectory ? EXFAT_ATTR_DIRECTORY : EXFAT_ATTR_ARCHIVE;

    u16 upperUtf16[48]{};
    asciiToUpperUtf16(upperUtf16, name, nameLen);

    streamEntry->entryType       = EXFAT_ENTRY_STREAM;
    streamEntry->flags           = streamFlags;
    streamEntry->nameLength      = static_cast<u8>(nameLen);
    streamEntry->nameHash        = computeNameHash(upperUtf16, nameLen);
    streamEntry->validDataLength = initialSize;
    streamEntry->firstCluster    = newFirstCluster;
    streamEntry->dataLength      = initialSize;

    for (u8 n = 0; n < nameEntriesCount; ++n) {
        auto* fn = reinterpret_cast<ExFatNameEntry*>(&entries[foundSlot + 2 + n]);
        fn->entryType = EXFAT_ENTRY_FILENAME;
        fn->flags     = 0;
        usize chunkOffset = static_cast<usize>(n) * 15;
        asciiToUtf16(fn->fileName, name + chunkOffset, 15);
    }

    fileEntry->setChecksum = computeEntrySetChecksum(fileEntry, totalEntriesNeeded);

    st = writeCluster(dirCluster, dirBuf);
    delete[] dirBuf;
    if (st != Status::Ok) return st;

    sync();

    NodeType ntype = isDirectory ? NodeType::Directory : NodeType::File;
    ExFatNode* created = cacheOrCreateNode(name, ntype, newFirstCluster, initialSize,
                                           streamFlags, dirCluster, foundSlot);
    return created ? Result<IVfsNode*>(created) : Result<IVfsNode*>(Status::OutOfMemory);
}

Status ExFatFileSystem::removeEntryInDir(ExFatNode* dirNode, const char* name) noexcept {
    if (!dirNode || !name || !name[0]) return Status::InvalidArgument;

    auto nodeRes = lookupInDir(dirNode, name);
    if (nodeRes.isError()) return nodeRes.status();

    auto* exNode = static_cast<ExFatNode*>(nodeRes.value());

    // If directory, verify it is empty before removal
    if (exNode->getType() == NodeType::Directory) {
        DirEntry dummy{};
        if (readDirEntryAt(exNode, 0, &dummy) == Status::Ok) {
            return Status::DirectoryNotEmpty;
        }
    }

    if (exNode->getFirstCluster() >= EXFAT_CLUSTER_FIRST) {
        freeClusterChain(exNode->getFirstCluster(), exNode->getSize(), exNode->getStreamFlags());
    }

    u8* dirBuf = new u8[m_clusterBytes];
    if (!dirBuf) return Status::OutOfMemory;

    u32 dirCluster = dirNode->getFirstCluster();
    Status st = readCluster(dirCluster, dirBuf);
    if (st != Status::Ok) {
        delete[] dirBuf;
        return st;
    }

    auto* entries = reinterpret_cast<ExFatDirEntryRaw*>(dirBuf);
    u32 slot      = exNode->getDirEntrySlot();
    u8 secCount   = reinterpret_cast<ExFatFileEntry*>(&entries[slot])->secondaryCount;

    for (u32 s = 0; s <= secCount && (slot + s) < (m_clusterBytes / sizeof(ExFatDirEntryRaw)); ++s) {
        entries[slot + s].entryType &= 0x7FU; // Clear InUse bit (bit 7) per exFAT specification
    }

    st = writeCluster(dirCluster, dirBuf);
    delete[] dirBuf;
    if (st != Status::Ok) return st;

    return sync();
}

Status ExFatFileSystem::readDirEntryAt(ExFatNode* dirNode, usize targetIndex, DirEntry* outEntry) noexcept {
    if (!dirNode || !outEntry) return Status::InvalidArgument;

    u8* dirBuf = new u8[m_clusterBytes];
    if (!dirBuf) return Status::OutOfMemory;

    u32 dirCluster = dirNode->getFirstCluster();
    if (readCluster(dirCluster, dirBuf) != Status::Ok) {
        delete[] dirBuf;
        return Status::IoError;
    }

    u32 maxEntries = m_clusterBytes / sizeof(ExFatDirEntryRaw);
    const auto* entries = reinterpret_cast<const ExFatDirEntryRaw*>(dirBuf);
    usize currentMatch = 0;

    for (u32 i = 0; i + 2 < maxEntries; ++i) {
        if (entries[i].entryType == EXFAT_ENTRY_EOD) break;
        if (entries[i].entryType != EXFAT_ENTRY_FILE) continue;

        const auto* fileEntry   = reinterpret_cast<const ExFatFileEntry*>(&entries[i]);
        const auto* streamEntry = reinterpret_cast<const ExFatStreamEntry*>(&entries[i + 1]);
        if (streamEntry->entryType != EXFAT_ENTRY_STREAM) continue;

        if (currentMatch == targetIndex) {
            char nameBuf[64]{};
            usize namePos = 0;
            u8 secCount   = fileEntry->secondaryCount;
            for (u8 s = 2; s <= secCount && (i + s) < maxEntries; ++s) {
                if (entries[i + s].entryType != EXFAT_ENTRY_FILENAME) break;
                const auto* fn = reinterpret_cast<const ExFatNameEntry*>(&entries[i + s]);
                for (usize c = 0; c < 15 && namePos < streamEntry->nameLength && namePos + 1 < sizeof(nameBuf); ++c) {
                    u16 ch = fn->fileName[c];
                    if (ch == 0) break;
                    nameBuf[namePos++] = static_cast<char>(ch & 0x7FU);
                }
            }
            nameBuf[namePos] = '\0';

            str::copy(outEntry->name, nameBuf, sizeof(outEntry->name));
            outEntry->type         = (fileEntry->fileAttributes & EXFAT_ATTR_DIRECTORY)
                                         ? NodeType::Directory
                                         : NodeType::File;
            outEntry->size         = streamEntry->validDataLength;
            outEntry->firstCluster = streamEntry->firstCluster;

            delete[] dirBuf;
            return Status::Ok;
        }

        ++currentMatch;
        i += fileEntry->secondaryCount;
    }

    delete[] dirBuf;
    return Status::NotFound;
}

Status ExFatFileSystem::format(drivers::IBlockDevice* dev, const char* label) {
    if (!dev) return Status::InvalidArgument;

    u32 sectorSize  = dev->getBlockSize();
    u64 totalBlocks = dev->getBlockCount();
    if (totalBlocks < 256) return Status::NoSpace;

    u8 bpsShift = 9;
    if (sectorSize == 4096) bpsShift = 12;
    u8 spcShift = (sectorSize == 4096) ? 0 : 3; // 4096B clusters
    u32 spc     = 1U << spcShift;
    u32 clusterBytes = sectorSize * spc;

    u32 fatOffset  = 32;
    u32 maxClusters = static_cast<u32>((totalBlocks - 128) / spc);
    u32 fatLength  = static_cast<u32>((maxClusters * 4U + sectorSize - 1U) / sectorSize);
    u32 heapOffset = alignUp(fatOffset + fatLength, spc);
    u32 clusterCount = static_cast<u32>((totalBlocks - heapOffset) / spc);

    // 1. Write Main Boot Sector at LBA 0
    CORTEZ_ALIGNED(16) u8 sectorBuf[4096]{};
    auto* bs = reinterpret_cast<ExFatBootSector*>(sectorBuf);
    bs->jumpBoot[0]            = 0xEB;
    bs->jumpBoot[1]            = 0x76;
    bs->jumpBoot[2]            = 0x90;
    memcpy(bs->fsName, "EXFAT   ", 8);
    bs->partitionOffset        = 0;
    bs->volumeLength           = totalBlocks;
    bs->fatOffset              = fatOffset;
    bs->fatLength              = fatLength;
    bs->clusterHeapOffset      = heapOffset;
    bs->clusterCount           = clusterCount;
    bs->rootDirFirstCluster    = 4; // Cluster 2=Bitmap, Cluster 3=Upcase, Cluster 4=RootDir
    bs->volumeSerialNumber     = 0xC047E226U;
    bs->fsRevision             = 0x0100U;
    bs->bytesPerSectorShift    = bpsShift;
    bs->sectorsPerClusterShift = spcShift;
    bs->numberOfFats           = 1;
    bs->bootSignature          = 0xAA55U;

    Status st = dev->writeBlocks(0, 1, sectorBuf);
    if (st != Status::Ok) return st;

    // 2. Initialize FAT first sector (Clusters 0, 1, 2=Bitmap, 3=Upcase, 4=RootDir)
    memset(sectorBuf, 0, sizeof(sectorBuf));
    auto* fatEntries = reinterpret_cast<u32*>(sectorBuf);
    fatEntries[0] = 0xFFFFFFF8U;
    fatEntries[1] = 0xFFFFFFFFU;
    fatEntries[2] = EXFAT_CLUSTER_EOC;
    fatEntries[3] = EXFAT_CLUSTER_EOC;
    fatEntries[4] = EXFAT_CLUSTER_EOC;
    st = dev->writeBlocks(fatOffset, 1, sectorBuf);
    if (st != Status::Ok) return st;

    // 3. Write Allocation Bitmap at Cluster 2 (mark clusters 2, 3, 4 allocated -> 0x07)
    u8* clBuf = new u8[clusterBytes];
    if (!clBuf) return Status::OutOfMemory;
    memset(clBuf, 0, clusterBytes);
    clBuf[0] = 0x07U; // Bits 0, 1, 2 -> Clusters 2, 3, 4 allocated
    dev->writeBlocks(heapOffset + 0 * spc, spc, clBuf);

    // 4. Write Root Directory at Cluster 4 (0x83 Volume Label + 0x81 Allocation Bitmap Entry)
    memset(clBuf, 0, clusterBytes);
    auto* rootEntries = reinterpret_cast<ExFatDirEntryRaw*>(clBuf);

    auto* lbl = reinterpret_cast<ExFatLabelEntry*>(&rootEntries[0]);
    lbl->entryType = EXFAT_ENTRY_LABEL;
    const char* volName = (label && label[0]) ? label : "CORTEZ-OS";
    usize vlen = str::length(volName);
    if (vlen > 11) vlen = 11;
    lbl->charCount = static_cast<u8>(vlen);
    asciiToUtf16(lbl->volumeLabel, volName, vlen);

    auto* bm = reinterpret_cast<ExFatBitmapEntry*>(&rootEntries[1]);
    bm->entryType    = EXFAT_ENTRY_BITMAP;
    bm->bitmapFlags  = 0;
    bm->firstCluster = 2;
    bm->dataLength   = (clusterCount + 7U) / 8U;

    st = dev->writeBlocks(heapOffset + 2 * spc, spc, clBuf);
    delete[] clBuf;
    if (st != Status::Ok) return st;

    dev->flush();
    return mount(dev);
}

} // namespace cortez::fs::exfat
