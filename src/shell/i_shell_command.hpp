/**
 * @file i_shell_command.hpp
 * @brief Command Pattern interface (IShellCommand) for Cortez Shell commands.
 */

#pragma once

#include "../lib/types.hpp"
#include "../lib/result.hpp"

namespace cortez::shell {

struct CommandArgs {
    static constexpr usize MAX_ARGS = 128;
    usize       argc = 0;
    const char* argv[MAX_ARGS]{};
};

/**
 * @brief SOLID Command Pattern interface for shell built-ins and utilities.
 */
class IShellCommand {
public:
    virtual ~IShellCommand() = default;

    virtual const char* getName() const noexcept = 0;
    virtual const char* getUsage() const noexcept = 0;
    virtual const char* getDescription() const noexcept = 0;
    virtual Status      execute(const CommandArgs& args) = 0;
};

} // namespace cortez::shell
