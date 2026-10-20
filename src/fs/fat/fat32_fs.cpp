/**
 * @file fat32_fs.cpp
 * @brief Implementation of memory-safe, lazy-loading FAT32 Filesystem driver.
 */

#include "fat32_fs.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::fs::fat {

namespace {

void sfnToFilename(const char* sfn11, char* out, usize maxOut) noexcept {
    if (!sfn11 || !out || maxOut < 2) return;

    usize pos = 0;
    // 8 characters base name
    for (usize i = 0; i < 8 && pos + 1 < maxOut; ++i) {
        if (sfn11[i] == ' ') break;
        out[pos++] = static_cast<char>(str::toLower(sfn11[i]));
    }

    // 3 characters extension
    if (sfn11[8] != ' ' && pos + 2 < maxOut) {
        out[pos++] = '.';
        for (usize i = 8; i < 11 && pos + 1 < maxOut; ++i) {
            if (sfn11[i] == ' ') break;
            out[pos++] = static_cast<char>(str::toLower(sfn11[i]));
        }
    }
    out[pos] = '\0';
}

void filenameToSfn(const char* name, char* sfn11) noexcept {
    memset(sfn11, ' ', 11);
    if (!name || !name[0]) return;

    const char* dot = str::findLastChar(name, '.');
    usize baseLen   = dot ? static_cast<usize>(dot - name) : str::length(name);
    if (baseLen > 8) baseLen = 8;

    for (usize i = 0; i < baseLen; ++i) {
        sfn11[i] = static_cast<char>(str::toUpper(name[i]));
    }

    if (dot) {
        const char* ext = dot + 1;
        usize extLen = str::length(ext);
        if (extLen > 3) extLen = 3;
        for (usize i = 0; i < extLen; ++i) {
            sfn11[8 + i] = static_cast<char>(str::toUpper(ext[i]));
        }
    }
}

[[maybe_unused]] u8 computeSfnChecksum(const char* sfn11) noexcept {
    u8 sum = 0;
    for (int i = 0; i < 11; ++i) {
        sum = static_cast<u8>(((sum & 1U) ? 0x80U : 0U) + (sum >> 1U) + static_cast<u8>(sfn11[i]));
    }
    return sum;
}

} // namespace

// =============================================================================
// Fat32Node Implementation
// =============================================================================

Fat32Node::Fat32Node(Fat32FileSystem* fs,
                     const char* name,
                     NodeType type,
                     u32 firstCluster,
                     u64 size,
                     u32 parentDirCluster,
                     u32 dirEntryIndex) noexcept
    : m_fs(fs),
      m_type(type),
      m_firstCluster(firstCluster),
      m_size(size),
      m_parentCluster(parentDirCluster),
      m_dirSlotIndex(dirEntryIndex) {
    str::copy(m_name, name ? name : "", sizeof(m_name));
}

Result<usize> Fat32Node::read(u64 offset, void* buffer, usize size) {
    if (m_type != NodeType::File) return Status::IsDirectory;
    if (!buffer) return Status::InvalidArgument;
    if (offset >= m_size || size == 0 || m_firstCluster < FAT32_CLUSTER_FIRST) {
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
        currCluster = m_fs->getNextCluster(currCluster);
        if (currCluster < FAT32_CLUSTER_FIRST || currCluster >= FAT32_CLUSTER_EOC_MIN) {
            delete[] clusterBuf;
            return Status::IoError;
        }
    }

    usize bytesRead = 0;
    u32   inCluster = static_cast<u32>(offset % clusterSize);
    u8*   outPtr    = static_cast<u8*>(buffer);

    while (bytesRead < toRead && currCluster >= FAT32_CLUSTER_FIRST && currCluster < FAT32_CLUSTER_EOC_MIN) {
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
            currCluster = m_fs->getNextCluster(currCluster);
        }
    }

    delete[] clusterBuf;
    return bytesRead;
}

Result<usize> Fat32Node::write(u64 offset, const void* buffer, usize size) {
    if (m_type != NodeType::File) return Status::IsDirectory;
    if (!buffer) return Status::InvalidArgument;
    if (size == 0) return usize(0);

    u32 clusterSize = m_fs->getClusterSize();
    u8* clusterBuf  = new u8[clusterSize];
    if (!clusterBuf) return Status::OutOfMemory;

    // Allocate first cluster if file was empty
    if (m_firstCluster < FAT32_CLUSTER_FIRST) {
        auto allocRes = m_fs->allocateCluster(0);
        if (allocRes.isError()) {
            delete[] clusterBuf;
            return allocRes.status();
        }
        m_firstCluster = allocRes.value();
    }

    u32 currCluster = m_firstCluster;
    u64 targetClusterIdx = offset / clusterSize;
    for (u64 i = 0; i < targetClusterIdx; ++i) {
        u32 next = m_fs->getNextCluster(currCluster);
        if (next < FAT32_CLUSTER_FIRST || next >= FAT32_CLUSTER_EOC_MIN) {
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
            u32 next = m_fs->getNextCluster(currCluster);
            if (next < FAT32_CLUSTER_FIRST || next >= FAT32_CLUSTER_EOC_MIN) {
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

Status Fat32Node::truncate(u64 newSize) {
    if (m_type != NodeType::File) return Status::IsDirectory;
    if (newSize == 0) {
        if (m_firstCluster >= FAT32_CLUSTER_FIRST) {
            m_fs->freeClusterChain(m_firstCluster);
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

Result<IVfsNode*> Fat32Node::lookup(const char* name) {
    if (m_type != NodeType::Directory) return Status::NotDirectory;
    return m_fs->lookupInDir(this, name);
}

Result<IVfsNode*> Fat32Node::createFile(const char* name) {
    if (m_type != NodeType::Directory) return Status::NotDirectory;
    return m_fs->createEntryInDir(this, name, false);
}

Result<IVfsNode*> Fat32Node::createDir(const char* name) {
    if (m_type != NodeType::Directory) return Status::NotDirectory;
    return m_fs->createEntryInDir(this, name, true);
}

Status Fat32Node::remove(const char* name) {
    if (m_type != NodeType::Directory) return Status::NotDirectory;
    return m_fs->removeEntryInDir(this, name);
}

Status Fat32Node::readDir(usize index, DirEntry* outEntry) {
    if (m_type != NodeType::Directory) return Status::NotDirectory;
    return m_fs->readDirEntryAt(this, index, outEntry);
}

Status Fat32Node::stat(FileStat* outStat) const noexcept {
    if (!outStat) return Status::InvalidArgument;
    str::copy(outStat->name, m_name, sizeof(outStat->name));
    outStat->type         = m_type;
    outStat->size         = m_size;
    outStat->firstCluster = m_firstCluster;
    u32 clSize            = m_fs->getClusterSize();
    outStat->blocks       = static_cast<u32>((m_size + clSize - 1) / clSize);
    return Status::Ok;
}

Status Fat32Node::sync() {
    return m_fs->sync();
}

// =============================================================================
// Fat32FileSystem Implementation
// =============================================================================

Fat32FileSystem::~Fat32FileSystem() {
    for (usize i = 0; i < m_nodeCacheCount; ++i) {
        delete m_nodeCache[i];
        m_nodeCache[i] = nullptr;
    }
    delete m_rootNode;
    m_rootNode = nullptr;
}

u64 Fat32FileSystem::clusterToLba(u32 cluster) const noexcept {
    if (cluster < FAT32_CLUSTER_FIRST) return m_dataStartLba;
    return static_cast<u64>(m_dataStartLba) +
           static_cast<u64>(cluster - FAT32_CLUSTER_FIRST) * m_sectorsPerCluster;
}

Status Fat32FileSystem::readCluster(u32 cluster, void* buffer) noexcept {
    if (!m_device || !buffer || cluster < FAT32_CLUSTER_FIRST) {
        return Status::InvalidArgument;
    }
    return m_device->readBlocks(clusterToLba(cluster), m_sectorsPerCluster, buffer);
}

Status Fat32FileSystem::writeCluster(u32 cluster, const void* buffer) noexcept {
    if (!m_device || !buffer || cluster < FAT32_CLUSTER_FIRST) {
        return Status::InvalidArgument;
    }
    return m_device->writeBlocks(clusterToLba(cluster), m_sectorsPerCluster, buffer);
}

u32 Fat32FileSystem::readFatEntry(u32 cluster) noexcept {
    if (!m_device || cluster < FAT32_CLUSTER_FIRST || cluster >= m_clusterCount + FAT32_CLUSTER_FIRST) {
        return FAT32_CLUSTER_EOC;
    }

    u64 byteOffset     = static_cast<u64>(cluster) * 4ULL;
    u32 fatSector      = m_fatStartLba + static_cast<u32>(byteOffset / m_bytesPerSector);
    u32 offsetInSector = static_cast<u32>(byteOffset % m_bytesPerSector);

    if (m_cachedFatSector != fatSector) {
        flushFatCache();
        if (m_device->readBlocks(fatSector, 1, m_fatCache) != Status::Ok) {
            return FAT32_CLUSTER_EOC;
        }
        m_cachedFatSector = fatSector;
        m_fatCacheDirty   = false;
    }

    u32 entry = *reinterpret_cast<const u32*>(m_fatCache + offsetInSector);
    return entry & 0x0FFFFFFFU;
}

Status Fat32FileSystem::writeFatEntry(u32 cluster, u32 nextVal) noexcept {
    if (!m_device || cluster < FAT32_CLUSTER_FIRST || cluster >= m_clusterCount + FAT32_CLUSTER_FIRST) {
        return Status::InvalidArgument;
    }

    u64 byteOffset     = static_cast<u64>(cluster) * 4ULL;
    u32 fatSector      = m_fatStartLba + static_cast<u32>(byteOffset / m_bytesPerSector);
    u32 offsetInSector = static_cast<u32>(byteOffset % m_bytesPerSector);

    if (m_cachedFatSector != fatSector) {
        flushFatCache();
        if (m_device->readBlocks(fatSector, 1, m_fatCache) != Status::Ok) {
            return Status::IoError;
        }
        m_cachedFatSector = fatSector;
        m_fatCacheDirty   = false;
    }

    u32 oldVal = *reinterpret_cast<const u32*>(m_fatCache + offsetInSector);
    *reinterpret_cast<u32*>(m_fatCache + offsetInSector) = (oldVal & 0xF0000000U) | (nextVal & 0x0FFFFFFFU);
    m_fatCacheDirty = true;
    return Status::Ok;
}

Status Fat32FileSystem::flushFatCache() noexcept {
    if (!m_fatCacheDirty || m_cachedFatSector == 0xFFFFFFFFU || !m_device) {
        return Status::Ok;
    }

    Status st = m_device->writeBlocks(m_cachedFatSector, 1, m_fatCache);
    if (st == Status::Ok && m_fatCount > 1) {
        u32 fat2Sector = m_cachedFatSector + m_fatSizeSectors;
        (void)m_device->writeBlocks(fat2Sector, 1, m_fatCache);
    }
    m_fatCacheDirty = false;
    return st;
}

Result<u32> Fat32FileSystem::allocateCluster(u32 prevCluster) noexcept {
    for (u32 c = FAT32_CLUSTER_FIRST; c < m_clusterCount + FAT32_CLUSTER_FIRST; ++c) {
        if (readFatEntry(c) == FAT32_CLUSTER_FREE) {
            writeFatEntry(c, FAT32_CLUSTER_EOC);
            if (prevCluster >= FAT32_CLUSTER_FIRST) {
                writeFatEntry(prevCluster, c);
            }
            flushFatCache();

            // Zero out newly allocated cluster
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

Status Fat32FileSystem::freeClusterChain(u32 startCluster) noexcept {
    if (startCluster < FAT32_CLUSTER_FIRST) return Status::Ok;

    u32 curr = startCluster;
    u32 guard = 0;
    while (curr >= FAT32_CLUSTER_FIRST && curr < FAT32_CLUSTER_EOC_MIN && guard++ < m_clusterCount) {
        u32 next = readFatEntry(curr);
        writeFatEntry(curr, FAT32_CLUSTER_FREE);
        curr = next;
    }
    flushFatCache();
    return Status::Ok;
}

Fat32Node* Fat32FileSystem::cacheOrCreateNode(const char* name,
                                              NodeType type,
                                              u32 firstCluster,
                                              u64 size,
                                              u32 parentCluster,
                                              u32 slotIndex) noexcept {
    for (usize i = 0; i < m_nodeCacheCount; ++i) {
        Fat32Node* existing = m_nodeCache[i];
        if (existing &&
            existing->getParentCluster() == parentCluster &&
            existing->getDirSlotIndex() == slotIndex &&
            str::compareCaseInsensitive(existing->getName(), name) == 0) {
            existing->updateMetadata(firstCluster, size);
            return existing;
        }
    }

    auto* node = new Fat32Node(this, name, type, firstCluster, size, parentCluster, slotIndex);
    if (node && m_nodeCacheCount < MAX_OPEN_NODES) {
        m_nodeCache[m_nodeCacheCount++] = node;
    }
    return node;
}

Status Fat32FileSystem::mount(drivers::IBlockDevice* dev) {
    if (!dev) return Status::InvalidArgument;

    m_device = dev;

    CORTEZ_ALIGNED(16) u8 bootBuf[4096]{};
    Status st = m_device->readBlocks(0, 1, bootBuf);
    if (st != Status::Ok) return st;

    const auto* bs = reinterpret_cast<const FatBootSector*>(bootBuf);
    u16 bootSig = *reinterpret_cast<const u16*>(bootBuf + 510);
    if (bootSig != 0xAA55U) return Status::CorruptedFs;

    m_bytesPerSector    = bs->bytesPerSector;
    m_sectorsPerCluster = bs->sectorsPerCluster;
    if (m_bytesPerSector < 512 || m_bytesPerSector > 4096 || m_sectorsPerCluster == 0) {
        return Status::CorruptedFs;
    }

    m_clusterBytes      = m_bytesPerSector * m_sectorsPerCluster;
    m_reservedSectors   = bs->reservedSectors;
    m_fatCount          = (bs->numFats >= 1) ? bs->numFats : 2;
    m_fatStartLba       = m_reservedSectors;
    m_fatSizeSectors    = bs->fatSize32;
    m_rootCluster       = bs->rootDirFirstCluster;

    if (m_fatSizeSectors == 0 || m_rootCluster < FAT32_CLUSTER_FIRST) {
        return Status::CorruptedFs;
    }

    m_dataStartLba = m_fatStartLba + (m_fatCount * m_fatSizeSectors);
    u64 totalSectors = (bs->totalSectors16 != 0) ? bs->totalSectors16 : bs->totalSectors32;
    if (totalSectors <= m_dataStartLba) {
        totalSectors = dev->getBlockCount();
    }
    m_clusterCount = static_cast<u32>((totalSectors - m_dataStartLba) / m_sectorsPerCluster);

    // Extract volume label
    char label[12]{};
    for (usize i = 0; i < 11; ++i) {
        label[i] = bs->volumeLabel[i];
    }
    label[11] = '\0';
    for (int i = 10; i >= 0 && label[i] == ' '; --i) {
        label[i] = '\0';
    }
    str::copy(m_volumeLabel, label[0] ? label : "FAT32-VOL", sizeof(m_volumeLabel));

    m_cachedFatSector = 0xFFFFFFFFU;
    m_fatCacheDirty   = false;

    m_rootNode = new Fat32Node(this, "/", NodeType::Directory, m_rootCluster, 0, 0, 0);

    pr_dbg("fat32", "Mounted physical FAT32 volume '%s' on /dev/%s (%u clusters x %u B)",
          m_volumeLabel, m_device->getName(), m_clusterCount, m_clusterBytes);
    return Status::Ok;
}

Status Fat32FileSystem::unmount() {
    return sync();
}

Status Fat32FileSystem::sync() {
    Status st = flushFatCache();
    if (st != Status::Ok) return st;
    return m_device ? m_device->flush() : Status::Ok;
}

Status Fat32FileSystem::statFs(FsStat* outStat) const noexcept {
    if (!outStat) return Status::InvalidArgument;
    str::copy(outStat->fsType, "fat32", sizeof(outStat->fsType));
    str::copy(outStat->volumeLabel, m_volumeLabel, sizeof(outStat->volumeLabel));
    outStat->clusterSize   = m_clusterBytes;
    outStat->totalClusters = m_clusterCount;
    outStat->freeClusters  = m_clusterCount; // Bounded fast estimation to avoid freezing
    outStat->totalBytes    = static_cast<u64>(m_clusterCount) * m_clusterBytes;
    outStat->freeBytes     = outStat->totalBytes;
    outStat->usedBytes     = 0;
    return Status::Ok;
}

Result<IVfsNode*> Fat32FileSystem::lookupInDir(Fat32Node* dirNode, const char* targetName) noexcept {
    if (!dirNode || !targetName || !targetName[0]) return Status::InvalidArgument;

    u8* clusterBuf = new u8[m_clusterBytes];
    if (!clusterBuf) return Status::OutOfMemory;

    u32 currCluster = dirNode->getFirstCluster();
    u32 entriesPerCluster = m_clusterBytes / sizeof(FatDirEntryRaw);
    char lfnBuf[256]{};

    while (currCluster >= FAT32_CLUSTER_FIRST && currCluster < FAT32_CLUSTER_EOC_MIN) {
        if (readCluster(currCluster, clusterBuf) != Status::Ok) {
            delete[] clusterBuf;
            return Status::IoError;
        }

        const auto* rawEntries = reinterpret_cast<const FatDirEntryRaw*>(clusterBuf);
        for (u32 i = 0; i < entriesPerCluster; ++i) {
            u8 firstByte = static_cast<u8>(rawEntries[i].name[0]);
            if (firstByte == FAT_ENTRY_LAST_FREE) {
                delete[] clusterBuf;
                return Status::NotFound;
            }
            if (firstByte == FAT_ENTRY_FREE_OR_DELETED) {
                lfnBuf[0] = '\0';
                continue;
            }

            u8 attr = rawEntries[i].attr;
            if (attr == FAT_ATTR_LFN) {
                const auto* lfn = reinterpret_cast<const FatLfnEntryRaw*>(&rawEntries[i]);
                u8 seq = lfn->order & 0x1FU;
                if (seq >= 1 && seq <= 20) {
                    usize charBase = static_cast<usize>(seq - 1) * 13;
                    usize p = 0;
                    for (usize c = 0; c < 5; ++c) {
                        u16 ch = lfn->name1[c];
                        if (ch == 0 || ch == 0xFFFF) break;
                        if (charBase + p + 1 < sizeof(lfnBuf)) lfnBuf[charBase + p++] = static_cast<char>(ch & 0x7FU);
                    }
                    for (usize c = 0; c < 6; ++c) {
                        u16 ch = lfn->name2[c];
                        if (ch == 0 || ch == 0xFFFF) break;
                        if (charBase + p + 1 < sizeof(lfnBuf)) lfnBuf[charBase + p++] = static_cast<char>(ch & 0x7FU);
                    }
                    for (usize c = 0; c < 2; ++c) {
                        u16 ch = lfn->name3[c];
                        if (ch == 0 || ch == 0xFFFF) break;
                        if (charBase + p + 1 < sizeof(lfnBuf)) lfnBuf[charBase + p++] = static_cast<char>(ch & 0x7FU);
                    }
                }
                continue;
            }

            if (attr & FAT_ATTR_VOLUME_ID) {
                lfnBuf[0] = '\0';
                continue;
            }

            char fileName[64]{};
            if (lfnBuf[0] != '\0') {
                str::copy(fileName, lfnBuf, sizeof(fileName));
                lfnBuf[0] = '\0';
            } else {
                sfnToFilename(rawEntries[i].name, fileName, sizeof(fileName));
            }

            if (str::compareCaseInsensitive(fileName, targetName) == 0) {
                NodeType ntype = (attr & FAT_ATTR_DIRECTORY) ? NodeType::Directory : NodeType::File;
                u32 firstCl = (static_cast<u32>(rawEntries[i].firstClusterHigh) << 16) | rawEntries[i].firstClusterLow;
                u64 fSize   = rawEntries[i].fileSize;

                Fat32Node* node = cacheOrCreateNode(fileName, ntype, firstCl, fSize, currCluster, i);
                delete[] clusterBuf;
                return node ? Result<IVfsNode*>(node) : Result<IVfsNode*>(Status::OutOfMemory);
            }
        }

        currCluster = getNextCluster(currCluster);
    }

    delete[] clusterBuf;
    return Status::NotFound;
}

Result<IVfsNode*> Fat32FileSystem::createEntryInDir(Fat32Node* dirNode, const char* name, bool isDirectory) noexcept {
    if (!dirNode || !name || !name[0]) return Status::InvalidArgument;
    if (lookupInDir(dirNode, name).isOk()) return Status::AlreadyExists;

    u8* clusterBuf = new u8[m_clusterBytes];
    if (!clusterBuf) return Status::OutOfMemory;

    u32 currCluster = dirNode->getFirstCluster();
    u32 prevCluster = 0;
    u32 entriesPerCluster = m_clusterBytes / sizeof(FatDirEntryRaw);
    u32 targetSlot = entriesPerCluster;
    u32 slotCluster = 0;

    while (currCluster >= FAT32_CLUSTER_FIRST && currCluster < FAT32_CLUSTER_EOC_MIN) {
        if (readCluster(currCluster, clusterBuf) != Status::Ok) {
            delete[] clusterBuf;
            return Status::IoError;
        }

        auto* rawEntries = reinterpret_cast<FatDirEntryRaw*>(clusterBuf);
        for (u32 i = 0; i < entriesPerCluster; ++i) {
            u8 b = static_cast<u8>(rawEntries[i].name[0]);
            if (b == FAT_ENTRY_LAST_FREE || b == FAT_ENTRY_FREE_OR_DELETED) {
                targetSlot  = i;
                slotCluster = currCluster;
                break;
            }
        }
        if (targetSlot < entriesPerCluster) break;

        prevCluster = currCluster;
        currCluster = getNextCluster(currCluster);
    }

    // If directory has no free slots, allocate a new cluster
    if (targetSlot >= entriesPerCluster) {
        auto clRes = allocateCluster(prevCluster);
        if (clRes.isError()) {
            delete[] clusterBuf;
            return clRes.status();
        }
        slotCluster = clRes.value();
        targetSlot  = 0;
        memset(clusterBuf, 0, m_clusterBytes);
    }

    char sfn[11];
    filenameToSfn(name, sfn);

    u32 newFirstCluster = 0;
    if (isDirectory) {
        auto clRes = allocateCluster(0);
        if (clRes.isError()) {
            delete[] clusterBuf;
            return clRes.status();
        }
        newFirstCluster = clRes.value();

        // Write '.' and '..' directory entries in new directory cluster
        u8* subDirBuf = new u8[m_clusterBytes];
        if (subDirBuf) {
            memset(subDirBuf, 0, m_clusterBytes);
            auto* subEntries = reinterpret_cast<FatDirEntryRaw*>(subDirBuf);
            // '.' entry
            memset(subEntries[0].name, ' ', 11);
            subEntries[0].name[0]           = '.';
            subEntries[0].attr              = FAT_ATTR_DIRECTORY;
            subEntries[0].firstClusterHigh  = static_cast<u16>(newFirstCluster >> 16);
            subEntries[0].firstClusterLow   = static_cast<u16>(newFirstCluster & 0xFFFFU);

            // '..' entry
            memset(subEntries[1].name, ' ', 11);
            subEntries[1].name[0]           = '.';
            subEntries[1].name[1]           = '.';
            subEntries[1].attr              = FAT_ATTR_DIRECTORY;
            u32 parentCluster = (dirNode->getFirstCluster() == m_rootCluster) ? 0 : dirNode->getFirstCluster();
            subEntries[1].firstClusterHigh  = static_cast<u16>(parentCluster >> 16);
            subEntries[1].firstClusterLow   = static_cast<u16>(parentCluster & 0xFFFFU);

            writeCluster(newFirstCluster, subDirBuf);
            delete[] subDirBuf;
        }
    }

    auto* rawEntries = reinterpret_cast<FatDirEntryRaw*>(clusterBuf);
    memset(&rawEntries[targetSlot], 0, sizeof(FatDirEntryRaw));
    memcpy(rawEntries[targetSlot].name, sfn, 11);
    rawEntries[targetSlot].attr             = isDirectory ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    rawEntries[targetSlot].firstClusterHigh = static_cast<u16>(newFirstCluster >> 16);
    rawEntries[targetSlot].firstClusterLow  = static_cast<u16>(newFirstCluster & 0xFFFFU);
    rawEntries[targetSlot].fileSize         = 0;

    Status st = writeCluster(slotCluster, clusterBuf);
    delete[] clusterBuf;
    if (st != Status::Ok) return st;

    sync();

    NodeType ntype = isDirectory ? NodeType::Directory : NodeType::File;
    Fat32Node* node = cacheOrCreateNode(name, ntype, newFirstCluster, 0, slotCluster, targetSlot);
    return node ? Result<IVfsNode*>(node) : Result<IVfsNode*>(Status::OutOfMemory);
}

Status Fat32FileSystem::removeEntryInDir(Fat32Node* dirNode, const char* name) noexcept {
    if (!dirNode || !name || !name[0]) return Status::InvalidArgument;

    auto nodeRes = lookupInDir(dirNode, name);
    if (nodeRes.isError()) return nodeRes.status();

    auto* targetNode = static_cast<Fat32Node*>(nodeRes.value());
    if (targetNode->getType() == NodeType::Directory) {
        DirEntry dummy{};
        if (readDirEntryAt(targetNode, 0, &dummy) == Status::Ok) {
            return Status::DirectoryNotEmpty;
        }
    }

    if (targetNode->getFirstCluster() >= FAT32_CLUSTER_FIRST) {
        freeClusterChain(targetNode->getFirstCluster());
    }

    u8* clusterBuf = new u8[m_clusterBytes];
    if (!clusterBuf) return Status::OutOfMemory;

    u32 parentCl = targetNode->getParentCluster();
    Status st = readCluster(parentCl, clusterBuf);
    if (st != Status::Ok) {
        delete[] clusterBuf;
        return st;
    }

    auto* rawEntries = reinterpret_cast<FatDirEntryRaw*>(clusterBuf);
    u32 slot = targetNode->getDirSlotIndex();
    rawEntries[slot].name[0] = static_cast<char>(FAT_ENTRY_FREE_OR_DELETED);

    // Also mark any preceding LFN entries as deleted
    int lfnSlot = static_cast<int>(slot) - 1;
    while (lfnSlot >= 0 && rawEntries[lfnSlot].attr == FAT_ATTR_LFN) {
        rawEntries[lfnSlot].name[0] = static_cast<char>(FAT_ENTRY_FREE_OR_DELETED);
        --lfnSlot;
    }

    st = writeCluster(parentCl, clusterBuf);
    delete[] clusterBuf;
    sync();
    return st;
}

Status Fat32FileSystem::readDirEntryAt(Fat32Node* dirNode, usize targetIndex, DirEntry* outEntry) noexcept {
    if (!dirNode || !outEntry) return Status::InvalidArgument;

    u8* clusterBuf = new u8[m_clusterBytes];
    if (!clusterBuf) return Status::OutOfMemory;

    u32 currCluster = dirNode->getFirstCluster();
    u32 entriesPerCluster = m_clusterBytes / sizeof(FatDirEntryRaw);
    usize currentMatch = 0;
    char lfnBuf[256]{};

    while (currCluster >= FAT32_CLUSTER_FIRST && currCluster < FAT32_CLUSTER_EOC_MIN) {
        if (readCluster(currCluster, clusterBuf) != Status::Ok) {
            delete[] clusterBuf;
            return Status::IoError;
        }

        const auto* rawEntries = reinterpret_cast<const FatDirEntryRaw*>(clusterBuf);
        for (u32 i = 0; i < entriesPerCluster; ++i) {
            u8 firstByte = static_cast<u8>(rawEntries[i].name[0]);
            if (firstByte == FAT_ENTRY_LAST_FREE) {
                delete[] clusterBuf;
                return Status::NotFound;
            }
            if (firstByte == FAT_ENTRY_FREE_OR_DELETED) {
                lfnBuf[0] = '\0';
                continue;
            }

            u8 attr = rawEntries[i].attr;
            if (attr == FAT_ATTR_LFN) {
                const auto* lfn = reinterpret_cast<const FatLfnEntryRaw*>(&rawEntries[i]);
                u8 seq = lfn->order & 0x1FU;
                if (seq >= 1 && seq <= 20) {
                    usize charBase = static_cast<usize>(seq - 1) * 13;
                    usize p = 0;
                    for (usize c = 0; c < 5; ++c) {
                        u16 ch = lfn->name1[c];
                        if (ch == 0 || ch == 0xFFFF) break;
                        if (charBase + p + 1 < sizeof(lfnBuf)) lfnBuf[charBase + p++] = static_cast<char>(ch & 0x7FU);
                    }
                    for (usize c = 0; c < 6; ++c) {
                        u16 ch = lfn->name2[c];
                        if (ch == 0 || ch == 0xFFFF) break;
                        if (charBase + p + 1 < sizeof(lfnBuf)) lfnBuf[charBase + p++] = static_cast<char>(ch & 0x7FU);
                    }
                    for (usize c = 0; c < 2; ++c) {
                        u16 ch = lfn->name3[c];
                        if (ch == 0 || ch == 0xFFFF) break;
                        if (charBase + p + 1 < sizeof(lfnBuf)) lfnBuf[charBase + p++] = static_cast<char>(ch & 0x7FU);
                    }
                }
                continue;
            }

            if (attr & FAT_ATTR_VOLUME_ID) {
                lfnBuf[0] = '\0';
                continue;
            }

            // Skip '.' and '..' in directory listings for clean root navigation
            if (rawEntries[i].name[0] == '.') {
                lfnBuf[0] = '\0';
                continue;
            }

            if (currentMatch == targetIndex) {
                if (lfnBuf[0] != '\0') {
                    str::copy(outEntry->name, lfnBuf, sizeof(outEntry->name));
                } else {
                    sfnToFilename(rawEntries[i].name, outEntry->name, sizeof(outEntry->name));
                }
                outEntry->type         = (attr & FAT_ATTR_DIRECTORY) ? NodeType::Directory : NodeType::File;
                outEntry->size         = rawEntries[i].fileSize;
                outEntry->firstCluster = (static_cast<u32>(rawEntries[i].firstClusterHigh) << 16) | rawEntries[i].firstClusterLow;

                delete[] clusterBuf;
                return Status::Ok;
            }

            ++currentMatch;
            lfnBuf[0] = '\0';
        }

        currCluster = getNextCluster(currCluster);
    }

    delete[] clusterBuf;
    return Status::NotFound;
}

Status Fat32FileSystem::updateDirectoryEntryOnDisk(Fat32Node* node) noexcept {
    if (!node || node->getParentCluster() < FAT32_CLUSTER_FIRST) {
        return Status::Ok; // Root directory has no parent entry
    }

    u8* clusterBuf = new u8[m_clusterBytes];
    if (!clusterBuf) return Status::OutOfMemory;

    u32 parentCl = node->getParentCluster();
    Status st = readCluster(parentCl, clusterBuf);
    if (st != Status::Ok) {
        delete[] clusterBuf;
        return st;
    }

    auto* rawEntries = reinterpret_cast<FatDirEntryRaw*>(clusterBuf);
    u32 slot = node->getDirSlotIndex();
    rawEntries[slot].firstClusterHigh = static_cast<u16>(node->getFirstCluster() >> 16);
    rawEntries[slot].firstClusterLow  = static_cast<u16>(node->getFirstCluster() & 0xFFFFU);
    rawEntries[slot].fileSize         = static_cast<u32>(node->getSize());

    st = writeCluster(parentCl, clusterBuf);
    delete[] clusterBuf;
    return st;
}

Status Fat32FileSystem::format(drivers::IBlockDevice* dev, const char* label) {
    if (!dev) return Status::InvalidArgument;
    u64 totalBlocks = dev->getBlockCount();
    if (totalBlocks < 65536) return Status::NoSpace;

    u32 sectorSize  = dev->getBlockSize();
    u8  spc         = (sectorSize == 4096) ? 1 : 8; // 4096B cluster
    u32 reserved    = 32;
    u32 numFats     = 2;
    u32 dataBlocks  = static_cast<u32>(totalBlocks - reserved);
    u32 fatSize     = ((dataBlocks / spc * 4U) + sectorSize - 1U) / sectorSize;

    CORTEZ_ALIGNED(16) u8 sectorBuf[4096]{};
    auto* bs = reinterpret_cast<FatBootSector*>(sectorBuf);
    bs->jumpBoot[0]            = 0xEB;
    bs->jumpBoot[1]            = 0x58;
    bs->jumpBoot[2]            = 0x90;
    memcpy(bs->oemName, "MSDOS5.0", 8);
    bs->bytesPerSector         = static_cast<u16>(sectorSize);
    bs->sectorsPerCluster      = spc;
    bs->reservedSectors        = static_cast<u16>(reserved);
    bs->numFats                = numFats;
    bs->mediaType              = 0xF8;
    bs->totalSectors32         = static_cast<u32>(totalBlocks);
    bs->fatSize32              = fatSize;
    bs->rootDirFirstCluster    = 2;
    bs->fsInfoSector           = 1;
    bs->backupBootSector       = 6;
    bs->bootSig                = 0x29;
    bs->volumeId               = 0xC047E226U;

    const char* volName = (label && label[0]) ? label : "CORTEZ-FAT";
    memset(bs->volumeLabel, ' ', 11);
    usize vlen = str::length(volName);
    if (vlen > 11) vlen = 11;
    for (usize i = 0; i < vlen; ++i) bs->volumeLabel[i] = static_cast<char>(str::toUpper(volName[i]));
    memcpy(bs->fsType, "FAT32   ", 8);

    *reinterpret_cast<u16*>(sectorBuf + 510) = 0xAA55U;
    Status st = dev->writeBlocks(0, 1, sectorBuf);
    if (st != Status::Ok) return st;

    // Initialize FAT entries for Clusters 0, 1, and 2 (root directory)
    memset(sectorBuf, 0, sizeof(sectorBuf));
    auto* fat = reinterpret_cast<u32*>(sectorBuf);
    fat[0] = 0x0FFFFFF8U; // Media type
    fat[1] = 0x0FFFFFFFU; // Clean shutdown / hard error flags
    fat[2] = 0x0FFFFFFFU; // Root dir EOC

    dev->writeBlocks(reserved, 1, sectorBuf);
    if (numFats > 1) {
        dev->writeBlocks(reserved + fatSize, 1, sectorBuf);
    }

    // Zero out root directory cluster
    u32 rootLba = reserved + (numFats * fatSize);
    u8* clBuf = new u8[sectorSize * spc];
    if (clBuf) {
        memset(clBuf, 0, sectorSize * spc);
        dev->writeBlocks(rootLba, spc, clBuf);
        delete[] clBuf;
    }

    dev->flush();
    return mount(dev);
}

} // namespace cortez::fs::fat
