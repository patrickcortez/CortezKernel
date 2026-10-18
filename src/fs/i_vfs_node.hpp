/**
 * @file i_vfs_node.hpp
 * @brief SOLID interface for VFS Nodes (inodes/vnodes) backed by physical disk storage.
 */

#pragma once

#include "../lib/types.hpp"
#include "../lib/result.hpp"

namespace cortez::fs {

enum class NodeType : u8 {
    File        = 1,
    Directory   = 2,
    CharDevice  = 3,
    BlockDevice = 4
};

struct FileStat {
    char     name[64];
    NodeType type;
    u64      size;
    u32      firstCluster;
    u32      blocks;
};

struct DirEntry {
    char     name[64];
    NodeType type;
    u64      size;
    u32      firstCluster;
};

/**
 * @brief Polymorphic VFS Node (Inode) representing an on-disk file or directory.
 */
class IVfsNode {
public:
    virtual ~IVfsNode() = default;

    virtual const char*   getName() const noexcept = 0;
    virtual NodeType      getType() const noexcept = 0;
    virtual u64           getSize() const noexcept = 0;

    virtual Result<usize> read(u64 offset, void* buffer, usize size) = 0;
    virtual Result<usize> write(u64 offset, const void* buffer, usize size) = 0;
    virtual Status        truncate(u64 newSize) = 0;

    virtual Result<IVfsNode*> lookup(const char* name) = 0;
    virtual Result<IVfsNode*> createFile(const char* name) = 0;
    virtual Result<IVfsNode*> createDir(const char* name) = 0;
    virtual Status            remove(const char* name) = 0;
    virtual Status            readDir(usize index, DirEntry* outEntry) = 0;

    virtual Status        stat(FileStat* outStat) const noexcept = 0;
    virtual Status        sync() = 0;
};

} // namespace cortez::fs
