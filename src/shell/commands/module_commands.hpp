/**
 * @file module_commands.hpp
 * @brief Shell commands for runtime C compilation and kernel module management (insmod, rmmod, lsmod, run, cc).
 */

#pragma once

#include "../i_shell_command.hpp"

namespace cortez::shell::commands {

class InsmodCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "insmod"; }
    const char* getUsage() const noexcept override { return "insmod <file.c>"; }
    const char* getDescription() const noexcept override { return "Compile and load a C kernel module or driver"; }
    Status      execute(const CommandArgs& args) override;
};

class RmmodCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "rmmod"; }
    const char* getUsage() const noexcept override { return "rmmod <module_name>"; }
    const char* getDescription() const noexcept override { return "Unload a runtime C kernel module and free resources"; }
    Status      execute(const CommandArgs& args) override;
};

class LsmodCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "lsmod"; }
    const char* getUsage() const noexcept override { return "lsmod"; }
    const char* getDescription() const noexcept override { return "List loaded runtime C kernel modules and resources"; }
    Status      execute(const CommandArgs& args) override;
};

class RunCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "run"; }
    const char* getUsage() const noexcept override { return "run <file.c> [args...]"; }
    const char* getDescription() const noexcept override { return "Compile and run a standalone C program in memory"; }
    Status      execute(const CommandArgs& args) override;
};

class CcCommand final : public IShellCommand {
public:
    const char* getName() const noexcept override { return "cc"; }
    const char* getUsage() const noexcept override { return "cc <file.c>"; }
    const char* getDescription() const noexcept override { return "Syntax-check and test-compile a C source file"; }
    Status      execute(const CommandArgs& args) override;
};

} // namespace cortez::shell::commands
