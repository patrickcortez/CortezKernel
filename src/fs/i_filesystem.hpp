/**
 * @file i_filesystem.hpp
 * @brief SOLID interface for physical disk filesystems (Linux super_block equivalent).
 */

#pragma once

#include "i_vfs_node.hpp"
#include "../drivers/i_device.hpp"

namespace cortez::fs {

struct FsStat {
    char fsType[16];
    char volumeLabel[32];
    u64  totalBytes;
    u64  freeBytes;
    u64  usedBytes;
    u32  clusterSize;
    u32  totalClusters;
    u32  freeClusters;
};

/**
 * @brief Interface for physical block-backed filesystems (exFAT / ext4).
 */
class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    virtual const char*           getFsName() const noexcept = 0;
    virtual Status                mount(drivers::IBlockDevice* dev) = 0;
    virtual Status                unmount() = 0;
    virtual IVfsNode*             getRootNode() noexcept = 0;
    virtual drivers::IBlockDevice* getBlockDevice() const noexcept = 0;
    virtual Status                statFs(FsStat* outStat) const noexcept = 0;
    virtual Status                sync() = 0;
    virtual Status                format(drivers::IBlockDevice* dev, const char* label) = 0;
};

} // namespace cortez::fs
