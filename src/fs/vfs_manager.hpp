/**
 * @file vfs_manager.hpp
 * @brief Virtual File System (VFS) Manager coordinating physical RootFS mount and File I/O.
 */

#pragma once

#include "i_filesystem.hpp"
#include "fd_table.hpp"
#include "path_resolver.hpp"

namespace cortez::fs {

class VfsManager final {
public:
    static VfsManager& instance() noexcept;

    Status mountBlockRoot(drivers::IBlockDevice* rootBdev) noexcept;
    bool   isRootMounted() const noexcept { return m_rootFs != nullptr; }

    IFileSystem* getRootFs() const noexcept { return m_rootFs; }
    FdTable&     getKernelFdTable() noexcept { return m_fdTable; }

    // Path-based VFS operations (resolved against task cwd)
    Result<IVfsNode*> resolvePath(const char* cwd, const char* path) noexcept;
    Result<i32>       open(const char* cwd, const char* path, u32 flags) noexcept;
    Status            close(i32 fd) noexcept;
    Result<usize>     read(i32 fd, void* buffer, usize count) noexcept;
    Result<usize>     write(i32 fd, const void* buffer, usize count) noexcept;

    Status            createFile(const char* cwd, const char* path) noexcept;
    Status            mkdir(const char* cwd, const char* path) noexcept;
    Status            unlink(const char* cwd, const char* path) noexcept;
    Status            readDir(const char* cwd, const char* path, usize index, DirEntry* outEntry) noexcept;
    Status            stat(const char* cwd, const char* path, FileStat* outStat) noexcept;
    Status            writeFileAll(const char* cwd, const char* path, const void* data, usize size) noexcept;
    Status            sync() noexcept;

private:
    VfsManager() noexcept = default;

    IFileSystem* m_rootFs  = nullptr;
    FdTable      m_fdTable{};
};

} // namespace cortez::fs
