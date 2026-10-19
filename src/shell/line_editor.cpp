/**
 * @file line_editor.cpp
 * @brief Implementation of the interactive VT100/ANSI Shell Line Editor and History Ring.
 */

#include "line_editor.hpp"
#include "shell.hpp"
#include "../drivers/input/ps2_keyboard.hpp"
#include "../drivers/video/terminal_driver.hpp"
#include "../fs/vfs_manager.hpp"
#include "../proc/scheduler.hpp"
#include "../lib/string_utils.hpp"
#include "../lib/printk.hpp"

namespace cortez::shell {

LineEditor& LineEditor::instance() noexcept {
    static LineEditor s_editor;
    return s_editor;
}

void LineEditor::addHistory(const char* line) noexcept {
    if (!line) return;
    while (*line == ' ' || *line == '\t') ++line;
    if (*line == '\0') return;

    // Avoid consecutive duplicate history entries
    if (m_historyCount > 0) {
        const char* last = getHistoryAt(m_historyCount - 1);
        if (last && str::compare(last, line) == 0) {
            return;
        }
    }

    usize writeSlot = 0;
    if (m_historyCount < HISTORY_CAPACITY) {
        writeSlot = (m_historyHead + m_historyCount) % HISTORY_CAPACITY;
        ++m_historyCount;
    } else {
        writeSlot     = m_historyHead;
        m_historyHead = (m_historyHead + 1) % HISTORY_CAPACITY;
    }

    str::copy(m_history[writeSlot], line, MAX_LINE_LEN);
}

const char* LineEditor::getHistoryAt(usize index) const noexcept {
    if (index >= m_historyCount) return nullptr;
    usize slot = (m_historyHead + index) % HISTORY_CAPACITY;
    return m_history[slot];
}

void LineEditor::replaceLine(char* buf, usize maxLen, usize& len, usize& cursor, const char* newText) noexcept {
    if (cursor > 0) {
        kprintf("\033[%zuD", cursor);
    }

    usize oldLen = len;
    str::copy(buf, newText ? newText : "", maxLen);
    len    = str::length(buf);
    cursor = len;

    if (len > 0) {
        klog::Logger::instance().writeRaw(buf, len);
    }

    if (oldLen > len) {
        usize diff = oldLen - len;
        for (usize i = 0; i < diff; ++i) {
            klog::Logger::instance().putChar(' ');
        }
        kprintf("\033[%zuD", diff);
    }
}

void LineEditor::readLine(char* outBuf, usize maxLen) noexcept {
    if (!outBuf || maxLen == 0) return;

    usize len        = 0;
    usize cursor     = 0;
    usize historyIdx = m_historyCount;
    outBuf[0]        = '\0';
    m_draft[0]       = '\0';

    enum class EscState : u8 { None, Esc, Bracket, TildeParam };
    EscState escState = EscState::None;
    u32      escParam = 0;

    auto& kbd = drivers::input::Ps2KeyboardDriver::instance();

    auto readNextKey = [&]() -> char {
        if (!kbd.hasData()) {
            kbd.pollHardware();
            if (!kbd.hasData()) {
                proc::Scheduler::instance().yield();
            }
        }
        return kbd.readCharBlocking();
    };

    for (;;) {
        char ch = readNextKey();

        if (escState == EscState::Esc) {
            if (ch == '[' || ch == 'O') {
                escState = EscState::Bracket;
                escParam = 0;
            } else {
                escState = EscState::None;
            }
            continue;
        }

        if (escState == EscState::Bracket || escState == EscState::TildeParam) {
            if (ch >= '0' && ch <= '9') {
                escParam = escParam * 10 + static_cast<u32>(ch - '0');
                escState = EscState::TildeParam;
                continue;
            }

            escState = EscState::None;

            if (ch == 'A') { // Up Arrow (History previous)
                if (m_historyCount > 0 && historyIdx > 0) {
                    if (historyIdx == m_historyCount) {
                        str::copy(m_draft, outBuf, sizeof(m_draft));
                    }
                    --historyIdx;
                    replaceLine(outBuf, maxLen, len, cursor, getHistoryAt(historyIdx));
                }
            } else if (ch == 'B') { // Down Arrow (History next)
                if (historyIdx < m_historyCount) {
                    ++historyIdx;
                    const char* replacement = (historyIdx == m_historyCount)
                                                  ? m_draft
                                                  : getHistoryAt(historyIdx);
                    replaceLine(outBuf, maxLen, len, cursor, replacement);
                }
            } else if (ch == 'C') { // Right Arrow
                if (cursor < len) {
                    ++cursor;
                    kprintf("\033[1C");
                }
            } else if (ch == 'D') { // Left Arrow
                if (cursor > 0) {
                    --cursor;
                    kprintf("\033[1D");
                }
            } else if (ch == 'H') { // Home
                if (cursor > 0) {
                    kprintf("\033[%zuD", cursor);
                    cursor = 0;
                }
            } else if (ch == 'F') { // End
                if (cursor < len) {
                    kprintf("\033[%zuC", len - cursor);
                    cursor = len;
                }
            } else if (ch == '~') {
                if (escParam == 1 || escParam == 7) { // Home
                    if (cursor > 0) {
                        kprintf("\033[%zuD", cursor);
                        cursor = 0;
                    }
                } else if (escParam == 4 || escParam == 8) { // End
                    if (cursor < len) {
                        kprintf("\033[%zuC", len - cursor);
                        cursor = len;
                    }
                } else if (escParam == 3) { // Delete under cursor
                    if (cursor < len) {
                        memmove(outBuf + cursor, outBuf + cursor + 1, len - cursor);
                        --len;
                        usize tail = len - cursor;
                        if (tail > 0) {
                            klog::Logger::instance().writeRaw(outBuf + cursor, tail);
                        }
                        klog::Logger::instance().putChar(' ');
                        kprintf("\033[%zuD", tail + 1);
                    }
                }
            }
            continue;
        }

        if (ch == '\033') {
            escState = EscState::Esc;
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (cursor < len) {
                kprintf("\033[%zuC", len - cursor);
            }
            kprintf("\n");
            outBuf[len] = '\0';
            addHistory(outBuf);
            return;
        }

        if (ch == '\x03') { // Ctrl+C: cancel current line
            if (cursor < len) {
                kprintf("\033[%zuC", len - cursor);
            }
            kprintf("^C\n");
            outBuf[0] = '\0';
            return;
        }

        if (ch == '\x01') { // Ctrl+A: Home
            if (cursor > 0) {
                kprintf("\033[%zuD", cursor);
                cursor = 0;
            }
            continue;
        }

        if (ch == '\x05') { // Ctrl+E: End
            if (cursor < len) {
                kprintf("\033[%zuC", len - cursor);
                cursor = len;
            }
            continue;
        }

        if (ch == '\x15') { // Ctrl+U: clear entire line
            replaceLine(outBuf, maxLen, len, cursor, "");
            continue;
        }

        if (ch == '\x0B') { // Ctrl+K: kill from cursor to end of line
            if (cursor < len) {
                usize tail = len - cursor;
                for (usize i = 0; i < tail; ++i) {
                    klog::Logger::instance().putChar(' ');
                }
                kprintf("\033[%zuD", tail);
                outBuf[cursor] = '\0';
                len            = cursor;
            }
            continue;
        }

        if (ch == '\x0C') { // Ctrl+L: clear screen and redraw prompt + line
            drivers::video::TerminalDriver::instance().clear();
            Shell::instance().printPrompt();
            klog::Logger::instance().writeRaw(outBuf, len);
            if (cursor < len) {
                kprintf("\033[%zuD", len - cursor);
            }
            continue;
        }

        if (ch == '\b' || ch == 127) { // Backspace
            if (cursor > 0) {
                memmove(outBuf + cursor - 1, outBuf + cursor, len - cursor + 1);
                --cursor;
                --len;
                usize tail = len - cursor;
                kprintf("\033[1D");
                if (tail > 0) {
                    klog::Logger::instance().writeRaw(outBuf + cursor, tail);
                }
                klog::Logger::instance().putChar(' ');
                kprintf("\033[%zuD", tail + 1);
            }
            continue;
        }

        if (ch == '\t') { // Tab completion for commands and VFS file/directory paths
            if (cursor == len && len > 0) {
                const char* lastSpace = str::findLastChar(outBuf, ' ');
                if (!lastSpace) {
                    const char* match = nullptr;
                    usize matchCount  = 0;
                    usize cmdCount    = Shell::instance().getCommandCount();
                    for (usize i = 0; i < cmdCount; ++i) {
                        const IShellCommand* cmd = Shell::instance().getCommandAt(i);
                        if (cmd && str::compareN(cmd->getName(), outBuf, len) == 0) {
                            match = cmd->getName();
                            ++matchCount;
                        }
                    }
                    if (matchCount == 1 && match) {
                        char completed[128]{};
                        str::copy(completed, match, sizeof(completed));
                        str::concat(completed, " ", sizeof(completed));
                        replaceLine(outBuf, maxLen, len, cursor, completed);
                    }
                } else {
                    const char* token     = lastSpace + 1;
                    const char* lastSlash = str::findLastChar(token, '/');
                    const proc::Task* t   = proc::Scheduler::instance().getCurrentTask();
                    const char* cwd       = t ? t->getCwd() : "/";

                    char dirPart[128]{};
                    const char* namePrefix = token;
                    if (lastSlash) {
                        usize dirLen = static_cast<usize>(lastSlash - token);
                        if (dirLen == 0) {
                            str::copy(dirPart, "/", sizeof(dirPart));
                        } else {
                            if (dirLen >= sizeof(dirPart)) dirLen = sizeof(dirPart) - 1;
                            memcpy(dirPart, token, dirLen);
                            dirPart[dirLen] = '\0';
                        }
                        namePrefix = lastSlash + 1;
                    } else {
                        str::copy(dirPart, cwd, sizeof(dirPart));
                    }

                    usize prefixLen  = str::length(namePrefix);
                    usize matchCount = 0;
                    fs::DirEntry matchedEntry{};
                    fs::DirEntry entry{};
                    for (usize idx = 0;
                         fs::VfsManager::instance().readDir(cwd, dirPart, idx, &entry) == Status::Ok;
                         ++idx) {
                        if (str::compareN(entry.name, namePrefix, prefixLen) == 0) {
                            matchedEntry = entry;
                            ++matchCount;
                        }
                    }

                    if (matchCount == 1) {
                        char completed[MAX_LINE_LEN]{};
                        usize headLen = static_cast<usize>((lastSlash ? (lastSlash + 1) : token) - outBuf);
                        if (headLen < sizeof(completed)) {
                            memcpy(completed, outBuf, headLen);
                            completed[headLen] = '\0';
                            str::concat(completed, matchedEntry.name, sizeof(completed));
                            str::concat(completed,
                                        (matchedEntry.type == fs::NodeType::Directory) ? "/" : " ",
                                        sizeof(completed));
                            replaceLine(outBuf, maxLen, len, cursor, completed);
                        }
                    }
                }
            }
            continue;
        }

        if (static_cast<u8>(ch) >= 0x20U && len + 1 < maxLen) {
            if (cursor == len) {
                outBuf[len++] = ch;
                outBuf[len]   = '\0';
                ++cursor;
                klog::Logger::instance().putChar(ch);
            } else {
                memmove(outBuf + cursor + 1, outBuf + cursor, len - cursor + 1);
                outBuf[cursor] = ch;
                ++len;
                ++cursor;
                usize tail = len - cursor + 1;
                klog::Logger::instance().writeRaw(outBuf + cursor - 1, tail);
                if (len > cursor) {
                    kprintf("\033[%zuD", len - cursor);
                }
            }
        }
    }
}

} // namespace cortez::shell
