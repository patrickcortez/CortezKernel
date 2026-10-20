/**
 * @file vfs_manager.cpp
 * @brief Implementation of VfsManager (mount_block_root and physical file operations).
 */

#include "vfs_manager.hpp"
#include "exfat/exfat_fs.hpp"
#include "../lib/string_utils.hpp"
#include "../lib/printk.hpp"

namespace cortez::fs {

VfsManager& VfsManager::instance() noexcept {
    static VfsManager s_vfs;
    return s_vfs;
}

Status VfsManager::mountBlockRoot(drivers::IBlockDevice* rootBdev) noexcept {
    if (!rootBdev) return Status::DeviceNotFound;

    auto* exfatFs = new exfat::ExFatFileSystem();
    if (!exfatFs) return Status::OutOfMemory;

    Status st = exfatFs->mount(rootBdev);
    if (st != Status::Ok) {
        delete exfatFs;
        return st;
    }

    m_rootFs   = exfatFs;
    m_rootBdev = rootBdev;
    pr_ok("vfs", "VFS: Mounted root (%s filesystem) on /dev/%s as '/'",
          m_rootFs->getFsName(), rootBdev->getName());
    return Status::Ok;
}

Result<IVfsNode*> VfsManager::resolvePath(const char* cwd, const char* path) noexcept {
    if (!m_rootFs || !m_rootFs->getRootNode()) {
        return Status::DeviceNotFound;
    }

    char canonical[PathResolver::MAX_PATH_LEN]{};
    PathResolver::normalize(cwd, path, canonical, sizeof(canonical));

    // Check mount points (longest prefix match)
    for (usize i = 0; i < m_mountCount; ++i) {
        if (!m_mounts[i].fs || !m_mounts[i].fs->getRootNode()) continue;
        usize mlen = str::length(m_mounts[i].path);
        if (str::compare(canonical, m_mounts[i].path) == 0) {
            return m_mounts[i].fs->getRootNode();
        }
        if (str::compareN(canonical, m_mounts[i].path, mlen) == 0 && canonical[mlen] == '/') {
            const char* subpath = canonical + mlen;
            return PathResolver::resolveNode(m_mounts[i].fs->getRootNode(), subpath);
        }
    }

    return PathResolver::resolveNode(m_rootFs->getRootNode(), canonical);
}

Result<i32> VfsManager::open(const char* cwd, const char* path, u32 flags) noexcept {
    if (!m_rootFs) return Status::DeviceNotFound;

    auto nodeRes = resolvePath(cwd, path);
    if (nodeRes.isError()) {
        if (nodeRes.status() == Status::NotFound && (flags & O_CREAT)) {
            Status cst = createFile(cwd, path);
            if (cst != Status::Ok) return cst;
            nodeRes = resolvePath(cwd, path);
        }
        if (nodeRes.isError()) return nodeRes.status();
    }

    IVfsNode* node = nodeRes.value();
    if (node->getType() == NodeType::Directory && (flags & (O_WRONLY | O_RDWR | O_TRUNC))) {
        return Status::IsDirectory;
    }

    if (flags & O_TRUNC) {
        node->truncate(0);
    }

    return m_fdTable.allocateVfsFd(node, flags);
}

Status VfsManager::close(i32 fd) noexcept {
    return m_fdTable.closeFd(fd);
}

Result<usize> VfsManager::read(i32 fd, void* buffer, usize count) noexcept {
    FileHandle* fh = m_fdTable.getHandle(fd);
    if (!fh) return Status::BadFileDescriptor;

    if (fh->charDevice) {
        return fh->charDevice->read(buffer, count);
    }
    if (!fh->vfsNode) return Status::BadFileDescriptor;

    auto res = fh->vfsNode->read(fh->offset, buffer, count);
    if (res.isOk()) {
        fh->offset += res.value();
    }
    return res;
}

Result<usize> VfsManager::write(i32 fd, const void* buffer, usize count) noexcept {
    FileHandle* fh = m_fdTable.getHandle(fd);
    if (!fh) return Status::BadFileDescriptor;

    if (fh->charDevice) {
        return fh->charDevice->write(buffer, count);
    }
    if (!fh->vfsNode) return Status::BadFileDescriptor;

    auto res = fh->vfsNode->write(fh->offset, buffer, count);
    if (res.isOk()) {
        fh->offset += res.value();
    }
    return res;
}

Result<u64> VfsManager::seek(i32 fd, i64 offset, i32 whence) noexcept {
    FileHandle* fh = m_fdTable.getHandle(fd);
    if (!fh || !fh->vfsNode) return Status::BadFileDescriptor;

    i64 base = 0;
    if (whence == 0) { // SEEK_SET
        base = 0;
    } else if (whence == 1) { // SEEK_CUR
        base = static_cast<i64>(fh->offset);
    } else if (whence == 2) { // SEEK_END
        base = static_cast<i64>(fh->vfsNode->getSize());
    } else {
        return Status::InvalidArgument;
    }

    i64 target = base + offset;
    if (target < 0) return Status::InvalidArgument;
    fh->offset = static_cast<u64>(target);
    return fh->offset;
}

Status VfsManager::createFile(const char* cwd, const char* path) noexcept {
    if (!m_rootFs || !path || !path[0]) return Status::InvalidArgument;

    char canonical[PathResolver::MAX_PATH_LEN]{};
    PathResolver::normalize(cwd, path, canonical, sizeof(canonical));

    char parentPath[PathResolver::MAX_PATH_LEN]{};
    char leafName[64]{};
    PathResolver::splitParentAndLeaf(canonical, parentPath, sizeof(parentPath), leafName, sizeof(leafName));

    if (!leafName[0]) return Status::InvalidArgument;

    auto parentRes = resolvePath(cwd, parentPath);
    if (parentRes.isError()) return parentRes.status();

    auto createRes = parentRes.value()->createFile(leafName);
    if (createRes.isError()) return createRes.status();
    return Status::Ok;
}

Status VfsManager::mkdir(const char* cwd, const char* path) noexcept {
    if (!m_rootFs || !path || !path[0]) return Status::InvalidArgument;

    char canonical[PathResolver::MAX_PATH_LEN]{};
    PathResolver::normalize(cwd, path, canonical, sizeof(canonical));

    char parentPath[PathResolver::MAX_PATH_LEN]{};
    char leafName[64]{};
    PathResolver::splitParentAndLeaf(canonical, parentPath, sizeof(parentPath), leafName, sizeof(leafName));

    if (!leafName[0]) return Status::InvalidArgument;

    auto parentRes = resolvePath(cwd, parentPath);
    if (parentRes.isError()) return parentRes.status();

    auto createRes = parentRes.value()->createDir(leafName);
    if (createRes.isError()) return createRes.status();
    return Status::Ok;
}

Status VfsManager::unlink(const char* cwd, const char* path) noexcept {
    if (!m_rootFs || !path || !path[0]) return Status::InvalidArgument;

    char canonical[PathResolver::MAX_PATH_LEN]{};
    PathResolver::normalize(cwd, path, canonical, sizeof(canonical));
    if (str::compare(canonical, "/") == 0) return Status::InvalidArgument;

    char parentPath[PathResolver::MAX_PATH_LEN]{};
    char leafName[64]{};
    PathResolver::splitParentAndLeaf(canonical, parentPath, sizeof(parentPath), leafName, sizeof(leafName));

    auto parentRes = resolvePath(cwd, parentPath);
    if (parentRes.isError()) return parentRes.status();

    return parentRes.value()->remove(leafName);
}

Status VfsManager::readDir(const char* cwd, const char* path, usize index, DirEntry* outEntry) noexcept {
    auto nodeRes = resolvePath(cwd, path);
    if (nodeRes.isError()) return nodeRes.status();
    return nodeRes.value()->readDir(index, outEntry);
}

Status VfsManager::stat(const char* cwd, const char* path, FileStat* outStat) noexcept {
    auto nodeRes = resolvePath(cwd, path);
    if (nodeRes.isError()) return nodeRes.status();
    return nodeRes.value()->stat(outStat);
}

Status VfsManager::writeFileAll(const char* cwd, const char* path, const void* data, usize size) noexcept {
    auto fdRes = open(cwd, path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fdRes.isError()) return fdRes.status();

    i32 fd = fdRes.value();
    auto wRes = write(fd, data, size);
    close(fd);
    return wRes.isOk() ? Status::Ok : wRes.status();
}

Status VfsManager::mount(const char* mountpoint, IFileSystem* fs, drivers::IBlockDevice* bdev) noexcept {
    if (!mountpoint || !fs) return Status::InvalidArgument;
    if (m_mountCount >= MAX_MOUNTS) return Status::NoSpace;

    char canonical[sizeof(MountPoint::path)]{};
    PathResolver::normalize("/", mountpoint, canonical, sizeof(canonical));

    for (usize i = 0; i < m_mountCount; ++i) {
        if (str::compare(m_mounts[i].path, canonical) == 0) {
            return Status::AlreadyExists;
        }
    }

    str::copy(m_mounts[m_mountCount].path, canonical, sizeof(m_mounts[m_mountCount].path));
    m_mounts[m_mountCount].fs   = fs;
    m_mounts[m_mountCount].bdev = bdev;
    ++m_mountCount;

    pr_dbg("vfs", "Mounted %s (%s) at '%s'",
          bdev ? bdev->getName() : "virtual", fs->getFsName(), canonical);
    return Status::Ok;
}

Status VfsManager::unmount(const char* mountpoint) noexcept {
    if (!mountpoint) return Status::InvalidArgument;

    char canonical[sizeof(MountPoint::path)]{};
    PathResolver::normalize("/", mountpoint, canonical, sizeof(canonical));

    for (usize i = 0; i < m_mountCount; ++i) {
        if (str::compare(m_mounts[i].path, canonical) == 0) {
            if (m_mounts[i].fs) {
                (void)m_mounts[i].fs->sync();
            }
            pr_dbg("vfs", "Unmounted '%s'", canonical);
            for (usize j = i; j + 1 < m_mountCount; ++j) {
                m_mounts[j] = m_mounts[j + 1];
            }
            m_mounts[--m_mountCount] = {};
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

Status VfsManager::sync() noexcept {
    Status st = Status::Ok;
    if (m_rootFs) {
        Status s = m_rootFs->sync();
        if (s != Status::Ok) st = s;
    }
    for (usize i = 0; i < m_mountCount; ++i) {
        if (m_mounts[i].fs) {
            Status s = m_mounts[i].fs->sync();
            if (s != Status::Ok) st = s;
        }
    }
    return st;
}

} // namespace cortez::fs
