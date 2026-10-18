/**
 * @file terminal_driver.cpp
 * @brief Implementation of the GOP Framebuffer VT100/ANSI Terminal Driver.
 */

#include "terminal_driver.hpp"

namespace cortez::drivers::video {

TerminalDriver& TerminalDriver::instance() noexcept {
    static TerminalDriver s_term;
    return s_term;
}

Status TerminalDriver::init(const boot::FramebufferInfo& fbInfo) noexcept {
    Status st = m_fb.init(fbInfo);
    if (st != Status::Ok) return st;

    m_cols = (m_fb.getCols() > MAX_COLS) ? MAX_COLS : m_fb.getCols();
    m_rows = (m_fb.getRows() > MAX_ROWS) ? MAX_ROWS : m_fb.getRows();

    clear();
    return Status::Ok;
}

void TerminalDriver::clear() noexcept {
    m_cursorCol = 0;
    m_cursorRow = 0;
    m_fgColor   = DEFAULT_FG_COLOR;
    m_bgColor   = DEFAULT_BG_COLOR;

    for (u32 r = 0; r < m_rows; ++r) {
        for (u32 c = 0; c < m_cols; ++c) {
            m_grid[r][c] = { ' ', DEFAULT_FG_COLOR, DEFAULT_BG_COLOR };
        }
    }

    m_fb.clearScreen(DEFAULT_BG_COLOR);
}

void TerminalDriver::drawCursor(bool visible) noexcept {
    if (m_cursorCol >= m_cols || m_cursorRow >= m_rows) return;
    const Cell& cell = m_grid[m_cursorRow][m_cursorCol];
    if (visible) {
        m_fb.drawGlyph8x16(m_cursorCol, m_cursorRow, cell.ch ? cell.ch : ' ', cell.bg, 0x58A6FFU);
    } else {
        m_fb.drawGlyph8x16(m_cursorCol, m_cursorRow, cell.ch ? cell.ch : ' ', cell.fg, cell.bg);
    }
}

void TerminalDriver::scrollUp() noexcept {
    for (u32 r = 0; r + 1 < m_rows; ++r) {
        for (u32 c = 0; c < m_cols; ++c) {
            Cell next = m_grid[r + 1][c];
            Cell curr = m_grid[r][c];
            if (next.ch != curr.ch || next.fg != curr.fg || next.bg != curr.bg) {
                m_grid[r][c] = next;
                m_fb.drawGlyph8x16(c, r, next.ch, next.fg, next.bg);
            }
        }
    }

    u32 lastRow = m_rows - 1;
    for (u32 c = 0; c < m_cols; ++c) {
        m_grid[lastRow][c] = { ' ', DEFAULT_FG_COLOR, DEFAULT_BG_COLOR };
        m_fb.drawGlyph8x16(c, lastRow, ' ', DEFAULT_FG_COLOR, DEFAULT_BG_COLOR);
    }
}

void TerminalDriver::applyAnsiCode(u32 code) noexcept {
    switch (code) {
        case 0:  m_fgColor = DEFAULT_FG_COLOR; m_bgColor = DEFAULT_BG_COLOR; break;
        case 30: m_fgColor = 0x484F58U; break;
        case 31: m_fgColor = 0xFF7B72U; break; // Red
        case 32: m_fgColor = 0x3FB950U; break; // Green
        case 33: m_fgColor = 0xD29922U; break; // Yellow
        case 34: m_fgColor = 0x58A6FFU; break; // Blue
        case 35: m_fgColor = 0xBC8CFFU; break; // Magenta
        case 36: m_fgColor = 0x39C5CFU; break; // Cyan
        case 37: m_fgColor = 0xE5E9F0U; break; // White
        case 90: m_fgColor = 0x8B949EU; break; // Bright Grey
        case 91: m_fgColor = 0xFFA198U; break; // Bright Red
        case 92: m_fgColor = 0x56D364U; break; // Bright Green
        case 93: m_fgColor = 0xE3B341U; break; // Bright Yellow
        case 94: m_fgColor = 0x79C0FFU; break; // Bright Blue
        case 96: m_fgColor = 0x56D4DDU; break; // Bright Cyan
        default: break;
    }
}

void TerminalDriver::handleAnsiChar(char c) noexcept {
    if (m_ansiState == AnsiState::Escape) {
        if (c == '[') {
            m_ansiState = AnsiState::Bracket;
            m_ansiParam = 0;
        } else {
            m_ansiState = AnsiState::Normal;
        }
        return;
    }

    if (c >= '0' && c <= '9') {
        m_ansiParam = m_ansiParam * 10 + static_cast<u32>(c - '0');
        return;
    }

    if (c == ';') {
        applyAnsiCode(m_ansiParam);
        m_ansiParam = 0;
        return;
    }

    if (c == 'm') {
        applyAnsiCode(m_ansiParam);
    } else if (c == 'J' && m_ansiParam == 2) {
        clear();
    } else if (c == 'H') {
        m_cursorCol = 0;
        m_cursorRow = 0;
    }

    m_ansiState = AnsiState::Normal;
}

void TerminalDriver::putChar(char c) noexcept {
    if (!m_fb.isReady()) return;

    if (m_ansiState != AnsiState::Normal) {
        handleAnsiChar(c);
        return;
    }

    if (c == '\033') {
        m_ansiState = AnsiState::Escape;
        return;
    }

    drawCursor(false);

    if (c == '\r') {
        m_cursorCol = 0;
    } else if (c == '\n') {
        m_cursorCol = 0;
        if (++m_cursorRow >= m_rows) {
            m_cursorRow = m_rows - 1;
            scrollUp();
        }
    } else if (c == '\b') {
        if (m_cursorCol > 0) {
            --m_cursorCol;
        } else if (m_cursorRow > 0) {
            --m_cursorRow;
            m_cursorCol = m_cols - 1;
        }
        m_grid[m_cursorRow][m_cursorCol] = { ' ', m_fgColor, m_bgColor };
        m_fb.drawGlyph8x16(m_cursorCol, m_cursorRow, ' ', m_fgColor, m_bgColor);
    } else if (c == '\t') {
        u32 spaces = 4 - (m_cursorCol % 4);
        for (u32 i = 0; i < spaces; ++i) {
            putChar(' ');
        }
        return;
    } else if (static_cast<u8>(c) >= 0x20U) {
        m_grid[m_cursorRow][m_cursorCol] = { c, m_fgColor, m_bgColor };
        m_fb.drawGlyph8x16(m_cursorCol, m_cursorRow, c, m_fgColor, m_bgColor);
        if (++m_cursorCol >= m_cols) {
            m_cursorCol = 0;
            if (++m_cursorRow >= m_rows) {
                m_cursorRow = m_rows - 1;
                scrollUp();
            }
        }
    }

    drawCursor(true);
}

Result<usize> TerminalDriver::read(void*, usize) {
    return Status::NotSupported;
}

Result<usize> TerminalDriver::write(const void* buffer, usize count) {
    if (!buffer) return Status::InvalidArgument;
    const auto* chars = static_cast<const char*>(buffer);
    for (usize i = 0; i < count; ++i) {
        putChar(chars[i]);
    }
    return count;
}

} // namespace cortez::drivers::video
