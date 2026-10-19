/**
 * @file text_editor.hpp
 * @brief GNU nano-style full-screen interactive VT100/ANSI text editor (nano / edit commands).
 */

#pragma once

#include "../lib/types.hpp"
#include "../lib/result.hpp"

namespace cortez::shell {

class TextEditor final {
public:
    static constexpr usize MAX_LINES        = 2048;
    static constexpr usize INITIAL_LINE_CAP = 128;
    static constexpr usize MAX_LINE_CAP     = 4096;
    static constexpr usize MAX_CUT_LINES    = 256;
    static constexpr usize MAX_UNDO_STEPS   = 16;

    static Status openAndEdit(const char* cwd,
                              const char* filePath = nullptr,
                              usize initialLine    = 0,
                              usize initialCol     = 0) noexcept;
};

} // namespace cortez::shell
