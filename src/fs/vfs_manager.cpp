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

    m_rootFs = exfatFs;
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

Status VfsManager::createFile(const char* cwd, const char* path) noexcept {
    if (!m_rootFs || !path || !path[0]) return Status::InvalidArgument;

    char canonical[PathResolver::MAX_PATH_LEN]{};
    PathResolver::normalize(cwd, path, canonical, sizeof(canonical));

    char parentPath[PathResolver::MAX_PATH_LEN]{};
    char leafName[64]{};
    PathResolver::splitParentAndLeaf(canonical, parentPath, sizeof(parentPath), leafName, sizeof(leafName));

    if (!leafName[0]) return Status::InvalidArgument;

    auto parentRes = PathResolver::resolveNode(m_rootFs->getRootNode(), parentPath);
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

    auto parentRes = PathResolver::resolveNode(m_rootFs->getRootNode(), parentPath);
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

    auto parentRes = PathResolver::resolveNode(m_rootFs->getRootNode(), parentPath);
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

Status VfsManager::sync() noexcept {
    return m_rootFs ? m_rootFs->sync() : Status::DeviceNotFound;
}

} // namespace cortez::fs
