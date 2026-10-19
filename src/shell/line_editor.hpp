/**
 * @file line_editor.hpp
 * @brief Interactive VT100/ANSI Shell Line Editor with cursor movement and command history.
 */

#pragma once

#include "../lib/types.hpp"

namespace cortez::shell {

class LineEditor final {
public:
    static constexpr usize HISTORY_CAPACITY = 32;
    static constexpr usize MAX_LINE_LEN     = 1024;

    static LineEditor& instance() noexcept;

    void        readLine(char* outBuf, usize maxLen) noexcept;
    void        addHistory(const char* line) noexcept;
    usize       getHistoryCount() const noexcept { return m_historyCount; }
    const char* getHistoryAt(usize index) const noexcept;

private:
    LineEditor() noexcept = default;

    void replaceLine(char* buf, usize maxLen, usize& len, usize& cursor, const char* newText) noexcept;

    char  m_history[HISTORY_CAPACITY][MAX_LINE_LEN]{};
    usize m_historyHead  = 0; // Oldest entry index
    usize m_historyCount = 0;
    char  m_draft[MAX_LINE_LEN]{};
};

} // namespace cortez::shell
