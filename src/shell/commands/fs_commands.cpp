/**
 * @file fs_commands.cpp
 * @brief Implementation of physical NVMe Filesystem & File I/O Shell Commands.
 */

#include "fs_commands.hpp"
#include "../../fs/vfs_manager.hpp"
#include "../../fs/exfat/exfat_fs.hpp"
#include "../../fs/fat/fat32_fs.hpp"
#include "../../fs/fs_detector.hpp"
#include "../../drivers/i_device.hpp"
#include "../../proc/scheduler.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::shell::commands {

namespace {
const char* getActiveCwd() noexcept {
    proc::Task* t = proc::Scheduler::instance().getCurrentTask();
    return t ? t->getCwd() : "/";
}
} // namespace

Status LsCommand::execute(const CommandArgs& args) {
    const char* target = (args.argc >= 2) ? args.argv[1] : getActiveCwd();

    fs::FileStat st{};
    Status status = fs::VfsManager::instance().stat(getActiveCwd(), target, &st);
    if (status != Status::Ok) return status;

    if (st.type == fs::NodeType::File) {
        kprintf("  \033[90m%-6s %-28s %12s  %s\033[0m\n", "TYPE", "NAME", "SIZE", "CLUSTER");
        kprintf("  \033[32m<FILE> \033[97m%-28s\033[0m %10llu B  #%u\n",
                st.name, st.size, st.firstCluster);
        return Status::Ok;
    }

    usize idx       = 0;
    usize dirCount  = 0;
    usize fileCount = 0;
    fs::DirEntry entry{};
    kprintf("  \033[90m%-6s %-28s %12s  %s\033[0m\n", "TYPE", "NAME", "SIZE", "CLUSTER");
    while (fs::VfsManager::instance().readDir(getActiveCwd(), target, idx, &entry) == Status::Ok) {
        if (entry.type == fs::NodeType::Directory) {
            char dirName[72]{};
            str::copy(dirName, entry.name, sizeof(dirName) - 2);
            str::concat(dirName, "/", sizeof(dirName));
            kprintf("  \033[34m<DIR>  \033[94m%-28s\033[0m %10llu B  #%u\n",
                    dirName, entry.size, entry.firstCluster);
            ++dirCount;
        } else {
            kprintf("  \033[32m<FILE> \033[97m%-28s\033[0m %10llu B  #%u\n",
                    entry.name, entry.size, entry.firstCluster);
            ++fileCount;
        }
        ++idx;
    }

    if (idx == 0) {
        kprintf("  \033[90m(empty directory)\033[0m\n");
    } else {
        kprintf("  \033[90mTotal: %zu file(s), %zu dir(s)\033[0m\n", fileCount, dirCount);
    }
    return Status::Ok;
}

Status CdCommand::execute(const CommandArgs& args) {
    const char* target = (args.argc >= 2) ? args.argv[1] : "/";

    char canonical[fs::PathResolver::MAX_PATH_LEN]{};
    fs::PathResolver::normalize(getActiveCwd(), target, canonical, sizeof(canonical));

    fs::FileStat st{};
    Status status = fs::VfsManager::instance().stat("/", canonical, &st);
    if (status != Status::Ok) return status;
    if (st.type != fs::NodeType::Directory) return Status::NotDirectory;

    if (proc::Task* t = proc::Scheduler::instance().getCurrentTask()) {
        t->setCwd(canonical);
    }
    return Status::Ok;
}

Status PwdCommand::execute(const CommandArgs&) {
    kprintf("%s\n", getActiveCwd());
    return Status::Ok;
}

Status CatCommand::execute(const CommandArgs& args) {
    if (args.argc < 2) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }

    auto fdRes = fs::VfsManager::instance().open(getActiveCwd(), args.argv[1], fs::O_RDONLY);
    if (fdRes.isError()) return fdRes.status();

    i32 fd = fdRes.value();
    char buf[512];
    bool endedWithNewline = true;

    for (;;) {
        auto rRes = fs::VfsManager::instance().read(fd, buf, sizeof(buf));
        if (rRes.isError()) {
            fs::VfsManager::instance().close(fd);
            return rRes.status();
        }
        usize bytes = rRes.value();
        if (bytes == 0) break;

        klog::Logger::instance().writeRaw(buf, bytes);
        endedWithNewline = (buf[bytes - 1] == '\n');
    }

    if (!endedWithNewline) {
        kprintf("\n");
    }

    fs::VfsManager::instance().close(fd);
    return Status::Ok;
}

Status WriteCommand::execute(const CommandArgs& args) {
    if (args.argc < 3) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }

    char payload[2048]{};
    for (usize i = 2; i < args.argc; ++i) {
        if (i > 2) str::concat(payload, " ", sizeof(payload));
        str::concat(payload, args.argv[i], sizeof(payload));
    }
    str::concat(payload, "\n", sizeof(payload));

    usize len = str::length(payload);
    Status st = fs::VfsManager::instance().writeFileAll(getActiveCwd(), args.argv[1], payload, len);
    if (st == Status::Ok) {
        kprintf("\033[32mWrote %zu bytes to '%s'.\033[0m\n", len, args.argv[1]);
    }
    return st;
}

Status AppendCommand::execute(const CommandArgs& args) {
    if (args.argc < 3) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }

    auto fdRes = fs::VfsManager::instance().open(getActiveCwd(), args.argv[1],
                                                 fs::O_WRONLY | fs::O_CREAT | fs::O_APPEND);
    if (fdRes.isError()) return fdRes.status();

    char payload[2048]{};
    for (usize i = 2; i < args.argc; ++i) {
        if (i > 2) str::concat(payload, " ", sizeof(payload));
        str::concat(payload, args.argv[i], sizeof(payload));
    }
    str::concat(payload, "\n", sizeof(payload));

    i32 fd    = fdRes.value();
    usize len = str::length(payload);
    auto wRes = fs::VfsManager::instance().write(fd, payload, len);
    fs::VfsManager::instance().close(fd);

    if (wRes.isError()) return wRes.status();
    kprintf("\033[32mAppended %zu bytes to '%s'.\033[0m\n", len, args.argv[1]);
    return Status::Ok;
}

Status TouchCommand::execute(const CommandArgs& args) {
    if (args.argc < 2) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }
    return fs::VfsManager::instance().createFile(getActiveCwd(), args.argv[1]);
}

Status MkdirCommand::execute(const CommandArgs& args) {
    if (args.argc < 2) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }
    return fs::VfsManager::instance().mkdir(getActiveCwd(), args.argv[1]);
}

Status RmCommand::execute(const CommandArgs& args) {
    if (args.argc < 2) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }
    return fs::VfsManager::instance().unlink(getActiveCwd(), args.argv[1]);
}

Status StatCommand::execute(const CommandArgs& args) {
    if (args.argc < 2) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }

    fs::FileStat st{};
    Status status = fs::VfsManager::instance().stat(getActiveCwd(), args.argv[1], &st);
    if (status != Status::Ok) return status;

    kprintf("  File: %s\n", st.name);
    kprintf("  Type: %s\n", (st.type == fs::NodeType::Directory) ? "Directory" : "Regular File");
    kprintf("  Size: %llu bytes (%u clusters)\n", st.size, st.blocks);
    kprintf("  First Cluster: #%u\n", st.firstCluster);
    return Status::Ok;
}

Status DfCommand::execute(const CommandArgs&) {
    fs::IFileSystem* rootFs = fs::VfsManager::instance().getRootFs();
    if (!rootFs) return Status::DeviceNotFound;

    fs::FsStat st{};
    Status status = rootFs->statFs(&st);
    if (status != Status::Ok) return status;

    const char* devName = rootFs->getBlockDevice() ? rootFs->getBlockDevice()->getName() : "nvme0n1p2";
    kprintf("  \033[90m%-14s %-8s %-12s %10s %10s %10s %10s\033[0m\n",
            "FILESYSTEM", "TYPE", "LABEL", "TOTAL(KiB)", "USED(KiB)", "FREE(KiB)", "CLUSTERS");
    kprintf("  /dev/%-9s %-8s %-12s %10llu %10llu %10llu %u/%u\n",
            devName,
            st.fsType,
            st.volumeLabel,
            st.totalBytes / 1024ULL,
            st.usedBytes / 1024ULL,
            st.freeBytes / 1024ULL,
            st.totalClusters - st.freeClusters,
            st.totalClusters);
    return Status::Ok;
}

Status SyncCommand::execute(const CommandArgs&) {
    Status st = fs::VfsManager::instance().sync();
    if (st == Status::Ok) {
        kprintf("\033[32mFlushed VFS metadata and NVMe volatile cache (NVME_OPC_FLUSH).\033[0m\n");
    }
    return st;
}

Status MkfsCommand::execute(const CommandArgs& args) {
    if (args.argc < 2) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }

    drivers::IBlockDevice* bdev = drivers::DeviceRegistry::instance().findBlockDevice(args.argv[1]);
    if (!bdev) return Status::DeviceNotFound;

    // Safety: refuse to wipe partitions not created for Cortez (e.g. the host's Ubuntu/Windows disk).
    bool force = false;
    const char* label = "CORTEZ-OS";
    for (usize i = 2; i < args.argc; ++i) {
        if (str::compare(args.argv[i], "--force") == 0) force = true;
        else label = args.argv[i];
    }
    if (!force && str::compare(bdev->getLabel(), "Cortez-RootFS") != 0) {
        kprintf("\033[31mRefusing to format /dev/%s (GPT label '%s' is not 'Cortez-RootFS').\033[0m\n"
                "This may contain another OS's data. Re-run with --force to override.\n",
                bdev->getName(), bdev->getLabel());
        return Status::NotSupported;
    }

    fs::exfat::ExFatFileSystem formatter;
    Status st = formatter.format(bdev, label);
    if (st == Status::Ok) {
        kprintf("\033[32mFormatted /dev/%s with physical exFAT volume '%s'.\033[0m\n",
                bdev->getName(), label);
    }
    return st;
}

Status MountCommand::execute(const CommandArgs& args) {
    auto& vfs = fs::VfsManager::instance();
    if (args.argc < 2) {
        kprintf("  \033[90m%-16s %-12s %s\033[0m\n", "TARGET", "FILESYSTEM", "SOURCE");
        for (usize i = 0; i < vfs.getMountCount(); ++i) {
            const auto* m = vfs.getMountAt(i);
            if (!m) continue;
            kprintf("  \033[36m%-16s\033[0m %-12s %s\n",
                    m->path,
                    m->fs ? m->fs->getFsName() : "unknown",
                    m->bdev ? m->bdev->getName() : "virtual");
        }
        return Status::Ok;
    }

    if (args.argc < 3) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }

    const char* devName = args.argv[1];
    if (str::compareN(devName, "/dev/", 5) == 0) devName += 5;
    const char* mountPoint = args.argv[2];

    auto* bdev = drivers::DeviceRegistry::instance().findBlockDevice(devName);
    if (!bdev) {
        kprintf("\033[31mBlock device '/dev/%s' not found.\033[0m\n", devName);
        return Status::DeviceNotFound;
    }

    auto probe = fs::FsDetector::probe(bdev);
    fs::IFileSystem* targetFs = nullptr;
    if (probe.type == fs::DetectedFsType::Fat32 ||
        probe.type == fs::DetectedFsType::Fat16 ||
        probe.type == fs::DetectedFsType::Fat12) {
        targetFs = new fs::fat::Fat32FileSystem();
    } else {
        targetFs = new fs::exfat::ExFatFileSystem();
    }

    if (!targetFs || targetFs->mount(bdev) != Status::Ok) {
        delete targetFs;
        targetFs = new fs::fat::Fat32FileSystem();
        if (targetFs->mount(bdev) != Status::Ok) {
            delete targetFs;
            kprintf("\033[31mFailed to mount /dev/%s: unsupported or corrupted filesystem.\033[0m\n", devName);
            return Status::CorruptedFs;
        }
    }

    (void)vfs.mkdir("/", mountPoint);
    Status st = vfs.mount(mountPoint, targetFs, bdev);
    if (st == Status::Ok) {
        kprintf("\033[32mMounted /dev/%s (%s) on '%s'.\033[0m\n",
                bdev->getName(), targetFs->getFsName(), mountPoint);
    } else {
        delete targetFs;
        kprintf("\033[31mMount failed: %s\033[0m\n", statusToString(st));
    }
    return st;
}

Status UmountCommand::execute(const CommandArgs& args) {
    if (args.argc < 2) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }

    const char* target = args.argv[1];
    auto& vfs = fs::VfsManager::instance();
    for (usize i = 0; i < vfs.getMountCount(); ++i) {
        const auto* m = vfs.getMountAt(i);
        if (m && (str::compare(m->path, target) == 0 ||
                  (m->bdev && str::compare(m->bdev->getName(), target) == 0))) {
            char mpath[64]{};
            str::copy(mpath, m->path, sizeof(mpath));
            auto* targetFs = m->fs;
            Status st = vfs.unmount(mpath);
            delete targetFs;
            if (st == Status::Ok) {
                kprintf("\033[32mUnmounted '%s'.\033[0m\n", mpath);
            }
            return st;
        }
    }

    kprintf("\033[31mNo mount point found matching '%s'.\033[0m\n", target);
    return Status::NotFound;
}

} // namespace cortez::shell::commands
