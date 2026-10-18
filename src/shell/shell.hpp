/**
 * @file shell.hpp
 * @brief Interactive Cortez Kernel Shell with Command Pattern dispatch and I/O redirection.
 */

#pragma once

#include "i_shell_command.hpp"

namespace cortez::shell {

class Shell final {
public:
    static constexpr usize MAX_COMMANDS = 32;
    static constexpr usize MAX_LINE_LEN = 1024;

    static Shell& instance() noexcept;

    void init() noexcept;
    void registerCommand(IShellCommand* cmd) noexcept;
    void runInteractiveLoop() noexcept;
    void executeLine(char* line) noexcept;

    usize                getCommandCount() const noexcept { return m_cmdCount; }
    const IShellCommand* getCommandAt(usize idx) const noexcept {
        return (idx < m_cmdCount) ? m_commands[idx] : nullptr;
    }

private:
    Shell() noexcept = default;

    void printPrompt() const noexcept;
    void readLine(char* outBuf, usize maxLen) noexcept;

    IShellCommand* m_commands[MAX_COMMANDS]{};
    usize          m_cmdCount = 0;
};

} // namespace cortez::shell
