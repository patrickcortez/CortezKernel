/**
 * @file fs_commands.hpp
 * @brief Physical Filesystem & File I/O Shell Commands (ls, cd, pwd, cat, write, append, touch, mkdir, rm, stat, df, sync, mkfs).
 */

#pragma once

#include "../i_shell_command.hpp"

namespace cortez::shell::commands {

class LsCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "ls"; }
    const char* getUsage() const noexcept override { return "ls [path]"; }
    const char* getDescription() const noexcept override { return "List directory entries on NVMe disk"; }
    Status      execute(const CommandArgs& args) override;
};

class CdCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "cd"; }
    const char* getUsage() const noexcept override { return "cd <dir>"; }
    const char* getDescription() const noexcept override { return "Change current working directory"; }
    Status      execute(const CommandArgs& args) override;
};

class PwdCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "pwd"; }
    const char* getUsage() const noexcept override { return "pwd"; }
    const char* getDescription() const noexcept override { return "Print current working directory"; }
    Status      execute(const CommandArgs& args) override;
};

class CatCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "cat"; }
    const char* getUsage() const noexcept override { return "cat <file>"; }
    const char* getDescription() const noexcept override { return "Read and print file from NVMe disk"; }
    Status      execute(const CommandArgs& args) override;
};

class WriteCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "write"; }
    const char* getUsage() const noexcept override { return "write <file> <text...>"; }
    const char* getDescription() const noexcept override { return "Write text to a file on NVMe disk"; }
    Status      execute(const CommandArgs& args) override;
};

class AppendCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "append"; }
    const char* getUsage() const noexcept override { return "append <file> <text...>"; }
    const char* getDescription() const noexcept override { return "Append text to a file on NVMe disk"; }
    Status      execute(const CommandArgs& args) override;
};

class TouchCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "touch"; }
    const char* getUsage() const noexcept override { return "touch <file>"; }
    const char* getDescription() const noexcept override { return "Create an empty file on NVMe disk"; }
    Status      execute(const CommandArgs& args) override;
};

class MkdirCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "mkdir"; }
    const char* getUsage() const noexcept override { return "mkdir <dir>"; }
    const char* getDescription() const noexcept override { return "Create a directory on NVMe disk"; }
    Status      execute(const CommandArgs& args) override;
};

class RmCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "rm"; }
    const char* getUsage() const noexcept override { return "rm <path>"; }
    const char* getDescription() const noexcept override { return "Remove a file or empty directory from NVMe disk"; }
    Status      execute(const CommandArgs& args) override;
};

class StatCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "stat"; }
    const char* getUsage() const noexcept override { return "stat <path>"; }
    const char* getDescription() const noexcept override { return "Display physical on-disk file metadata"; }
    Status      execute(const CommandArgs& args) override;
};

class DfCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "df"; }
    const char* getUsage() const noexcept override { return "df"; }
    const char* getDescription() const noexcept override { return "Show physical NVMe filesystem space usage"; }
    Status      execute(const CommandArgs& args) override;
};

class SyncCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "sync"; }
    const char* getUsage() const noexcept override { return "sync"; }
    const char* getDescription() const noexcept override { return "Flush filesystem metadata and NVMe hardware cache"; }
    Status      execute(const CommandArgs& args) override;
};

class MkfsCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "mkfs.exfat"; }
    const char* getUsage() const noexcept override { return "mkfs.exfat <bdev> [label]"; }
    const char* getDescription() const noexcept override { return "Format a physical block device with exFAT"; }
    Status      execute(const CommandArgs& args) override;
};

class MountCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "mount"; }
    const char* getUsage() const noexcept override { return "mount [bdev] [mountpoint]"; }
    const char* getDescription() const noexcept override { return "Mount a block device or list active mounts"; }
    Status      execute(const CommandArgs& args) override;
};

class UmountCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "umount"; }
    const char* getUsage() const noexcept override { return "umount <mountpoint>"; }
    const char* getDescription() const noexcept override { return "Unmount a mounted filesystem"; }
    Status      execute(const CommandArgs& args) override;
};

} // namespace cortez::shell::commands
