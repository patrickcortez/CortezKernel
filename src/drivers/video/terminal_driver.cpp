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

void TerminalDriver::setCursorVisible(bool visible) noexcept {
    if (m_cursorVisible == visible) return;
    drawCursor(false);
    m_cursorVisible = visible;
    drawCursor(m_cursorVisible);
}

void TerminalDriver::setCursorPos(u32 col, u32 row) noexcept {
    drawCursor(false);
    m_cursorCol = (col < m_cols) ? col : (m_cols ? m_cols - 1 : 0);
    m_cursorRow = (row < m_rows) ? row : (m_rows ? m_rows - 1 : 0);
    drawCursor(m_cursorVisible);
}

void TerminalDriver::putCellAt(u32 col, u32 row, char ch, u32 fgColor, u32 bgColor) noexcept {
    if (!m_fb.isReady() || col >= m_cols || row >= m_rows) return;
    char renderCh = (static_cast<u8>(ch) >= 0x20U) ? ch : ' ';
    Cell& curr = m_grid[row][col];
    if (curr.ch != renderCh || curr.fg != fgColor || curr.bg != bgColor) {
        curr = { renderCh, fgColor, bgColor };
        m_fb.drawGlyph8x16(col, row, renderCh, fgColor, bgColor);
    }
}

void TerminalDriver::writeStringAt(u32 col, u32 row, const char* str, usize len, u32 fgColor, u32 bgColor) noexcept {
    if (!str || !m_fb.isReady() || row >= m_rows || col >= m_cols) return;
    u32 maxCols = m_cols - col;
    usize count = (len < maxCols) ? len : maxCols;
    for (usize i = 0; i < count; ++i) {
        putCellAt(col + static_cast<u32>(i), row, str[i], fgColor, bgColor);
    }
}

void TerminalDriver::fillRowAt(u32 col, u32 row, u32 count, char ch, u32 fgColor, u32 bgColor) noexcept {
    if (!m_fb.isReady() || row >= m_rows || col >= m_cols) return;
    u32 maxCols = m_cols - col;
    u32 n = (count < maxCols) ? count : maxCols;
    for (u32 i = 0; i < n; ++i) {
        putCellAt(col + i, row, ch, fgColor, bgColor);
    }
}

void TerminalDriver::moveCursorLeft(u32 count) noexcept {
    while (count > 0) {
        if (m_cursorCol > 0) {
            --m_cursorCol;
        } else if (m_cursorRow > 0) {
            --m_cursorRow;
            m_cursorCol = m_cols - 1;
        } else {
            break;
        }
        --count;
    }
}

void TerminalDriver::moveCursorRight(u32 count) noexcept {
    while (count > 0) {
        if (m_cursorCol + 1 < m_cols) {
            ++m_cursorCol;
        } else if (m_cursorRow + 1 < m_rows) {
            m_cursorCol = 0;
            ++m_cursorRow;
        } else {
            break;
        }
        --count;
    }
}

void TerminalDriver::eraseInLine(u32 mode) noexcept {
    u32 startCol = 0;
    u32 endCol   = m_cols;
    if (mode == 0) {
        startCol = m_cursorCol;
    } else if (mode == 1) {
        endCol = (m_cursorCol + 1 <= m_cols) ? (m_cursorCol + 1) : m_cols;
    }
    for (u32 c = startCol; c < endCol; ++c) {
        m_grid[m_cursorRow][c] = { ' ', DEFAULT_FG_COLOR, DEFAULT_BG_COLOR };
        m_fb.drawGlyph8x16(c, m_cursorRow, ' ', DEFAULT_FG_COLOR, DEFAULT_BG_COLOR);
    }
}

void TerminalDriver::applyAnsiCode(u32 code) noexcept {
    switch (code) {
        case 0:  m_fgColor = DEFAULT_FG_COLOR; m_bgColor = DEFAULT_BG_COLOR; break;
        case 7:  { u32 tmp = m_fgColor; m_fgColor = m_bgColor; m_bgColor = tmp; break; }
        case 27: m_fgColor = DEFAULT_FG_COLOR; m_bgColor = DEFAULT_BG_COLOR; break;
        case 30: m_fgColor = 0x484F58U; break;
        case 31: m_fgColor = 0xFF7B72U; break; // Red
        case 32: m_fgColor = 0x3FB950U; break; // Green
        case 33: m_fgColor = 0xD29922U; break; // Yellow
        case 34: m_fgColor = 0x58A6FFU; break; // Blue
        case 35: m_fgColor = 0xBC8CFFU; break; // Magenta
        case 36: m_fgColor = 0x39C5CFU; break; // Cyan
        case 37: m_fgColor = 0xE5E9F0U; break; // White
        case 40: m_bgColor = 0x0D1117U; break;
        case 41: m_bgColor = 0x6E1A1AU; break;
        case 42: m_bgColor = 0x1B4B27U; break;
        case 43: m_bgColor = 0x5A4310U; break;
        case 44: m_bgColor = 0x1F4E79U; break;
        case 45: m_bgColor = 0x4B2E73U; break;
        case 46: m_bgColor = 0x164E54U; break;
        case 47: m_bgColor = 0xC9D1D9U; break;
        case 90: m_fgColor = 0x8B949EU; break; // Bright Grey
        case 91: m_fgColor = 0xFFA198U; break; // Bright Red
        case 92: m_fgColor = 0x56D364U; break; // Bright Green
        case 93: m_fgColor = 0xE3B341U; break; // Bright Yellow
        case 94: m_fgColor = 0x79C0FFU; break; // Bright Blue
        case 95: m_fgColor = 0xD2A8FFU; break; // Bright Magenta
        case 96: m_fgColor = 0x56D4DDU; break; // Bright Cyan
        case 97: m_fgColor = 0xFFFFFFU; break; // Bright White
        default: break;
    }
}

void TerminalDriver::handleAnsiChar(char c) noexcept {
    if (m_ansiState == AnsiState::Escape) {
        if (c == '[') {
            m_ansiState      = AnsiState::Bracket;
            m_ansiParamCount = 0;
            m_ansiPrivate    = false;
            for (u32 i = 0; i < 4; ++i) m_ansiParams[i] = 0;
        } else {
            m_ansiState = AnsiState::Normal;
        }
        return;
    }

    if (c == '?') {
        m_ansiPrivate = true;
        return;
    }

    if (c >= '0' && c <= '9') {
        if (m_ansiParamCount == 0) m_ansiParamCount = 1;
        u8 idx = m_ansiParamCount - 1;
        if (idx < 4) {
            m_ansiParams[idx] = m_ansiParams[idx] * 10 + static_cast<u32>(c - '0');
        }
        return;
    }

    if (c == ';') {
        if (m_ansiParamCount == 0) m_ansiParamCount = 1;
        if (m_ansiParamCount < 4) {
            ++m_ansiParamCount;
        }
        return;
    }

    drawCursor(false);

    u32 p0 = (m_ansiParamCount >= 1) ? m_ansiParams[0] : 0;
    u32 p1 = (m_ansiParamCount >= 2) ? m_ansiParams[1] : 0;

    if (m_ansiPrivate) {
        if (p0 == 25) {
            if (c == 'l') m_cursorVisible = false;
            if (c == 'h') m_cursorVisible = true;
        }
        m_ansiState = AnsiState::Normal;
        drawCursor(m_cursorVisible);
        return;
    }

    switch (c) {
        case 'm': {
            if (m_ansiParamCount == 0) {
                applyAnsiCode(0);
            } else {
                for (u8 i = 0; i < m_ansiParamCount && i < 4; ++i) {
                    applyAnsiCode(m_ansiParams[i]);
                }
            }
            break;
        }
        case 'A': {
            u32 n = (p0 == 0) ? 1 : p0;
            m_cursorRow = (m_cursorRow >= n) ? (m_cursorRow - n) : 0;
            break;
        }
        case 'B': {
            u32 n = (p0 == 0) ? 1 : p0;
            m_cursorRow = (m_cursorRow + n < m_rows) ? (m_cursorRow + n) : (m_rows - 1);
            break;
        }
        case 'C': {
            moveCursorRight((p0 == 0) ? 1 : p0);
            break;
        }
        case 'D': {
            moveCursorLeft((p0 == 0) ? 1 : p0);
            break;
        }
        case 'G': {
            u32 col = (p0 > 0) ? (p0 - 1) : 0;
            m_cursorCol = (col < m_cols) ? col : (m_cols - 1);
            break;
        }
        case 'H':
        case 'f': {
            u32 row = (p0 > 0) ? (p0 - 1) : 0;
            u32 col = (p1 > 0) ? (p1 - 1) : 0;
            m_cursorRow = (row < m_rows) ? row : (m_rows - 1);
            m_cursorCol = (col < m_cols) ? col : (m_cols - 1);
            break;
        }
        case 'J': {
            if (p0 == 2) {
                clear();
            } else if (p0 == 0) {
                eraseInLine(0);
                for (u32 r = m_cursorRow + 1; r < m_rows; ++r) {
                    for (u32 col = 0; col < m_cols; ++col) {
                        m_grid[r][col] = { ' ', DEFAULT_FG_COLOR, DEFAULT_BG_COLOR };
                        m_fb.drawGlyph8x16(col, r, ' ', DEFAULT_FG_COLOR, DEFAULT_BG_COLOR);
                    }
                }
            }
            break;
        }
        case 'K': {
            eraseInLine(p0);
            break;
        }
        default:
            break;
    }

    m_ansiState = AnsiState::Normal;
    drawCursor(m_cursorVisible);
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

    drawCursor(m_cursorVisible);
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
