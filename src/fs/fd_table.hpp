/**
 * @file fd_table.hpp
 * @brief File Descriptor Table (FdTable) mapping integer FDs to VFS nodes and Char Devices.
 */

#pragma once

#include "i_vfs_node.hpp"
#include "../drivers/i_device.hpp"

namespace cortez::fs {

enum OpenFlags : u32 {
    O_RDONLY = 0x0001U,
    O_WRONLY = 0x0002U,
    O_RDWR   = 0x0003U,
    O_CREAT  = 0x0040U,
    O_TRUNC  = 0x0200U,
    O_APPEND = 0x0400U
};

struct FileHandle {
    bool                  inUse      = false;
    u32                   flags      = 0;
    u64                   offset     = 0;
    IVfsNode*             vfsNode    = nullptr;
    drivers::ICharDevice* charDevice = nullptr;
};

class FdTable final {
public:
    static constexpr usize MAX_OPEN_FDS = 32;

    FdTable() noexcept = default;

    void bindStdStreams(drivers::ICharDevice* stdinDev,
                        drivers::ICharDevice* stdoutDev,
                        drivers::ICharDevice* stderrDev) noexcept;

    Result<i32> allocateVfsFd(IVfsNode* node, u32 flags) noexcept;
    FileHandle* getHandle(i32 fd) noexcept;
    Status      closeFd(i32 fd) noexcept;

private:
    FileHandle m_fds[MAX_OPEN_FDS]{};
};

} // namespace cortez::fs
