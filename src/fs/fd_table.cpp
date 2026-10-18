/**
 * @file fd_table.cpp
 * @brief Implementation of per-task / kernel File Descriptor Table.
 */

#include "fd_table.hpp"

namespace cortez::fs {

void FdTable::bindStdStreams(drivers::ICharDevice* stdinDev,
                             drivers::ICharDevice* stdoutDev,
                             drivers::ICharDevice* stderrDev) noexcept {
    m_fds[0] = { true, O_RDONLY, 0, nullptr, stdinDev  };
    m_fds[1] = { true, O_WRONLY, 0, nullptr, stdoutDev };
    m_fds[2] = { true, O_WRONLY, 0, nullptr, stderrDev };
}

Result<i32> FdTable::allocateVfsFd(IVfsNode* node, u32 flags) noexcept {
    if (!node) return Status::InvalidArgument;

    for (usize i = 3; i < MAX_OPEN_FDS; ++i) {
        if (!m_fds[i].inUse) {
            m_fds[i].inUse      = true;
            m_fds[i].flags      = flags;
            m_fds[i].offset     = (flags & O_APPEND) ? node->getSize() : 0ULL;
            m_fds[i].vfsNode    = node;
            m_fds[i].charDevice = nullptr;
            return static_cast<i32>(i);
        }
    }
    return Status::OutOfMemory;
}

FileHandle* FdTable::getHandle(i32 fd) noexcept {
    if (fd < 0 || static_cast<usize>(fd) >= MAX_OPEN_FDS) return nullptr;
    if (!m_fds[fd].inUse) return nullptr;
    return &m_fds[fd];
}

Status FdTable::closeFd(i32 fd) noexcept {
    if (fd < 3 || static_cast<usize>(fd) >= MAX_OPEN_FDS) return Status::BadFileDescriptor;
    if (!m_fds[fd].inUse) return Status::BadFileDescriptor;

    if (m_fds[fd].vfsNode) {
        m_fds[fd].vfsNode->sync();
    }
    m_fds[fd] = FileHandle{};
    return Status::Ok;
}

} // namespace cortez::fs
