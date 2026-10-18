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

    static TerminalDriver& instance() noexcept;

    Status init(const boot::FramebufferInfo& fbInfo) noexcept;

    const char*   getName() const noexcept override { return "tty0"; }
    Result<usize> read(void* buffer, usize count) override;
    Result<usize> write(const void* buffer, usize count) override;
    bool          hasData() const noexcept override { return false; }

    void clear() noexcept;
    void putChar(char c) noexcept;
    void drawCursor(bool visible) noexcept;

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

    GopFramebuffer m_fb{};
    u32            m_cols       = 80;
    u32            m_rows       = 25;
    u32            m_cursorCol  = 0;
    u32            m_cursorRow  = 0;
    u32            m_fgColor    = DEFAULT_FG_COLOR;
    u32            m_bgColor    = DEFAULT_BG_COLOR;

    enum class AnsiState : u8 { Normal, Escape, Bracket } m_ansiState = AnsiState::Normal;
    u32  m_ansiParam = 0;

    Cell m_grid[MAX_ROWS][MAX_COLS]{};
};

} // namespace cortez::drivers::video
