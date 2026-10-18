/**
 * @file exfat_fs.hpp
 * @brief Physical exFAT Filesystem and VFS Node implementation for NVMe partitions.
 */

#pragma once

#include "../i_filesystem.hpp"
#include "exfat_structs.hpp"
#include "../../lib/spinlock.hpp"

namespace cortez::fs::exfat {

class ExFatFileSystem;

/**
 * @brief Represents an on-disk exFAT file or directory node (inode).
 */
class ExFatNode final : public IVfsNode {
public:
    ExFatNode(ExFatFileSystem* fs,
              const char* name,
              NodeType type,
              u32 firstCluster,
              u64 dataSize,
              u8 streamFlags,
              u32 parentDirCluster,
              u32 dirEntrySlotIndex) noexcept;

    ~ExFatNode() override = default;

    const char*   getName() const noexcept override { return m_name; }
    NodeType      getType() const noexcept override { return m_type; }
    u64           getSize() const noexcept override { return m_size; }

    Result<usize> read(u64 offset, void* buffer, usize size) override;
    Result<usize> write(u64 offset, const void* buffer, usize size) override;
    Status        truncate(u64 newSize) override;

    Result<IVfsNode*> lookup(const char* name) override;
    Result<IVfsNode*> createFile(const char* name) override;
    Result<IVfsNode*> createDir(const char* name) override;
    Status            remove(const char* name) override;
    Status            readDir(usize index, DirEntry* outEntry) override;

    Status        stat(FileStat* outStat) const noexcept override;
    Status        sync() override;

    u32 getFirstCluster() const noexcept { return m_firstCluster; }
    u8  getStreamFlags() const noexcept { return m_streamFlags; }
    u32 getParentDirCluster() const noexcept { return m_parentCluster; }
    u32 getDirEntrySlot() const noexcept { return m_dirSlotIndex; }

    void updateMetadata(u32 firstCluster, u64 newSize, u8 streamFlags) noexcept {
        m_firstCluster = firstCluster;
        m_size         = newSize;
        m_streamFlags  = streamFlags;
    }

private:
    ExFatFileSystem* m_fs            = nullptr;
    char             m_name[64]{};
    NodeType         m_type          = NodeType::File;
    u32              m_firstCluster  = 0;
    u64              m_size          = 0;
    u8               m_streamFlags   = EXFAT_FLAG_ALLOC_POSSIBLE;
    u32              m_parentCluster = 0;
    u32              m_dirSlotIndex  = 0;
};

/**
 * @brief Physical exFAT Filesystem driver operating directly on an IBlockDevice.
 */
class ExFatFileSystem final : public IFileSystem {
public:
    static constexpr usize MAX_OPEN_NODES = 128;

    ExFatFileSystem() noexcept = default;
    ~ExFatFileSystem() override = default;

    const char*            getFsName() const noexcept override { return "exfat"; }
    Status                 mount(drivers::IBlockDevice* dev) override;
    Status                 unmount() override;
    IVfsNode*              getRootNode() noexcept override { return m_rootNode; }
    drivers::IBlockDevice* getBlockDevice() const noexcept override { return m_device; }
    Status                 statFs(FsStat* outStat) const noexcept override;
    Status                 sync() override;
    Status                 format(drivers::IBlockDevice* dev, const char* label) override;

    // Internal cluster and directory operations used by ExFatNode
    u32               getClusterSize() const noexcept { return m_clusterBytes; }
    Status            readCluster(u32 cluster, void* buffer) noexcept;
    Status            writeCluster(u32 cluster, const void* buffer) noexcept;
    u32               getNextCluster(u32 currentCluster, u8 streamFlags) noexcept;
    Result<u32>       allocateCluster(u32 prevCluster = 0) noexcept;
    Status            freeClusterChain(u32 startCluster, u64 byteSize, u8 streamFlags) noexcept;

    Status            updateDirectoryEntryOnDisk(ExFatNode* node) noexcept;
    Result<IVfsNode*> lookupInDir(ExFatNode* dirNode, const char* targetName) noexcept;
    Result<IVfsNode*> createEntryInDir(ExFatNode* dirNode, const char* name, bool isDirectory) noexcept;
    Status            removeEntryInDir(ExFatNode* dirNode, const char* name) noexcept;
    Status            readDirEntryAt(ExFatNode* dirNode, usize targetIndex, DirEntry* outEntry) noexcept;

private:
    u64        clusterToLba(u32 cluster) const noexcept;
    bool       isClusterAllocated(u32 cluster) const noexcept;
    void       setClusterAllocated(u32 cluster, bool allocated) noexcept;
    u32        readFatEntry(u32 cluster) noexcept;
    Status     writeFatEntry(u32 cluster, u32 value) noexcept;
    Status     flushBitmapToDisk() noexcept;
    ExFatNode* cacheOrCreateNode(const char* name,
                                 NodeType type,
                                 u32 firstCluster,
                                 u64 size,
                                 u8 flags,
                                 u32 parentCluster,
                                 u32 slotIndex) noexcept;

    mutable Spinlock       m_lock;
    drivers::IBlockDevice* m_device            = nullptr;
    u32                    m_bytesPerSector    = 512;
    u32                    m_sectorsPerCluster = 8;
    u32                    m_clusterBytes      = 4096;
    u32                    m_fatOffsetSectors  = 0;
    u32                    m_fatLengthSectors  = 0;
    u32                    m_heapOffsetSectors = 0;
    u32                    m_clusterCount      = 0;
    u32                    m_rootFirstCluster  = 0;
    u32                    m_bitmapCluster     = 2;
    u32                    m_bitmapByteLen     = 0;
    u8*                    m_bitmap            = nullptr;
    bool                   m_bitmapDirty       = false;
    char                   m_volumeLabel[32]   = "CORTEZ-OS";

    ExFatNode*             m_rootNode          = nullptr;
    ExFatNode*             m_nodeCache[MAX_OPEN_NODES]{};
    usize                  m_nodeCacheCount    = 0;
};

} // namespace cortez::fs::exfat
