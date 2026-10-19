/**
 * @file terminal_driver.hpp
 * @brief VT100/ANSI GOP Framebuffer Terminal Driver implementing ICharDevice.
 */

#pragma once

#include "../i_device.hpp"
#include "gop_framebuffer.hpp"

namespace cortez::drivers::video {

class TerminalDriver final : public ICharDevice {
public:
    static constexpr u32 MAX_COLS         = 256;
    static constexpr u32 MAX_ROWS         = 128;
    static constexpr u32 DEFAULT_FG_COLOR = 0xE5E9F0U; // Crisp Nord snow white
    static constexpr u32 DEFAULT_BG_COLOR = 0x0D1117U; // Deep obsidian background

    // Standard high-contrast RGB colors for full-screen UI applications
    static constexpr u32 COLOR_BLACK         = 0x484F58U;
    static constexpr u32 COLOR_RED           = 0xFF7B72U;
    static constexpr u32 COLOR_GREEN         = 0x3FB950U;
    static constexpr u32 COLOR_YELLOW        = 0xD29922U;
    static constexpr u32 COLOR_BLUE          = 0x58A6FFU;
    static constexpr u32 COLOR_MAGENTA       = 0xBC8CFFU;
    static constexpr u32 COLOR_CYAN          = 0x39C5CFU;
    static constexpr u32 COLOR_WHITE         = 0xE5E9F0U;
    static constexpr u32 COLOR_BRIGHT_GREY   = 0x8B949EU;
    static constexpr u32 COLOR_BRIGHT_RED    = 0xFFA198U;
    static constexpr u32 COLOR_BRIGHT_GREEN  = 0x56D364U;
    static constexpr u32 COLOR_BRIGHT_YELLOW = 0xE3B341U;
    static constexpr u32 COLOR_BRIGHT_BLUE   = 0x79C0FFU;
    static constexpr u32 COLOR_BRIGHT_MAG    = 0xD2A8FFU;
    static constexpr u32 COLOR_BRIGHT_CYAN   = 0x56D4DDU;
    static constexpr u32 COLOR_BRIGHT_WHITE  = 0xFFFFFFU;
    static constexpr u32 COLOR_BAR_BG        = 0xC9D1D9U; // Inverted title/status bar background
    static constexpr u32 COLOR_BAR_FG        = 0x0D1117U; // Inverted title/status bar text

    static TerminalDriver& instance() noexcept;

    Status init(const boot::FramebufferInfo& fbInfo) noexcept;

    const char*   getName() const noexcept override { return "tty0"; }
    Result<usize> read(void* buffer, usize count) override;
    Result<usize> write(const void* buffer, usize count) override;
    bool          hasData() const noexcept override { return false; }

    void clear() noexcept;
    void putChar(char c) noexcept;
    void drawCursor(bool visible) noexcept;
    void setCursorVisible(bool visible) noexcept;

    u32  getCols() const noexcept { return m_cols; }
    u32  getRows() const noexcept { return m_rows; }
    u32  getCursorCol() const noexcept { return m_cursorCol; }
    u32  getCursorRow() const noexcept { return m_cursorRow; }
    void setCursorPos(u32 col, u32 row) noexcept;

    // Direct, flicker-free cell and row rendering with differential caching
    void putCellAt(u32 col, u32 row, char ch, u32 fgColor, u32 bgColor) noexcept;
    void writeStringAt(u32 col, u32 row, const char* str, usize len, u32 fgColor, u32 bgColor) noexcept;
    void fillRowAt(u32 col, u32 row, u32 count, char ch, u32 fgColor, u32 bgColor) noexcept;

private:
    struct Cell {
        char ch;
        u32  fg;
        u32  bg;
    };

    TerminalDriver() noexcept = default;

    void scrollUp() noexcept;
    void handleAnsiChar(char c) noexcept;
    void applyAnsiCode(u32 code) noexcept;
    void moveCursorLeft(u32 count) noexcept;
    void moveCursorRight(u32 count) noexcept;
    void eraseInLine(u32 mode) noexcept;

    GopFramebuffer m_fb{};
    u32            m_cols          = 80;
    u32            m_rows          = 25;
    u32            m_cursorCol     = 0;
    u32            m_cursorRow     = 0;
    u32            m_fgColor       = DEFAULT_FG_COLOR;
    u32            m_bgColor       = DEFAULT_BG_COLOR;
    bool           m_cursorVisible = true;

    enum class AnsiState : u8 { Normal, Escape, Bracket } m_ansiState = AnsiState::Normal;
    u32  m_ansiParams[4]{};
    u8   m_ansiParamCount = 0;
    bool m_ansiPrivate    = false;

    Cell m_grid[MAX_ROWS][MAX_COLS]{};
};

} // namespace cortez::drivers::video
