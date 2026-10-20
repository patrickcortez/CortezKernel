/**
 * @file fat32_fs.hpp
 * @brief Definition of the lazy-loading, sliding-window FAT32 Filesystem driver.
 */

#pragma once

#include "fat_structs.hpp"
#include "../i_filesystem.hpp"
#include "../i_vfs_node.hpp"

namespace cortez::fs::fat {

class Fat32FileSystem;

/**
 * @brief Polymorphic VFS Node backed by on-disk FAT32 file or directory.
 */
class Fat32Node final : public IVfsNode {
public:
    Fat32Node(Fat32FileSystem* fs,
              const char* name,
              NodeType type,
              u32 firstCluster,
              u64 size,
              u32 parentDirCluster,
              u32 dirEntryIndex) noexcept;

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

    Status stat(FileStat* outStat) const noexcept override;
    Status sync() override;

    u32  getFirstCluster() const noexcept { return m_firstCluster; }
    void setFirstCluster(u32 c) noexcept  { m_firstCluster = c; }
    void setSize(u64 s) noexcept          { m_size = s; }
    u32  getParentCluster() const noexcept { return m_parentCluster; }
    u32  getDirSlotIndex() const noexcept  { return m_dirSlotIndex; }
    void updateMetadata(u32 firstCluster, u64 size) noexcept {
        m_firstCluster = firstCluster;
        m_size         = size;
    }

private:
    Fat32FileSystem* m_fs            = nullptr;
    NodeType         m_type          = NodeType::File;
    u32              m_firstCluster  = 0;
    u64              m_size          = 0;
    u32              m_parentCluster = 0;
    u32              m_dirSlotIndex  = 0;
    char             m_name[64]{};
};

/**
 * @brief Memory-efficient FAT32 Filesystem implementation with sliding-window FAT caching.
 */
class Fat32FileSystem final : public IFileSystem {
public:
    static constexpr usize MAX_OPEN_NODES  = 64;
    static constexpr usize FAT_CACHE_BYTES = 4096;

    Fat32FileSystem() noexcept = default;
    ~Fat32FileSystem() override;

    const char*            getFsName() const noexcept override { return "fat32"; }
    Status                 mount(drivers::IBlockDevice* dev) override;
    Status                 unmount() override;
    IVfsNode*              getRootNode() noexcept override { return m_rootNode; }
    drivers::IBlockDevice* getBlockDevice() const noexcept override { return m_device; }
    Status                 statFs(FsStat* outStat) const noexcept override;
    Status                 sync() override;
    Status                 format(drivers::IBlockDevice* dev, const char* label) override;

    // Geometry Helpers
    u64    clusterToLba(u32 cluster) const noexcept;
    u32    getClusterSize() const noexcept { return m_clusterBytes; }
    Status readCluster(u32 cluster, void* buffer) noexcept;
    Status writeCluster(u32 cluster, const void* buffer) noexcept;

    // Sliding-Window FAT cache methods
    u32         readFatEntry(u32 cluster) noexcept;
    Status      writeFatEntry(u32 cluster, u32 nextVal) noexcept;
    Status      flushFatCache() noexcept;
    u32         getNextCluster(u32 cluster) noexcept { return readFatEntry(cluster); }
    Result<u32> allocateCluster(u32 prevCluster) noexcept;
    Status      freeClusterChain(u32 startCluster) noexcept;

    // Directory operations
    Result<IVfsNode*> lookupInDir(Fat32Node* dirNode, const char* name) noexcept;
    Result<IVfsNode*> createEntryInDir(Fat32Node* dirNode, const char* name, bool isDirectory) noexcept;
    Status            removeEntryInDir(Fat32Node* dirNode, const char* name) noexcept;
    Status            readDirEntryAt(Fat32Node* dirNode, usize targetIndex, DirEntry* outEntry) noexcept;
    Status            updateDirectoryEntryOnDisk(Fat32Node* node) noexcept;

    Fat32Node* cacheOrCreateNode(const char* name,
                                 NodeType type,
                                 u32 firstCluster,
                                 u64 size,
                                 u32 parentCluster,
                                 u32 slotIndex) noexcept;

private:
    drivers::IBlockDevice* m_device            = nullptr;
    u32                    m_bytesPerSector    = 512;
    u32                    m_sectorsPerCluster = 8;
    u32                    m_clusterBytes      = 4096;
    u32                    m_reservedSectors   = 32;
    u32                    m_fatCount          = 2;
    u32                    m_fatStartLba       = 32;
    u32                    m_fatSizeSectors    = 0;
    u32                    m_dataStartLba      = 0;
    u32                    m_clusterCount      = 0;
    u32                    m_rootCluster       = 2;
    char                   m_volumeLabel[32]{};

    // Sliding window FAT sector cache
    u8   m_fatCache[FAT_CACHE_BYTES]{};
    u32  m_cachedFatSector = 0xFFFFFFFFU;
    bool m_fatCacheDirty   = false;

    Fat32Node* m_rootNode                          = nullptr;
    Fat32Node* m_nodeCache[MAX_OPEN_NODES]{};
    usize      m_nodeCacheCount                    = 0;
};

} // namespace cortez::fs::fat
