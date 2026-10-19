/**
 * @file text_editor.hpp
 * @brief Full-screen interactive VT100/ANSI text editor (edit command) for authoring C modules at runtime.
 */

#pragma once

#include "../lib/types.hpp"
#include "../lib/result.hpp"

namespace cortez::shell {

class TextEditor final {
public:
    static constexpr usize MAX_LINES     = 512;
    static constexpr usize MAX_LINE_COLS = 160;

    static Status openAndEdit(const char* cwd, const char* filePath) noexcept;
};

} // namespace cortez::shell
