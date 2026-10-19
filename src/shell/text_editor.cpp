/**
 * @file text_editor.cpp
 * @brief Implementation of the full-screen interactive VT100/ANSI text editor.
 */

#include "text_editor.hpp"
#include "../drivers/input/ps2_keyboard.hpp"
#include "../drivers/video/terminal_driver.hpp"
#include "../fs/vfs_manager.hpp"
#include "../proc/scheduler.hpp"
#include "../lib/string_utils.hpp"
#include "../lib/printk.hpp"

namespace cortez::shell {

namespace {

struct EditorBuffer {
    char  lines[TextEditor::MAX_LINES][TextEditor::MAX_LINE_COLS];
    usize lineCount;
    usize curRow;
    usize curCol;
    usize scrollRow;
    bool  modified;
    char  statusMsg[96];
};

void loadIntoBuffer(EditorBuffer* ed, const char* cwd, const char* filePath) noexcept {
    ed->lineCount    = 1;
    ed->lines[0][0]  = '\0';
    ed->curRow       = 0;
    ed->curCol       = 0;
    ed->scrollRow    = 0;
    ed->modified     = false;
    ed->statusMsg[0] = '\0';

    auto& vfs = fs::VfsManager::instance();
    fs::FileStat st{};
    if (vfs.stat(cwd, filePath, &st) != Status::Ok || st.type != fs::NodeType::File) {
        str::copy(ed->statusMsg, "[New File]", sizeof(ed->statusMsg));
        return;
    }

    if (st.size == 0) {
        str::copy(ed->statusMsg, "[Loaded empty file]", sizeof(ed->statusMsg));
        return;
    }

    auto fdRes = vfs.open(cwd, filePath, fs::O_RDONLY);
    if (fdRes.isError()) return;

    i32 fd = fdRes.value();
    usize fileSize = static_cast<usize>(st.size);
    char* raw = new char[fileSize + 1];
    if (!raw) {
        vfs.close(fd);
        return;
    }

    auto rRes = vfs.read(fd, raw, fileSize);
    vfs.close(fd);

    usize bytesRead = rRes.isOk() ? rRes.value() : 0;
    raw[bytesRead]  = '\0';

    ed->lineCount = 0;
    usize lineIdx = 0;
    usize colIdx  = 0;
    ed->lines[0][0] = '\0';

    for (usize i = 0; i < bytesRead && lineIdx < TextEditor::MAX_LINES; ++i) {
        char c = raw[i];
        if (c == '\r') continue;
        if (c == '\n') {
            ed->lines[lineIdx][colIdx] = '\0';
            ++lineIdx;
            colIdx = 0;
            if (lineIdx < TextEditor::MAX_LINES) {
                ed->lines[lineIdx][0] = '\0';
            }
        } else if (colIdx + 1 < TextEditor::MAX_LINE_COLS) {
            ed->lines[lineIdx][colIdx++] = c;
            ed->lines[lineIdx][colIdx]   = '\0';
        }
    }

    ed->lineCount = (lineIdx < TextEditor::MAX_LINES) ? (lineIdx + 1) : TextEditor::MAX_LINES;
    // If file ended with a newline and has >1 lines, trim trailing empty phantom line if desired
    if (ed->lineCount > 1 && bytesRead > 0 && raw[bytesRead - 1] == '\n' &&
        ed->lines[ed->lineCount - 1][0] == '\0') {
        --ed->lineCount;
    }

    delete[] raw;
    str::copy(ed->statusMsg, "[Loaded from disk]", sizeof(ed->statusMsg));
}

Status saveBuffer(EditorBuffer* ed, const char* cwd, const char* filePath) noexcept {
    usize totalCap = ed->lineCount * (TextEditor::MAX_LINE_COLS + 1) + 16;
    char* out      = new char[totalCap];
    if (!out) return Status::OutOfMemory;

    usize pos = 0;
    for (usize r = 0; r < ed->lineCount; ++r) {
        usize l = str::length(ed->lines[r]);
        if (pos + l + 2 >= totalCap) break;
        memcpy(out + pos, ed->lines[r], l);
        pos += l;
        out[pos++] = '\n';
    }

    auto& vfs = fs::VfsManager::instance();
    Status st = vfs.writeFileAll(cwd, filePath, out, pos);
    delete[] out;

    if (st == Status::Ok) {
        (void)vfs.sync();
        ed->modified = false;
        str::copy(ed->statusMsg, "[Saved to disk]", sizeof(ed->statusMsg));
    } else {
        str::copy(ed->statusMsg, "[Save ERROR]", sizeof(ed->statusMsg));
    }
    return st;
}

void renderScreen(const EditorBuffer* ed, const char* filePath) noexcept {
    auto& term   = drivers::video::TerminalDriver::instance();
    u32 termCols = term.getCols();
    u32 termRows = term.getRows();
    if (termRows < 4) return;

    u32 viewRows = termRows - 2; // Top bar + bottom bar

    kprintf("\033[?25l\033[1;1H");

    // Top header bar
    kprintf("\033[44;97m Cortez Edit | %s %s | Line %zu/%zu Col %zu %s\033[K\033[0m\n",
            filePath,
            ed->modified ? "[*]" : "   ",
            ed->curRow + 1,
            ed->lineCount,
            ed->curCol + 1,
            ed->statusMsg);

    // Body rows
    u32 maxTextCols = (termCols > 6) ? (termCols - 6) : 1;
    for (u32 vr = 0; vr < viewRows; ++vr) {
        usize docRow = ed->scrollRow + vr;
        if (docRow < ed->lineCount) {
            kprintf("\033[90m%4zu \033[0m", docRow + 1);
            const char* line = ed->lines[docRow];
            usize lineLen    = str::length(line);
            usize showLen    = (lineLen > maxTextCols) ? maxTextCols : lineLen;
            if (showLen > 0) {
                klog::Logger::instance().writeRaw(line, showLen);
            }
            kprintf("\033[K\n");
        } else {
            kprintf("\033[90m   ~\033[0m\033[K\n");
        }
    }

    // Bottom status/help bar
    kprintf("\033[44;97m ^S Save   ^X Exit   ^K Cut Line   Arrows/Home/End/PgUp/PgDn Move\033[K\033[0m");

    // Position cursor inside viewport (1-based ANSI coordinates)
    u32 screenRow = static_cast<u32>((ed->curRow - ed->scrollRow) + 2);
    u32 screenCol = static_cast<u32>(ed->curCol + 6);
    if (screenCol > termCols) screenCol = termCols;
    kprintf("\033[%u;%uH\033[?25h", screenRow, screenCol);
}

} // namespace

Status TextEditor::openAndEdit(const char* cwd, const char* filePath) noexcept {
    if (!filePath || !filePath[0]) return Status::InvalidArgument;

    auto* ed = new EditorBuffer();
    if (!ed) return Status::OutOfMemory;

    loadIntoBuffer(ed, cwd, filePath);

    auto& kbd  = drivers::input::Ps2KeyboardDriver::instance();
    auto& term = drivers::video::TerminalDriver::instance();

    enum class EscState : u8 { None, Esc, Bracket, TildeParam };
    EscState escState = EscState::None;
    u32      escParam = 0;

    auto ensureVisible = [&]() {
        u32 viewRows = (term.getRows() > 2) ? (term.getRows() - 2) : 1;
        if (ed->curRow < ed->scrollRow) {
            ed->scrollRow = ed->curRow;
        } else if (ed->curRow >= ed->scrollRow + viewRows) {
            ed->scrollRow = ed->curRow - viewRows + 1;
        }
        usize lineLen = str::length(ed->lines[ed->curRow]);
        if (ed->curCol > lineLen) {
            ed->curCol = lineLen;
        }
    };

    renderScreen(ed, filePath);

    for (;;) {
        if (!kbd.hasData()) {
            kbd.pollHardware();
            if (!kbd.hasData()) {
                proc::Scheduler::instance().yield();
            }
        }
        char ch = kbd.readCharBlocking();

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

            u32 viewRows = (term.getRows() > 2) ? (term.getRows() - 2) : 1;
            if (ch == 'A') { // Up
                if (ed->curRow > 0) --ed->curRow;
            } else if (ch == 'B') { // Down
                if (ed->curRow + 1 < ed->lineCount) ++ed->curRow;
            } else if (ch == 'C') { // Right
                usize lineLen = str::length(ed->lines[ed->curRow]);
                if (ed->curCol < lineLen) {
                    ++ed->curCol;
                } else if (ed->curRow + 1 < ed->lineCount) {
                    ++ed->curRow;
                    ed->curCol = 0;
                }
            } else if (ch == 'D') { // Left
                if (ed->curCol > 0) {
                    --ed->curCol;
                } else if (ed->curRow > 0) {
                    --ed->curRow;
                    ed->curCol = str::length(ed->lines[ed->curRow]);
                }
            } else if (ch == 'H') { // Home
                ed->curCol = 0;
            } else if (ch == 'F') { // End
                ed->curCol = str::length(ed->lines[ed->curRow]);
            } else if (ch == '~') {
                if (escParam == 1 || escParam == 7) {
                    ed->curCol = 0;
                } else if (escParam == 4 || escParam == 8) {
                    ed->curCol = str::length(ed->lines[ed->curRow]);
                } else if (escParam == 5) { // PgUp
                    ed->curRow = (ed->curRow >= viewRows) ? (ed->curRow - viewRows) : 0;
                } else if (escParam == 6) { // PgDn
                    ed->curRow = (ed->curRow + viewRows < ed->lineCount)
                                     ? (ed->curRow + viewRows)
                                     : (ed->lineCount - 1);
                } else if (escParam == 3) { // Delete
                    char* line    = ed->lines[ed->curRow];
                    usize lineLen = str::length(line);
                    if (ed->curCol < lineLen) {
                        memmove(line + ed->curCol, line + ed->curCol + 1, lineLen - ed->curCol);
                        ed->modified = true;
                    } else if (ed->curRow + 1 < ed->lineCount) {
                        const char* nextLine = ed->lines[ed->curRow + 1];
                        str::concat(line, nextLine, MAX_LINE_COLS);
                        for (usize r = ed->curRow + 1; r + 1 < ed->lineCount; ++r) {
                            str::copy(ed->lines[r], ed->lines[r + 1], MAX_LINE_COLS);
                        }
                        --ed->lineCount;
                        ed->modified = true;
                    }
                }
            }

            ensureVisible();
            renderScreen(ed, filePath);
            continue;
        }

        if (ch == '\033') {
            escState = EscState::Esc;
            continue;
        }

        if (ch == '\x13' || ch == '\x0F') { // Ctrl+S or Ctrl+O: Save
            saveBuffer(ed, cwd, filePath);
            renderScreen(ed, filePath);
            continue;
        }

        if (ch == '\x18' || ch == '\x11') { // Ctrl+X or Ctrl+Q: Exit
            term.clear();
            delete ed;
            return Status::Ok;
        }

        if (ch == '\x01') { // Ctrl+A: Home
            ed->curCol = 0;
            renderScreen(ed, filePath);
            continue;
        }

        if (ch == '\x05') { // Ctrl+E: End
            ed->curCol = str::length(ed->lines[ed->curRow]);
            renderScreen(ed, filePath);
            continue;
        }

        if (ch == '\x0B') { // Ctrl+K: Delete current line
            if (ed->lineCount == 1) {
                ed->lines[0][0] = '\0';
                ed->curCol      = 0;
            } else {
                for (usize r = ed->curRow; r + 1 < ed->lineCount; ++r) {
                    str::copy(ed->lines[r], ed->lines[r + 1], MAX_LINE_COLS);
                }
                --ed->lineCount;
                if (ed->curRow >= ed->lineCount) {
                    ed->curRow = ed->lineCount - 1;
                }
            }
            ed->modified = true;
            ensureVisible();
            renderScreen(ed, filePath);
            continue;
        }

        if (ch == '\r' || ch == '\n') { // Enter: split line
            if (ed->lineCount + 1 < MAX_LINES) {
                for (usize r = ed->lineCount; r > ed->curRow + 1; --r) {
                    str::copy(ed->lines[r], ed->lines[r - 1], MAX_LINE_COLS);
                }
                char* currLine = ed->lines[ed->curRow];
                str::copy(ed->lines[ed->curRow + 1], currLine + ed->curCol, MAX_LINE_COLS);
                currLine[ed->curCol] = '\0';
                ++ed->lineCount;
                ++ed->curRow;
                ed->curCol   = 0;
                ed->modified = true;
            }
            ensureVisible();
            renderScreen(ed, filePath);
            continue;
        }

        if (ch == '\b' || ch == 127) { // Backspace
            char* line = ed->lines[ed->curRow];
            usize lineLen = str::length(line);
            if (ed->curCol > 0) {
                memmove(line + ed->curCol - 1, line + ed->curCol, lineLen - ed->curCol + 1);
                --ed->curCol;
                ed->modified = true;
            } else if (ed->curRow > 0) {
                usize prevLen = str::length(ed->lines[ed->curRow - 1]);
                str::concat(ed->lines[ed->curRow - 1], line, MAX_LINE_COLS);
                for (usize r = ed->curRow; r + 1 < ed->lineCount; ++r) {
                    str::copy(ed->lines[r], ed->lines[r + 1], MAX_LINE_COLS);
                }
                --ed->lineCount;
                --ed->curRow;
                ed->curCol   = prevLen;
                ed->modified = true;
            }
            ensureVisible();
            renderScreen(ed, filePath);
            continue;
        }

        if (ch == '\t') { // Tab -> 4 spaces
            char* line    = ed->lines[ed->curRow];
            usize lineLen = str::length(line);
            for (u32 s = 0; s < 4 && lineLen + 1 < MAX_LINE_COLS; ++s) {
                memmove(line + ed->curCol + 1, line + ed->curCol, lineLen - ed->curCol + 1);
                line[ed->curCol] = ' ';
                ++ed->curCol;
                ++lineLen;
                ed->modified = true;
            }
            ensureVisible();
            renderScreen(ed, filePath);
            continue;
        }

        if (static_cast<u8>(ch) >= 0x20U) {
            char* line    = ed->lines[ed->curRow];
            usize lineLen = str::length(line);
            if (lineLen + 1 < MAX_LINE_COLS) {
                memmove(line + ed->curCol + 1, line + ed->curCol, lineLen - ed->curCol + 1);
                line[ed->curCol] = ch;
                ++ed->curCol;
                ed->modified = true;
                ed->statusMsg[0] = '\0';
            }
            ensureVisible();
            renderScreen(ed, filePath);
        }
    }
}

} // namespace cortez::shell
