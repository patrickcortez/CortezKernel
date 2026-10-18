/**
 * @file sys_commands.hpp
 * @brief System, Hardware, Memory, and Process Shell Commands (help, clear, dmesg, lsblk, lspci, nvme, ps, free, uname, reboot).
 */

#pragma once

#include "../i_shell_command.hpp"

namespace cortez::shell::commands {

class HelpCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "help"; }
    const char* getUsage() const noexcept override { return "help"; }
    const char* getDescription() const noexcept override { return "List all Cortez Shell commands"; }
    Status      execute(const CommandArgs& args) override;
};

class ClearCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "clear"; }
    const char* getUsage() const noexcept override { return "clear"; }
    const char* getDescription() const noexcept override { return "Clear the GOP terminal screen"; }
    Status      execute(const CommandArgs& args) override;
};

class DmesgCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "dmesg"; }
    const char* getUsage() const noexcept override { return "dmesg"; }
    const char* getDescription() const noexcept override { return "Print kernel boot and driver ring buffer"; }
    Status      execute(const CommandArgs& args) override;
};

class LsblkCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "lsblk"; }
    const char* getUsage() const noexcept override { return "lsblk"; }
    const char* getDescription() const noexcept override { return "List physical block devices and GPT partitions"; }
    Status      execute(const CommandArgs& args) override;
};

class LspciCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "lspci"; }
    const char* getUsage() const noexcept override { return "lspci"; }
    const char* getDescription() const noexcept override { return "List enumerated PCIe hardware functions"; }
    Status      execute(const CommandArgs& args) override;
};

class NvmeInfoCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "nvme"; }
    const char* getUsage() const noexcept override { return "nvme"; }
    const char* getDescription() const noexcept override { return "Display physical NVMe controller and namespace status"; }
    Status      execute(const CommandArgs& args) override;
};

class LsusbCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "lsusb"; }
    const char* getUsage() const noexcept override { return "lsusb"; }
    const char* getDescription() const noexcept override { return "List xHCI USB host controllers and attached USB storage drives"; }
    Status      execute(const CommandArgs& args) override;
};

class PsCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "ps"; }
    const char* getUsage() const noexcept override { return "ps"; }
    const char* getDescription() const noexcept override { return "List active kernel processes/tasks"; }
    Status      execute(const CommandArgs& args) override;
};

class FreeCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "free"; }
    const char* getUsage() const noexcept override { return "free"; }
    const char* getDescription() const noexcept override { return "Display physical RAM (PMM) and Kernel Heap usage"; }
    Status      execute(const CommandArgs& args) override;
};

class UnameCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "uname"; }
    const char* getUsage() const noexcept override { return "uname [-a]"; }
    const char* getDescription() const noexcept override { return "Print Cortez Kernel version and architecture"; }
    Status      execute(const CommandArgs& args) override;
};

class RebootCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "reboot"; }
    const char* getUsage() const noexcept override { return "reboot"; }
    const char* getDescription() const noexcept override { return "Sync physical NVMe disk and reboot machine"; }
    Status      execute(const CommandArgs& args) override;
};

} // namespace cortez::shell::commands
