/**
 * @file text_editor.cpp
 * @brief Implementation of the GNU nano-style full-screen interactive VT100/ANSI text editor.
 */

#include "text_editor.hpp"
#include "../drivers/input/ps2_keyboard.hpp"
#include "../drivers/input/input_manager.hpp"
#include "../drivers/video/terminal_driver.hpp"
#include "../fs/vfs_manager.hpp"
#include "../fs/path_resolver.hpp"
#include "../module/module_manager.hpp"
#include "../proc/scheduler.hpp"
#include "../klibc/klibc.hpp"
#include "../lib/string_utils.hpp"
#include "../lib/printk.hpp"

namespace cortez::shell {

namespace {

struct EditorLine {
    char* data = nullptr;
    usize len  = 0;
    usize cap  = 0;
};

struct UndoSnapshot {
    char* data     = nullptr;
    usize byteLen  = 0;
    usize curRow   = 0;
    usize curCol   = 0;
    bool  modified = false;
};

struct ShortcutPair {
    const char* key;
    const char* label;
};

struct EditorBuffer {
    EditorLine   lines[TextEditor::MAX_LINES]{};
    usize        lineCount = 0;

    usize        curRow    = 0;
    usize        curCol    = 0;
    usize        scrollRow = 0;
    usize        scrollCol = 0;

    bool         modified        = false;
    bool         showLineNumbers = true;
    bool         syntaxHighlight = false;
    bool         autoIndent      = true;

    bool         markActive = false;
    usize        markRow    = 0;
    usize        markCol    = 0;

    bool         highlightMatch = false;
    usize        matchRow       = 0;
    usize        matchCol       = 0;
    usize        matchLen       = 0;

    EditorLine   cutLines[TextEditor::MAX_CUT_LINES]{};
    usize        cutCount      = 0;
    bool         lastKeyWasCut = false;
    bool         cutWasInline  = false;

    UndoSnapshot undoStack[TextEditor::MAX_UNDO_STEPS]{};
    usize        undoCount     = 0;
    usize        undoPos       = 0;
    bool         typingInRun   = false;

    char         cwd[fs::PathResolver::MAX_PATH_LEN]{};
    char         filePath[fs::PathResolver::MAX_PATH_LEN]{};
    char         lastSearch[96]{};
    char         lastReplace[96]{};
    char         statusMsg[128]{};

    mutable u8   syntaxColors[TextEditor::MAX_LINE_CAP]{};
};

// =============================================================================
// Dynamic Line Management
// =============================================================================

void freeLine(EditorLine& line) noexcept {
    if (line.data) {
        delete[] line.data;
        line.data = nullptr;
    }
    line.len = 0;
    line.cap = 0;
}

bool ensureLineCap(EditorLine& line, usize neededCap) noexcept {
    if (neededCap <= line.cap && line.data) return true;
    usize newCap = (line.cap == 0) ? TextEditor::INITIAL_LINE_CAP : line.cap;
    while (newCap < neededCap) {
        newCap *= 2;
    }
    if (newCap > TextEditor::MAX_LINE_CAP) {
        newCap = TextEditor::MAX_LINE_CAP;
    }
    if (neededCap > newCap) return false;

    char* newBuf = new char[newCap];
    if (!newBuf) return false;

    if (line.data && line.len > 0) {
        memcpy(newBuf, line.data, line.len);
    }
    newBuf[line.len] = '\0';
    if (line.data) delete[] line.data;
    line.data = newBuf;
    line.cap  = newCap;
    return true;
}

bool setLineText(EditorLine& line, const char* text, usize textLen) noexcept {
    if (textLen + 1 > TextEditor::MAX_LINE_CAP) {
        textLen = TextEditor::MAX_LINE_CAP - 1;
    }
    if (!ensureLineCap(line, textLen + 1)) return false;
    if (text && textLen > 0) {
        memcpy(line.data, text, textLen);
    }
    line.len           = textLen;
    line.data[textLen] = '\0';
    return true;
}

bool insertSpanInLine(EditorLine& line, usize col, const char* str, usize count) noexcept {
    if (count == 0) return true;
    if (col > line.len) col = line.len;
    if (line.len + count + 1 > TextEditor::MAX_LINE_CAP) {
        if (line.len + 1 >= TextEditor::MAX_LINE_CAP) return false;
        count = (TextEditor::MAX_LINE_CAP - 1) - line.len;
    }
    if (!ensureLineCap(line, line.len + count + 1)) return false;
    memmove(line.data + col + count, line.data + col, line.len - col + 1);
    memcpy(line.data + col, str, count);
    line.len += count;
    return true;
}

bool eraseSpanInLine(EditorLine& line, usize col, usize count) noexcept {
    if (col >= line.len || count == 0) return true;
    if (col + count > line.len) {
        count = line.len - col;
    }
    memmove(line.data + col, line.data + col + count, line.len - (col + count) + 1);
    line.len -= count;
    return true;
}

bool insertLineAt(EditorBuffer* ed, usize row, const char* text, usize textLen) noexcept {
    if (ed->lineCount >= TextEditor::MAX_LINES) return false;
    if (row > ed->lineCount) row = ed->lineCount;

    EditorLine newLine{};
    if (!setLineText(newLine, text, textLen)) return false;

    for (usize r = ed->lineCount; r > row; --r) {
        ed->lines[r] = ed->lines[r - 1];
    }
    ed->lines[row] = newLine;
    ++ed->lineCount;
    return true;
}

void removeLineAt(EditorBuffer* ed, usize row) noexcept {
    if (row >= ed->lineCount) return;
    if (ed->lineCount == 1) {
        setLineText(ed->lines[0], "", 0);
        return;
    }
    freeLine(ed->lines[row]);
    for (usize r = row; r + 1 < ed->lineCount; ++r) {
        ed->lines[r] = ed->lines[r + 1];
    }
    ed->lines[ed->lineCount - 1] = {};
    --ed->lineCount;
}

void clearLines(EditorBuffer* ed) noexcept {
    for (usize i = 0; i < ed->lineCount; ++i) {
        freeLine(ed->lines[i]);
    }
    ed->lineCount = 0;
}

void clearCutBuffer(EditorBuffer* ed) noexcept {
    for (usize i = 0; i < ed->cutCount; ++i) {
        freeLine(ed->cutLines[i]);
    }
    ed->cutCount     = 0;
    ed->cutWasInline = false;
}

void clearUndoStack(EditorBuffer* ed) noexcept {
    for (usize i = 0; i < TextEditor::MAX_UNDO_STEPS; ++i) {
        if (ed->undoStack[i].data) {
            delete[] ed->undoStack[i].data;
            ed->undoStack[i].data = nullptr;
        }
        ed->undoStack[i].byteLen = 0;
    }
    ed->undoCount = 0;
    ed->undoPos   = 0;
}

void destroyEditor(EditorBuffer* ed) noexcept {
    if (!ed) return;
    clearLines(ed);
    clearCutBuffer(ed);
    clearUndoStack(ed);
    delete ed;
}

// =============================================================================
// Syntax Highlighting Detection & Helpers
// =============================================================================

bool hasCSyntaxExtension(const char* path) noexcept {
    if (!path || !path[0]) return false;
    const char* dot = str::findLastChar(path, '.');
    if (!dot) return false;
    return str::compareCaseInsensitive(dot, ".c") == 0 ||
           str::compareCaseInsensitive(dot, ".h") == 0 ||
           str::compareCaseInsensitive(dot, ".cpp") == 0 ||
           str::compareCaseInsensitive(dot, ".hpp") == 0;
}

bool isIdentStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentChar(char c) noexcept {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

bool matchWordList(const char* word, usize len, const char* const* list) noexcept {
    for (usize i = 0; list[i]; ++i) {
        if (str::length(list[i]) == len && str::compareN(word, list[i], len) == 0) {
            return true;
        }
    }
    return false;
}

bool isCKeyword(const char* word, usize len) noexcept {
    static constexpr const char* kKeywords[] = {
        "if", "else", "for", "while", "do", "switch", "case", "default",
        "break", "continue", "return", "goto", "struct", "union", "enum",
        "typedef", "static", "extern", "const", "volatile", "inline",
        "sizeof", "namespace", "class", "public", "private", "protected",
        "virtual", "override", "final", "noexcept", "constexpr", "template",
        "typename", "new", "delete", "using", nullptr
    };
    return matchWordList(word, len, kKeywords);
}

bool isCTypeOrConstant(const char* word, usize len) noexcept {
    static constexpr const char* kTypes[] = {
        "void", "char", "short", "int", "long", "float", "double",
        "signed", "unsigned", "bool", "true", "false", "NULL", "nullptr",
        "size_t", "ssize_t", " int8_t", "int16_t", "int32_t", "int64_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t", "uintptr_t", "intptr_t",
        "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "usize",
        "cortez_pci_dev_t", "cortez_chardev_ops_t", "cortez_cmd_fn",
        "KLOG_INFO", "KLOG_OK", "KLOG_WARN", "KLOG_ERR",
        "O_RDONLY", "O_WRONLY", "O_RDWR", "O_CREAT", "O_TRUNC", "O_APPEND",
        nullptr
    };
    return matchWordList(word, len, kTypes);
}

// =============================================================================
// Serialization, Undo/Redo & File I/O
// =============================================================================

char* serializeBuffer(const EditorBuffer* ed, usize* outLen) noexcept {
    usize totalCap = 16;
    for (usize r = 0; r < ed->lineCount; ++r) {
        totalCap += ed->lines[r].len + 1;
    }

    char* out = new char[totalCap];
    if (!out) {
        if (outLen) *outLen = 0;
        return nullptr;
    }

    usize pos = 0;
    for (usize r = 0; r < ed->lineCount; ++r) {
        usize l = ed->lines[r].len;
        if (l > 0) {
            memcpy(out + pos, ed->lines[r].data, l);
            pos += l;
        }
        out[pos++] = '\n';
    }
    out[pos] = '\0';
    if (outLen) *outLen = pos;
    return out;
}

void deserializeIntoBuffer(EditorBuffer* ed, const char* raw, usize bytesRead) noexcept {
    clearLines(ed);

    if (!raw || bytesRead == 0) {
        insertLineAt(ed, 0, "", 0);
        return;
    }

    usize lineStart = 0;
    for (usize i = 0; i < bytesRead && ed->lineCount < TextEditor::MAX_LINES; ++i) {
        if (raw[i] == '\n') {
            usize segLen = i - lineStart;
            if (segLen > 0 && raw[lineStart + segLen - 1] == '\r') {
                --segLen;
            }
            insertLineAt(ed, ed->lineCount, raw + lineStart, segLen);
            lineStart = i + 1;
        }
    }

    if (lineStart < bytesRead && ed->lineCount < TextEditor::MAX_LINES) {
        usize segLen = bytesRead - lineStart;
        if (segLen > 0 && raw[lineStart + segLen - 1] == '\r') {
            --segLen;
        }
        insertLineAt(ed, ed->lineCount, raw + lineStart, segLen);
    }

    if (ed->lineCount == 0) {
        insertLineAt(ed, 0, "", 0);
    }
}

void recordUndoSnapshot(EditorBuffer* ed) noexcept {
    // Discard any redo states above undoPos
    for (usize i = ed->undoPos; i < ed->undoCount; ++i) {
        if (ed->undoStack[i].data) {
            delete[] ed->undoStack[i].data;
            ed->undoStack[i].data = nullptr;
        }
    }
    ed->undoCount = ed->undoPos;

    usize snapLen = 0;
    char* snapBuf = serializeBuffer(ed, &snapLen);
    if (!snapBuf) return;

    if (ed->undoCount == TextEditor::MAX_UNDO_STEPS) {
        if (ed->undoStack[0].data) {
            delete[] ed->undoStack[0].data;
        }
        for (usize i = 0; i + 1 < TextEditor::MAX_UNDO_STEPS; ++i) {
            ed->undoStack[i] = ed->undoStack[i + 1];
        }
        ed->undoStack[TextEditor::MAX_UNDO_STEPS - 1] = {};
        --ed->undoCount;
    }

    UndoSnapshot& slot = ed->undoStack[ed->undoCount++];
    slot.data          = snapBuf;
    slot.byteLen       = snapLen;
    slot.curRow        = ed->curRow;
    slot.curCol        = ed->curCol;
    slot.modified      = ed->modified;
    ed->undoPos        = ed->undoCount;
}

void performUndo(EditorBuffer* ed) noexcept {
    ed->typingInRun = false;
    if (ed->undoPos == 0) {
        str::copy(ed->statusMsg, "Nothing to undo", sizeof(ed->statusMsg));
        return;
    }

    // If at top of undo stack, save current state as redo target if room permits
    if (ed->undoPos == ed->undoCount && ed->undoCount < TextEditor::MAX_UNDO_STEPS) {
        usize curLen = 0;
        char* curBuf = serializeBuffer(ed, &curLen);
        if (curBuf) {
            UndoSnapshot& redoSlot = ed->undoStack[ed->undoCount++];
            redoSlot.data          = curBuf;
            redoSlot.byteLen       = curLen;
            redoSlot.curRow        = ed->curRow;
            redoSlot.curCol        = ed->curCol;
            redoSlot.modified      = ed->modified;
            ed->undoPos            = ed->undoCount - 1;
        }
    }

    --ed->undoPos;
    const UndoSnapshot& snap = ed->undoStack[ed->undoPos];
    deserializeIntoBuffer(ed, snap.data, snap.byteLen);
    ed->curRow   = (snap.curRow < ed->lineCount) ? snap.curRow : (ed->lineCount - 1);
    ed->curCol   = (snap.curCol <= ed->lines[ed->curRow].len) ? snap.curCol : ed->lines[ed->curRow].len;
    ed->modified = snap.modified;
    str::copy(ed->statusMsg, "Undid action", sizeof(ed->statusMsg));
}

void performRedo(EditorBuffer* ed) noexcept {
    ed->typingInRun = false;
    if (ed->undoPos + 1 >= ed->undoCount) {
        str::copy(ed->statusMsg, "Nothing to redo", sizeof(ed->statusMsg));
        return;
    }

    ++ed->undoPos;
    const UndoSnapshot& snap = ed->undoStack[ed->undoPos];
    deserializeIntoBuffer(ed, snap.data, snap.byteLen);
    ed->curRow   = (snap.curRow < ed->lineCount) ? snap.curRow : (ed->lineCount - 1);
    ed->curCol   = (snap.curCol <= ed->lines[ed->curRow].len) ? snap.curCol : ed->lines[ed->curRow].len;
    ed->modified = snap.modified;
    str::copy(ed->statusMsg, "Redid action", sizeof(ed->statusMsg));
}

void loadIntoBuffer(EditorBuffer* ed, const char* cwd, const char* filePath) noexcept {
    str::copy(ed->cwd, (cwd && cwd[0]) ? cwd : "/", sizeof(ed->cwd));
    str::copy(ed->filePath, filePath ? filePath : "", sizeof(ed->filePath));

    clearLines(ed);
    insertLineAt(ed, 0, "", 0);
    ed->curRow          = 0;
    ed->curCol          = 0;
    ed->scrollRow       = 0;
    ed->scrollCol       = 0;
    ed->modified        = false;
    ed->markActive      = false;
    ed->highlightMatch  = false;
    ed->syntaxHighlight = hasCSyntaxExtension(ed->filePath);

    if (!ed->filePath[0]) {
        str::copy(ed->statusMsg, "New Buffer", sizeof(ed->statusMsg));
        return;
    }

    auto& vfs = fs::VfsManager::instance();
    fs::FileStat st{};
    if (vfs.stat(ed->cwd, ed->filePath, &st) != Status::Ok) {
        str::copy(ed->statusMsg, "New File", sizeof(ed->statusMsg));
        return;
    }

    if (st.type == fs::NodeType::Directory) {
        klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg), "\"%s\" is a directory", ed->filePath);
        return;
    }

    if (st.size == 0) {
        str::copy(ed->statusMsg, "Read 0 lines (empty file)", sizeof(ed->statusMsg));
        return;
    }

    auto fdRes = vfs.open(ed->cwd, ed->filePath, fs::O_RDONLY);
    if (fdRes.isError()) {
        klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg), "Error opening %s", ed->filePath);
        return;
    }

    i32 fd = fdRes.value();
    usize fileSize = static_cast<usize>(st.size);
    char* raw = new char[fileSize + 1];
    if (!raw) {
        vfs.close(fd);
        str::copy(ed->statusMsg, "Out of memory reading file", sizeof(ed->statusMsg));
        return;
    }

    auto rRes = vfs.read(fd, raw, fileSize);
    vfs.close(fd);

    usize bytesRead = rRes.isOk() ? rRes.value() : 0;
    raw[bytesRead]  = '\0';

    deserializeIntoBuffer(ed, raw, bytesRead);
    delete[] raw;

    klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                   "Read %zu line%s (%zu B)",
                   ed->lineCount, (ed->lineCount == 1) ? "" : "s", bytesRead);
}

Status saveBuffer(EditorBuffer* ed, const char* targetPath) noexcept {
    if (!targetPath || !targetPath[0]) {
        str::copy(ed->statusMsg, "Cancelled: No file name", sizeof(ed->statusMsg));
        return Status::InvalidArgument;
    }

    usize byteCount = 0;
    char* out       = serializeBuffer(ed, &byteCount);
    if (!out) {
        str::copy(ed->statusMsg, "Save failed: Out of memory", sizeof(ed->statusMsg));
        return Status::OutOfMemory;
    }

    auto& vfs = fs::VfsManager::instance();
    Status st = vfs.writeFileAll(ed->cwd, targetPath, out, byteCount);
    delete[] out;

    if (st == Status::Ok) {
        (void)vfs.sync();
        str::copy(ed->filePath, targetPath, sizeof(ed->filePath));
        ed->syntaxHighlight = hasCSyntaxExtension(ed->filePath);
        ed->modified        = false;
        klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                       "Wrote %zu line%s (%zu B) to %s",
                       ed->lineCount, (ed->lineCount == 1) ? "" : "s", byteCount, ed->filePath);
    } else {
        klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                       "Error writing %s: %s", targetPath, statusToString(st));
    }
    return st;
}

Status insertFileIntoBuffer(EditorBuffer* ed, const char* inputPath) noexcept {
    if (!inputPath || !inputPath[0]) return Status::InvalidArgument;

    auto& vfs = fs::VfsManager::instance();
    fs::FileStat st{};
    Status statSt = vfs.stat(ed->cwd, inputPath, &st);
    if (statSt != Status::Ok || st.type != fs::NodeType::File) {
        klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                       "Cannot read \"%s\": %s", inputPath,
                       (statSt != Status::Ok) ? statusToString(statSt) : "Is a directory");
        return Status::NotFound;
    }

    auto fdRes = vfs.open(ed->cwd, inputPath, fs::O_RDONLY);
    if (fdRes.isError()) return fdRes.status();

    i32 fd = fdRes.value();
    usize fileSize = static_cast<usize>(st.size);
    char* raw = new char[fileSize + 1];
    if (!raw) {
        vfs.close(fd);
        return Status::OutOfMemory;
    }

    auto rRes = vfs.read(fd, raw, fileSize);
    vfs.close(fd);
    usize bytesRead = rRes.isOk() ? rRes.value() : 0;
    raw[bytesRead]  = '\0';

    recordUndoSnapshot(ed);

    usize insertedLines = 0;
    usize lineStart     = 0;
    usize insertRow     = ed->curRow;
    for (usize i = 0; i < bytesRead && ed->lineCount < TextEditor::MAX_LINES; ++i) {
        if (raw[i] == '\n') {
            usize segLen = i - lineStart;
            if (segLen > 0 && raw[lineStart + segLen - 1] == '\r') --segLen;
            insertLineAt(ed, insertRow++, raw + lineStart, segLen);
            ++insertedLines;
            lineStart = i + 1;
        }
    }
    if (lineStart < bytesRead && ed->lineCount < TextEditor::MAX_LINES) {
        usize segLen = bytesRead - lineStart;
        if (segLen > 0 && raw[lineStart + segLen - 1] == '\r') --segLen;
        insertLineAt(ed, insertRow++, raw + lineStart, segLen);
        ++insertedLines;
    }

    delete[] raw;
    ed->modified = true;
    klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                   "Inserted %zu line%s from %s",
                   insertedLines, (insertedLines == 1) ? "" : "s", inputPath);
    return Status::Ok;
}

// =============================================================================
// Region Normalization & Cut / Copy / Paste
// =============================================================================

void getNormalizedMark(const EditorBuffer* ed,
                       usize& startRow, usize& startCol,
                       usize& endRow, usize& endCol) noexcept {
    if (ed->markRow < ed->curRow || (ed->markRow == ed->curRow && ed->markCol <= ed->curCol)) {
        startRow = ed->markRow;
        startCol = ed->markCol;
        endRow   = ed->curRow;
        endCol   = ed->curCol;
    } else {
        startRow = ed->curRow;
        startCol = ed->curCol;
        endRow   = ed->markRow;
        endCol   = ed->markCol;
    }
}

bool isCellMarked(const EditorBuffer* ed, usize row, usize col) noexcept {
    if (!ed->markActive) return false;
    usize sr = 0, sc = 0, er = 0, ec = 0;
    getNormalizedMark(ed, sr, sc, er, ec);
    if (row < sr || row > er) return false;
    if (sr == er) return col >= sc && col < ec;
    if (row == sr) return col >= sc;
    if (row == er) return col < ec;
    return true;
}

bool isCellSearchMatch(const EditorBuffer* ed, usize row, usize col) noexcept {
    if (!ed->highlightMatch || row != ed->matchRow) return false;
    return col >= ed->matchCol && col < (ed->matchCol + ed->matchLen);
}

void copyOrCutRegion(EditorBuffer* ed, bool doDelete) noexcept {
    usize sr = 0, sc = 0, er = 0, ec = 0;
    getNormalizedMark(ed, sr, sc, er, ec);
    if (sr >= ed->lineCount) return;
    if (er >= ed->lineCount) er = ed->lineCount - 1;
    if (sc > ed->lines[sr].len) sc = ed->lines[sr].len;
    if (ec > ed->lines[er].len) ec = ed->lines[er].len;

    clearCutBuffer(ed);

    if (sr == er) {
        usize span = (ec > sc) ? (ec - sc) : 0;
        setLineText(ed->cutLines[0], ed->lines[sr].data + sc, span);
        ed->cutCount     = 1;
        ed->cutWasInline = true;
        if (doDelete && span > 0) {
            recordUndoSnapshot(ed);
            eraseSpanInLine(ed->lines[sr], sc, span);
            ed->curRow   = sr;
            ed->curCol   = sc;
            ed->modified = true;
        }
    } else {
        if (doDelete) recordUndoSnapshot(ed);
        // First partial line
        setLineText(ed->cutLines[ed->cutCount++],
                    ed->lines[sr].data + sc,
                    ed->lines[sr].len - sc);
        // Middle full lines
        for (usize r = sr + 1; r < er && ed->cutCount < TextEditor::MAX_CUT_LINES; ++r) {
            setLineText(ed->cutLines[ed->cutCount++], ed->lines[r].data, ed->lines[r].len);
        }
        // Last partial line
        if (ed->cutCount < TextEditor::MAX_CUT_LINES) {
            setLineText(ed->cutLines[ed->cutCount++], ed->lines[er].data, ec);
        }
        ed->cutWasInline = false;

        if (doDelete) {
            // Merge tail of endRow onto head of startRow
            const char* tailStr = ed->lines[er].data + ec;
            usize       tailLen = ed->lines[er].len - ec;
            ed->lines[sr].len = sc;
            ed->lines[sr].data[sc] = '\0';
            insertSpanInLine(ed->lines[sr], sc, tailStr, tailLen);

            for (usize r = er; r > sr; --r) {
                removeLineAt(ed, r);
            }
            ed->curRow   = sr;
            ed->curCol   = sc;
            ed->modified = true;
        }
    }

    ed->markActive = false;
    klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                   "%s %zu line%s",
                   doDelete ? "Cut" : "Copied",
                   ed->cutCount, (ed->cutCount == 1) ? "" : "s");
}

void handleCutLineOrRegion(EditorBuffer* ed) noexcept {
    ed->typingInRun = false;
    if (ed->markActive) {
        copyOrCutRegion(ed, true);
        ed->lastKeyWasCut = false;
        return;
    }

    recordUndoSnapshot(ed);
    if (!ed->lastKeyWasCut) {
        clearCutBuffer(ed);
    }

    if (ed->cutCount < TextEditor::MAX_CUT_LINES) {
        setLineText(ed->cutLines[ed->cutCount++],
                    ed->lines[ed->curRow].data,
                    ed->lines[ed->curRow].len);
    }
    ed->cutWasInline = false;

    if (ed->lineCount == 1) {
        setLineText(ed->lines[0], "", 0);
        ed->curCol = 0;
    } else {
        removeLineAt(ed, ed->curRow);
        if (ed->curRow >= ed->lineCount) {
            ed->curRow = ed->lineCount - 1;
        }
    }

    ed->modified      = true;
    ed->lastKeyWasCut = true;
    klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                   "Cut %zu line%s into cutbuffer",
                   ed->cutCount, (ed->cutCount == 1) ? "" : "s");
}

void handleCopyLineOrRegion(EditorBuffer* ed) noexcept {
    ed->typingInRun = false;
    if (ed->markActive) {
        copyOrCutRegion(ed, false);
        ed->lastKeyWasCut = false;
        return;
    }

    if (!ed->lastKeyWasCut) {
        clearCutBuffer(ed);
    }
    if (ed->cutCount < TextEditor::MAX_CUT_LINES) {
        setLineText(ed->cutLines[ed->cutCount++],
                    ed->lines[ed->curRow].data,
                    ed->lines[ed->curRow].len);
    }
    ed->cutWasInline  = false;
    ed->lastKeyWasCut = true;
    klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                   "Copied %zu line%s to cutbuffer",
                   ed->cutCount, (ed->cutCount == 1) ? "" : "s");
}

void handlePasteCutBuffer(EditorBuffer* ed) noexcept {
    ed->typingInRun   = false;
    ed->lastKeyWasCut = false;
    if (ed->cutCount == 0) {
        str::copy(ed->statusMsg, "Cutbuffer is empty", sizeof(ed->statusMsg));
        return;
    }

    recordUndoSnapshot(ed);

    if (ed->cutWasInline && ed->cutCount == 1) {
        insertSpanInLine(ed->lines[ed->curRow], ed->curCol,
                         ed->cutLines[0].data, ed->cutLines[0].len);
        ed->curCol += ed->cutLines[0].len;
        ed->modified = true;
        str::copy(ed->statusMsg, "Pasted text", sizeof(ed->statusMsg));
        return;
    }

    usize pasted = 0;
    for (usize i = 0; i < ed->cutCount && ed->lineCount < TextEditor::MAX_LINES; ++i) {
        insertLineAt(ed, ed->curRow + i, ed->cutLines[i].data, ed->cutLines[i].len);
        ++pasted;
    }
    ed->curRow  += pasted;
    if (ed->curRow >= ed->lineCount) ed->curRow = ed->lineCount - 1;
    ed->curCol   = 0;
    ed->modified = true;
    klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                   "Pasted %zu line%s", pasted, (pasted == 1) ? "" : "s");
}

// =============================================================================
// Search & Replace Helpers
// =============================================================================

const char* findSubstringCaseInsensitive(const char* haystack, const char* needle) noexcept {
    if (!haystack || !needle || !needle[0]) return nullptr;
    usize nlen = str::length(needle);
    for (const char* p = haystack; *p; ++p) {
        if (str::toLower(*p) == str::toLower(needle[0])) {
            bool match = true;
            for (usize i = 1; i < nlen; ++i) {
                if (!p[i] || str::toLower(p[i]) != str::toLower(needle[i])) {
                    match = false;
                    break;
                }
            }
            if (match) return p;
        }
    }
    return nullptr;
}

bool findNextOccurrence(EditorBuffer* ed,
                        const char* query,
                        usize startRow,
                        usize startCol,
                        bool allowWrap,
                        usize* outRow,
                        usize* outCol,
                        bool* outWrapped) noexcept {
    if (!query || !query[0] || ed->lineCount == 0) return false;
    if (outWrapped) *outWrapped = false;

    // 1. Search from (startRow, startCol) to end of document
    for (usize r = startRow; r < ed->lineCount; ++r) {
        const EditorLine& line = ed->lines[r];
        usize colOffset = (r == startRow) ? startCol : 0;
        if (colOffset > line.len) continue;
        const char* found = findSubstringCaseInsensitive(line.data + colOffset, query);
        if (found) {
            *outRow = r;
            *outCol = static_cast<usize>(found - line.data);
            return true;
        }
    }

    if (!allowWrap) return false;

    // 2. Wrap around from (0, 0) up to startRow
    for (usize r = 0; r <= startRow && r < ed->lineCount; ++r) {
        const EditorLine& line = ed->lines[r];
        const char* found = findSubstringCaseInsensitive(line.data, query);
        if (found) {
            usize c = static_cast<usize>(found - line.data);
            if (r < startRow || c < startCol) {
                if (outWrapped) *outWrapped = true;
                *outRow = r;
                *outCol = c;
                return true;
            }
        }
    }
    return false;
}

// =============================================================================
// Viewport & GNU nano 4-Zone Renderer
// =============================================================================

u32 getGutterCols(const EditorBuffer* ed) noexcept {
    return ed->showLineNumbers ? 6U : 1U;
}

void ensureVisible(EditorBuffer* ed) noexcept {
    auto& term   = drivers::video::TerminalDriver::instance();
    u32 termRows = term.getRows();
    u32 termCols = term.getCols();
    u32 viewRows = (termRows > 4) ? (termRows - 4) : 1;
    u32 gutter   = getGutterCols(ed);
    u32 textCols = (termCols > gutter + 1) ? (termCols - gutter - 1) : 1;

    if (ed->curRow >= ed->lineCount) {
        ed->curRow = (ed->lineCount > 0) ? (ed->lineCount - 1) : 0;
    }

    usize lineLen = ed->lines[ed->curRow].len;
    if (ed->curCol > lineLen) {
        ed->curCol = lineLen;
    }

    if (ed->curRow < ed->scrollRow) {
        ed->scrollRow = ed->curRow;
    } else if (ed->curRow >= ed->scrollRow + viewRows) {
        ed->scrollRow = ed->curRow - viewRows + 1;
    }

    if (ed->curCol < ed->scrollCol) {
        ed->scrollCol = ed->curCol;
    } else if (ed->curCol >= ed->scrollCol + textCols) {
        ed->scrollCol = ed->curCol - textCols + 1;
    }
}

void computeLineSyntaxColors(const EditorLine& line, bool enableSyntax, u8* outColors, usize maxLen) noexcept {
    usize n = (line.len < maxLen) ? line.len : maxLen;
    for (usize i = 0; i < n; ++i) outColors[i] = 37; // Default white
    if (!enableSyntax || !line.data || n == 0) return;

    usize i = 0;
    while (i < n && (line.data[i] == ' ' || line.data[i] == '\t')) ++i;
    if (i < n && line.data[i] == '#') {
        for (usize j = i; j < n; ++j) outColors[j] = 95; // Bright Magenta preprocessor
        return;
    }

    while (i < n) {
        char c = line.data[i];

        // Single-line or inline block comment
        if (c == '/' && i + 1 < n && line.data[i + 1] == '/') {
            for (usize j = i; j < n; ++j) outColors[j] = 90; // Bright Grey comment
            break;
        }
        if (c == '/' && i + 1 < n && line.data[i + 1] == '*') {
            usize j = i;
            outColors[j++] = 90;
            outColors[j++] = 90;
            while (j < n) {
                outColors[j] = 90;
                if (line.data[j] == '*' && j + 1 < n && line.data[j + 1] == '/') {
                    outColors[j + 1] = 90;
                    j += 2;
                    break;
                }
                ++j;
            }
            i = j;
            continue;
        }

        // String or char literal
        if (c == '"' || c == '\'') {
            char quote = c;
            outColors[i++] = 92; // Bright Green string
            while (i < n) {
                outColors[i] = 92;
                if (line.data[i] == '\\' && i + 1 < n) {
                    outColors[i + 1] = 92;
                    i += 2;
                    continue;
                }
                if (line.data[i] == quote) {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }

        // Identifier / Keyword / Type
        if (isIdentStart(c)) {
            usize start = i;
            while (i < n && isIdentChar(line.data[i])) ++i;
            usize wlen = i - start;
            u8 color = 37;
            if (isCKeyword(line.data + start, wlen)) {
                color = 93; // Bright Yellow keyword
            } else if (isCTypeOrConstant(line.data + start, wlen)) {
                color = 96; // Bright Cyan type/constant
            }
            for (usize j = start; j < i; ++j) outColors[j] = color;
            continue;
        }

        // Numeric literal
        if (c >= '0' && c <= '9') {
            while (i < n && (isIdentChar(line.data[i]) || line.data[i] == '.')) {
                outColors[i++] = 91; // Bright Red/Orange number
            }
            continue;
        }

        ++i;
    }
}

constexpr u32 ansiCodeToRgb(u8 code) noexcept {
    switch (code) {
        case 90: return drivers::video::TerminalDriver::COLOR_BRIGHT_GREY;
        case 91: return drivers::video::TerminalDriver::COLOR_BRIGHT_RED;
        case 92: return drivers::video::TerminalDriver::COLOR_BRIGHT_GREEN;
        case 93: return drivers::video::TerminalDriver::COLOR_BRIGHT_YELLOW;
        case 94: return drivers::video::TerminalDriver::COLOR_BRIGHT_BLUE;
        case 95: return drivers::video::TerminalDriver::COLOR_BRIGHT_MAG;
        case 96: return drivers::video::TerminalDriver::COLOR_BRIGHT_CYAN;
        case 97: return drivers::video::TerminalDriver::COLOR_BRIGHT_WHITE;
        case 31: return drivers::video::TerminalDriver::COLOR_RED;
        case 32: return drivers::video::TerminalDriver::COLOR_GREEN;
        case 33: return drivers::video::TerminalDriver::COLOR_YELLOW;
        case 34: return drivers::video::TerminalDriver::COLOR_BLUE;
        case 35: return drivers::video::TerminalDriver::COLOR_MAGENTA;
        case 36: return drivers::video::TerminalDriver::COLOR_CYAN;
        case 37:
        default: return drivers::video::TerminalDriver::DEFAULT_FG_COLOR;
    }
}

void renderShortcutRow(u32 termCols, u32 row, const ShortcutPair* pairs, usize pairCount) noexcept {
    auto& term = drivers::video::TerminalDriver::instance();
    u32 colWidth = (pairCount > 0) ? (termCols / static_cast<u32>(pairCount)) : termCols;
    if (colWidth < 10) colWidth = 10;

    u32 curCol = 0;
    for (usize i = 0; i < pairCount && curCol + 3 < termCols; ++i) {
        // Shortcut key (inverted: COLOR_BAR_FG on COLOR_BAR_BG)
        const char* key = pairs[i].key;
        usize keyLen    = str::length(key);
        term.writeStringAt(curCol, row, key, keyLen,
                           drivers::video::TerminalDriver::COLOR_BAR_FG,
                           drivers::video::TerminalDriver::COLOR_BAR_BG);
        curCol += static_cast<u32>(keyLen);

        // Gap after key
        if (curCol < termCols) {
            term.putCellAt(curCol++, row, ' ',
                           drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                           drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
        }

        // Slot target end column
        u32 slotEnd = static_cast<u32>((i + 1) * colWidth);
        if (i + 1 == pairCount || slotEnd > termCols) {
            slotEnd = termCols;
        }

        // Label (normal: DEFAULT_FG_COLOR on DEFAULT_BG_COLOR)
        const char* lbl = pairs[i].label;
        usize lblLen    = str::length(lbl);
        u32 maxLbl      = (slotEnd > curCol) ? (slotEnd - curCol) : 0;
        usize printLen  = (lblLen < maxLbl) ? lblLen : maxLbl;
        if (printLen > 0) {
            term.writeStringAt(curCol, row, lbl, printLen,
                               drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                               drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
            curCol += static_cast<u32>(printLen);
        }

        // Fill rest of slot with spaces
        if (slotEnd > curCol) {
            term.fillRowAt(curCol, row, slotEnd - curCol, ' ',
                           drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                           drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
            curCol = slotEnd;
        }
    }

    // Fill remainder of row to right edge
    if (curCol < termCols) {
        term.fillRowAt(curCol, row, termCols - curCol, ' ',
                       drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                       drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
    }
}

void renderScreen(const EditorBuffer* ed,
                  const char* activePromptLabel = nullptr,
                  const char* activePromptValue = nullptr,
                  usize       promptCursor      = 0,
                  const ShortcutPair* customRow1 = nullptr,
                  usize               customRow1Count = 0,
                  const ShortcutPair* customRow2 = nullptr,
                  usize               customRow2Count = 0) noexcept {
    auto& term   = drivers::video::TerminalDriver::instance();
    u32 termCols = term.getCols();
    u32 termRows = term.getRows();
    if (termRows < 6 || termCols < 20) return;

    u32 viewRows = termRows - 4; // 1 title bar + (termRows-4) body + 1 status/prompt + 2 shortcut bars
    u32 gutter   = getGutterCols(ed);
    u32 textCols = (termCols > gutter + 1) ? (termCols - gutter - 1) : 1;

    term.setCursorVisible(false);

    // 1. Top Inverted GNU nano Title Bar (Row 0)
    const char* leftTitle  = "  Cortez nano 1.0";
    const char* centerName = ed->filePath[0] ? ed->filePath : "New Buffer";
    const char* rightState = ed->modified
                                 ? (ed->markActive ? "[Mark] Modified  " : "Modified  ")
                                 : (ed->markActive ? "[Mark]           " : "          ");

    term.fillRowAt(0, 0, termCols, ' ',
                   drivers::video::TerminalDriver::COLOR_BAR_FG,
                   drivers::video::TerminalDriver::COLOR_BAR_BG);

    usize leftLen = str::length(leftTitle);
    term.writeStringAt(0, 0, leftTitle, leftLen,
                       drivers::video::TerminalDriver::COLOR_BAR_FG,
                       drivers::video::TerminalDriver::COLOR_BAR_BG);

    usize centerLen = str::length(centerName);
    u32 centerPos = (termCols > centerLen) ? static_cast<u32>((termCols - centerLen) / 2) : static_cast<u32>(leftLen + 2);
    if (centerPos < termCols) {
        term.writeStringAt(centerPos, 0, centerName, centerLen,
                           drivers::video::TerminalDriver::COLOR_BAR_FG,
                           drivers::video::TerminalDriver::COLOR_BAR_BG);
    }

    usize rightLen = str::length(rightState);
    if (termCols > rightLen) {
        u32 rightPos = termCols - static_cast<u32>(rightLen);
        term.writeStringAt(rightPos, 0, rightState, rightLen,
                           drivers::video::TerminalDriver::COLOR_BAR_FG,
                           drivers::video::TerminalDriver::COLOR_BAR_BG);
    }

    // 2. Document Viewport Rows (Rows 1 .. viewRows)
    for (u32 vr = 0; vr < viewRows; ++vr) {
        u32 row = 1 + vr;
        usize docRow = ed->scrollRow + vr;
        if (docRow < ed->lineCount) {
            const EditorLine& line = ed->lines[docRow];

            // Gutter
            char leftOverflow = (ed->scrollCol > 0 && line.len > 0) ? '<' : ' ';
            if (ed->showLineNumbers) {
                char gbuf[16]{};
                klibc_snprintf(gbuf, sizeof(gbuf), "%4zu%c ", docRow + 1, leftOverflow);
                term.writeStringAt(0, row, gbuf, 6,
                                   drivers::video::TerminalDriver::COLOR_BRIGHT_GREY,
                                   drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
            } else {
                term.putCellAt(0, row, leftOverflow,
                               drivers::video::TerminalDriver::COLOR_BRIGHT_GREY,
                               drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
            }

            computeLineSyntaxColors(line, ed->syntaxHighlight, ed->syntaxColors, line.len);

            usize startCol = ed->scrollCol;
            usize availLen = (line.len > startCol) ? (line.len - startCol) : 0;
            bool  hasRightOverflow = (availLen > textCols);
            usize showLen  = hasRightOverflow ? textCols : availLen;

            for (usize i = 0; i < showLen; ++i) {
                usize docCol = startCol + i;
                u8 syntaxCode = (docCol < line.len) ? ed->syntaxColors[docCol] : 37;
                u32 fg = ansiCodeToRgb(syntaxCode);
                u32 bg = drivers::video::TerminalDriver::DEFAULT_BG_COLOR;

                if (isCellSearchMatch(ed, docRow, docCol)) {
                    fg = drivers::video::TerminalDriver::DEFAULT_BG_COLOR;
                    bg = drivers::video::TerminalDriver::COLOR_BRIGHT_YELLOW;
                } else if (isCellMarked(ed, docRow, docCol)) {
                    fg = drivers::video::TerminalDriver::COLOR_BAR_FG;
                    bg = drivers::video::TerminalDriver::COLOR_BAR_BG;
                }

                term.putCellAt(gutter + static_cast<u32>(i), row, line.data[docCol], fg, bg);
            }

            // Fill empty columns after line text up to right margin
            u32 textEnd = gutter + static_cast<u32>(showLen);
            u32 rightEdge = gutter + textCols;
            if (rightEdge > textEnd) {
                term.fillRowAt(textEnd, row, rightEdge - textEnd, ' ',
                               drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                               drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
            }

            // Right overflow marker '>' or blank
            char rightOverflow = hasRightOverflow ? '>' : ' ';
            term.putCellAt(termCols - 1, row, rightOverflow,
                           drivers::video::TerminalDriver::COLOR_BRIGHT_GREY,
                           drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
        } else {
            // Tilde row beyond end of document
            if (ed->showLineNumbers) {
                term.writeStringAt(0, row, "   ~  ", 6,
                                   drivers::video::TerminalDriver::COLOR_BRIGHT_GREY,
                                   drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
                if (termCols > 6) {
                    term.fillRowAt(6, row, termCols - 6, ' ',
                                   drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                                   drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
                }
            } else {
                term.putCellAt(0, row, '~',
                               drivers::video::TerminalDriver::COLOR_BRIGHT_GREY,
                               drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
                if (termCols > 1) {
                    term.fillRowAt(1, row, termCols - 1, ' ',
                                   drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                                   drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
                }
            }
        }
    }

    // 3. Status Bar or Interactive Prompt Bar (Row termRows - 3)
    u32 statusRow = termRows - 3;
    u32 promptScreenCol = 1;
    if (activePromptLabel) {
        term.fillRowAt(0, statusRow, termCols, ' ',
                       drivers::video::TerminalDriver::COLOR_BAR_FG,
                       drivers::video::TerminalDriver::COLOR_BAR_BG);
        char promptBuf[256]{};
        klibc_snprintf(promptBuf, sizeof(promptBuf), " %s%s",
                       activePromptLabel,
                       activePromptValue ? activePromptValue : "");
        usize plen = str::length(promptBuf);
        term.writeStringAt(0, statusRow, promptBuf, plen,
                           drivers::video::TerminalDriver::COLOR_BAR_FG,
                           drivers::video::TerminalDriver::COLOR_BAR_BG);
        promptScreenCol = static_cast<u32>(1 + str::length(activePromptLabel) + promptCursor);
        if (promptScreenCol >= termCols) promptScreenCol = termCols - 1;
    } else if (ed->statusMsg[0] != '\0') {
        term.fillRowAt(0, statusRow, termCols, ' ',
                       drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                       drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
        char formattedStatus[144]{};
        klibc_snprintf(formattedStatus, sizeof(formattedStatus), "[ %s ]", ed->statusMsg);
        usize slen = str::length(formattedStatus);
        u32 padLeft = (termCols > slen) ? static_cast<u32>((termCols - slen) / 2) : 0;
        term.writeStringAt(padLeft, statusRow, formattedStatus, slen,
                           drivers::video::TerminalDriver::COLOR_BAR_FG,
                           drivers::video::TerminalDriver::COLOR_BAR_BG);
    } else {
        term.fillRowAt(0, statusRow, termCols, ' ',
                       drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                       drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
    }

    // 4. Bottom 2-Row GNU nano Shortcut Grid (Rows termRows - 2 and termRows - 1)
    static constexpr ShortcutPair kDefaultRow1[] = {
        { "^G", "Get Help"   },
        { "^O", "Write Out"  },
        { "^W", "Where Is"   },
        { "^K", "Cut Text"   },
        { "^T", "Check CC"   },
        { "^C", "Cur Pos"    }
    };
    static constexpr ShortcutPair kDefaultRow2[] = {
        { "^X",  "Exit"       },
        { "^R",  "Read File"  },
        { "^\\", "Replace"    },
        { "^U",  "Paste Text" },
        { "M-U", "Undo"       },
        { "^_",  "Go To Line" }
    };

    const ShortcutPair* r1 = customRow1 ? customRow1 : kDefaultRow1;
    usize r1Count          = customRow1 ? customRow1Count : 6;
    const ShortcutPair* r2 = customRow2 ? customRow2 : kDefaultRow2;
    usize r2Count          = customRow2 ? customRow2Count : 6;

    renderShortcutRow(termCols, termRows - 2, r1, r1Count);
    renderShortcutRow(termCols, termRows - 1, r2, r2Count);

    // 5. Position hardware cursor inside viewport or prompt bar
    if (activePromptLabel) {
        term.setCursorPos(promptScreenCol, statusRow);
        term.setCursorVisible(true);
    } else {
        u32 screenRow = static_cast<u32>((ed->curRow - ed->scrollRow) + 1);
        u32 screenCol = static_cast<u32>((ed->curCol - ed->scrollCol) + gutter);
        if (screenRow < 1) screenRow = 1;
        if (screenRow > viewRows) screenRow = viewRows;
        if (screenCol >= termCols) screenCol = termCols - 1;
        term.setCursorPos(screenCol, screenRow);
        term.setCursorVisible(true);
    }
}

// =============================================================================
// Keyboard Reading & Interactive Prompts
// =============================================================================

char readKeyBlocking() noexcept {
    auto& cin = drivers::input::InputManager::instance();
    if (!cin.hasData()) {
        cin.pollHardware();
        if (!cin.hasData()) {
            proc::Scheduler::instance().yield();
        }
    }
    return cin.readCharBlocking();
}

bool promptLineInput(EditorBuffer* ed,
                     const char* promptLabel,
                     char* ioBuf,
                     usize maxLen) noexcept {
    static constexpr ShortcutPair kPromptRow1[] = {
        { "Enter", "Accept" },
        { "^C",    "Cancel" },
        { "^A",    "Home"   },
        { "^E",    "End"    }
    };
    static constexpr ShortcutPair kPromptRow2[] = {
        { "Bksp",  "Delete Char" },
        { "^U",    "Clear Input" },
        { "Left",  "Move Left"   },
        { "Right", "Move Right"  }
    };

    usize len    = str::length(ioBuf);
    usize cursor = len;

    enum class EscState : u8 { None, Esc, Bracket, TildeParam };
    EscState escState = EscState::None;
    u32      escParam = 0;

    for (;;) {
        renderScreen(ed, promptLabel, ioBuf, cursor, kPromptRow1, 4, kPromptRow2, 4);
        char ch = readKeyBlocking();

        if (escState == EscState::Esc) {
            if (ch == '[' || ch == 'O') {
                escState = EscState::Bracket;
                escParam = 0;
                continue;
            }
            // Esc followed by non-bracket cancels prompt
            str::copy(ed->statusMsg, "Cancelled", sizeof(ed->statusMsg));
            return false;
        }

        if (escState == EscState::Bracket || escState == EscState::TildeParam) {
            if (ch >= '0' && ch <= '9') {
                escParam = escParam * 10 + static_cast<u32>(ch - '0');
                escState = EscState::TildeParam;
                continue;
            }
            escState = EscState::None;
            if (ch == 'D') {
                if (cursor > 0) --cursor;
            } else if (ch == 'C') {
                if (cursor < len) ++cursor;
            } else if (ch == 'H' || (ch == '~' && (escParam == 1 || escParam == 7))) {
                cursor = 0;
            } else if (ch == 'F' || (ch == '~' && (escParam == 4 || escParam == 8))) {
                cursor = len;
            } else if (ch == '~' && escParam == 3) {
                if (cursor < len) {
                    memmove(ioBuf + cursor, ioBuf + cursor + 1, len - cursor);
                    --len;
                }
            }
            continue;
        }

        if (ch == '\033') {
            escState = EscState::Esc;
            continue;
        }
        if (ch == '\x03' || ch == '\x18') { // ^C or ^X cancels prompt
            str::copy(ed->statusMsg, "Cancelled", sizeof(ed->statusMsg));
            return false;
        }
        if (ch == '\r' || ch == '\n') {
            ioBuf[len] = '\0';
            return true;
        }
        if (ch == '\x01') { // ^A
            cursor = 0;
            continue;
        }
        if (ch == '\x05') { // ^E
            cursor = len;
            continue;
        }
        if (ch == '\x15') { // ^U: clear prompt line
            ioBuf[0] = '\0';
            len      = 0;
            cursor   = 0;
            continue;
        }
        if (ch == '\b' || ch == 127) {
            if (cursor > 0) {
                memmove(ioBuf + cursor - 1, ioBuf + cursor, len - cursor + 1);
                --cursor;
                --len;
            }
            continue;
        }
        if (static_cast<u8>(ch) >= 0x20U && len + 1 < maxLen) {
            memmove(ioBuf + cursor + 1, ioBuf + cursor, len - cursor + 1);
            ioBuf[cursor] = ch;
            ++cursor;
            ++len;
        }
    }
}

// Returns: 'y' = Yes, 'n' = No, 'c' = Cancel
char promptYesNoCancel(EditorBuffer* ed, const char* question) noexcept {
    static constexpr ShortcutPair kYnRow1[] = {
        { "Y", "Yes" },
        { "N", "No"  }
    };
    static constexpr ShortcutPair kYnRow2[] = {
        { "^C", "Cancel" }
    };

    for (;;) {
        renderScreen(ed, question, "", 0, kYnRow1, 2, kYnRow2, 1);
        char ch = readKeyBlocking();
        if (ch == 'y' || ch == 'Y') return 'y';
        if (ch == 'n' || ch == 'N') return 'n';
        if (ch == '\x03' || ch == '\x18' || ch == '\033') {
            str::copy(ed->statusMsg, "Cancelled", sizeof(ed->statusMsg));
            return 'c';
        }
    }
}

void showHelpScreen() noexcept {
    auto& term = drivers::video::TerminalDriver::instance();
    static constexpr const char* kHelpLines[] = {
        "Cortez nano 1.0 — Full-Screen Interactive Text Editor Help",
        "======================================================================",
        "  Cortez nano is modeled after GNU nano for authoring C kernel modules,",
        "  device drivers, shell commands, and configuration files on NVMe/USB.",
        "",
        "File Operations:",
        "  ^O  (Ctrl+O)     Write Out (save current buffer, prompting for path)",
        "  ^S  (Ctrl+S)     Quick Save buffer to current file path",
        "  ^R  (Ctrl+R)     Read File (insert another VFS file at cursor)",
        "  ^X  (Ctrl+X)     Exit nano (prompts to save if buffer is modified)",
        "",
        "Navigation & View:",
        "  Arrows           Move cursor Up / Down / Left / Right",
        "  Home / ^A        Jump to beginning of current line",
        "  End  / ^E        Jump to end of current line",
        "  PgUp / ^Y        Scroll viewport one page up",
        "  PgDn / ^V        Scroll viewport one page down",
        "  ^_   (Ctrl+_)    Go To Line and Column number",
        "  ^C   (Ctrl+C)    Report current Line, Column, and Character offset",
        "  M-N  (Alt+N)     Toggle line numbers in left gutter",
        "  M-Y  (Alt+Y)     Toggle C/C++ syntax highlighting",
        "",
        "Editing, Cut/Copy/Paste & Undo:",
        "  ^^ / ^6 / M-A    Toggle Mark Set for region selection",
        "  ^K  (Ctrl+K)     Cut current line or marked region into cutbuffer",
        "  M-6 (Alt+6)      Copy current line or marked region into cutbuffer",
        "  ^U  (Ctrl+U)     Paste (Uncut) cutbuffer contents at cursor",
        "  M-U / ^Z         Undo last edit operation",
        "  M-E              Redo last undone operation",
        "  ^J  / M-I        Toggle automatic indentation on Enter",
        "",
        "Search, Replace & TinyCC Check:",
        "  ^W  (Ctrl+W)     Search forward (case-insensitive, wraps around)",
        "  M-W (Alt+W)      Repeat last forward search without prompting",
        "  ^\\  (Ctrl+\\)     Interactive Search and Replace (Y / N / A / ^C)",
        "  ^T  (Ctrl+T)     Compile-check current .c file with in-kernel TinyCC",
    };
    constexpr usize kHelpCount = sizeof(kHelpLines) / sizeof(kHelpLines[0]);
    usize scroll = 0;

    for (;;) {
        u32 termCols = term.getCols();
        u32 termRows = term.getRows();
        if (termRows < 6 || termCols < 20) return;
        u32 viewRows = (termRows > 3) ? (termRows - 3) : 1;

        term.setCursorVisible(false);

        // Title bar (Row 0)
        term.fillRowAt(0, 0, termCols, ' ',
                       drivers::video::TerminalDriver::COLOR_BAR_FG,
                       drivers::video::TerminalDriver::COLOR_BAR_BG);
        const char* helpTitle = "  Cortez nano 1.0 — Help Viewer";
        term.writeStringAt(0, 0, helpTitle, str::length(helpTitle),
                           drivers::video::TerminalDriver::COLOR_BAR_FG,
                           drivers::video::TerminalDriver::COLOR_BAR_BG);

        // Help lines (Rows 1 .. viewRows)
        for (u32 r = 0; r < viewRows; ++r) {
            u32 row = 1 + r;
            usize idx = scroll + r;
            if (idx < kHelpCount) {
                term.writeStringAt(0, row, "  ", 2,
                                   drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                                   drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
                const char* line = kHelpLines[idx];
                usize len = str::length(line);
                term.writeStringAt(2, row, line, len,
                                   drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                                   drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
                if (2 + len < termCols) {
                    term.fillRowAt(static_cast<u32>(2 + len), row,
                                   termCols - static_cast<u32>(2 + len), ' ',
                                   drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                                   drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
                }
            } else {
                term.fillRowAt(0, row, termCols, ' ',
                               drivers::video::TerminalDriver::DEFAULT_FG_COLOR,
                               drivers::video::TerminalDriver::DEFAULT_BG_COLOR);
            }
        }

        static constexpr ShortcutPair kHelpRow1[] = {
            { "^X",   "Close Help" },
            { "Up",   "Scroll Up"  },
            { "Down", "Scroll Dn"  }
        };
        static constexpr ShortcutPair kHelpRow2[] = {
            { "PgUp", "Page Up"   },
            { "PgDn", "Page Down" },
            { "Esc",  "Return"    }
        };
        renderShortcutRow(termCols, termRows - 2, kHelpRow1, 3);
        renderShortcutRow(termCols, termRows - 1, kHelpRow2, 3);

        char ch = readKeyBlocking();
        if (ch == '\x18' || ch == '\x11' || ch == '\x07' || ch == 'q' || ch == 'Q') {
            break;
        }
        if (ch == '\033') {
            auto& cin = drivers::input::InputManager::instance();
            cin.pollHardware();
            if (!cin.hasData()) break;
            char c1 = readKeyBlocking();
            if (c1 == '[' || c1 == 'O') {
                char c2 = readKeyBlocking();
                if (c2 == 'A' && scroll > 0) --scroll;
                else if (c2 == 'B' && scroll + viewRows < kHelpCount) ++scroll;
                else if (c2 == '5' || c2 == '6') {
                    (void)readKeyBlocking(); // '~'
                    if (c2 == '5') scroll = (scroll >= viewRows) ? (scroll - viewRows) : 0;
                    else if (scroll + viewRows < kHelpCount) scroll += viewRows;
                }
            } else {
                break;
            }
        }
    }
}

// =============================================================================
// High-Level Interactive Commands (^W, ^\, ^_, ^C, ^T)
// =============================================================================

void executeSearch(EditorBuffer* ed, bool promptUser) noexcept {
    char query[96]{};
    str::copy(query, ed->lastSearch, sizeof(query));

    if (promptUser) {
        char label[128]{};
        if (ed->lastSearch[0]) {
            klibc_snprintf(label, sizeof(label), "Search [%s]: ", ed->lastSearch);
        } else {
            str::copy(label, "Search: ", sizeof(label));
        }
        char input[96]{};
        if (!promptLineInput(ed, label, input, sizeof(input))) {
            ed->highlightMatch = false;
            return;
        }
        if (input[0] != '\0') {
            str::copy(query, input, sizeof(query));
            str::copy(ed->lastSearch, input, sizeof(ed->lastSearch));
        }
    }

    if (!query[0]) {
        str::copy(ed->statusMsg, "No search pattern", sizeof(ed->statusMsg));
        return;
    }

    usize foundRow = 0;
    usize foundCol = 0;
    bool  wrapped  = false;
    usize startCol = ed->curCol + 1;
    if (findNextOccurrence(ed, query, ed->curRow, startCol, true, &foundRow, &foundCol, &wrapped)) {
        ed->curRow         = foundRow;
        ed->curCol         = foundCol;
        ed->highlightMatch = true;
        ed->matchRow       = foundRow;
        ed->matchCol       = foundCol;
        ed->matchLen       = str::length(query);
        if (wrapped) {
            str::copy(ed->statusMsg, "Search Wrapped", sizeof(ed->statusMsg));
        } else {
            klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                           "Found \"%s\" at line %zu, col %zu", query, foundRow + 1, foundCol + 1);
        }
    } else {
        ed->highlightMatch = false;
        klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg), "\"%s\" not found", query);
    }
}

void executeReplace(EditorBuffer* ed) noexcept {
    char findStr[96]{};
    char label[128]{};
    if (ed->lastSearch[0]) {
        klibc_snprintf(label, sizeof(label), "Search (to replace) [%s]: ", ed->lastSearch);
    } else {
        str::copy(label, "Search (to replace): ", sizeof(label));
    }

    if (!promptLineInput(ed, label, findStr, sizeof(findStr))) return;
    if (findStr[0] == '\0') {
        str::copy(findStr, ed->lastSearch, sizeof(findStr));
    } else {
        str::copy(ed->lastSearch, findStr, sizeof(ed->lastSearch));
    }
    if (!findStr[0]) {
        str::copy(ed->statusMsg, "No search string specified", sizeof(ed->statusMsg));
        return;
    }

    char repStr[96]{};
    str::copy(repStr, ed->lastReplace, sizeof(repStr));
    if (!promptLineInput(ed, "Replace with: ", repStr, sizeof(repStr))) return;
    str::copy(ed->lastReplace, repStr, sizeof(ed->lastReplace));

    usize findLen   = str::length(findStr);
    usize repLen    = str::length(repStr);
    usize searchRow = ed->curRow;
    usize searchCol = ed->curCol;
    usize repCount  = 0;
    bool  replaceAll = false;
    bool  recordedUndo = false;

    static constexpr ShortcutPair kRepRow1[] = {
        { "Y", "Yes" },
        { "N", "No"  },
        { "A", "All" }
    };
    static constexpr ShortcutPair kRepRow2[] = {
        { "^C", "Cancel" }
    };

    for (;;) {
        usize mRow = 0;
        usize mCol = 0;
        if (!findNextOccurrence(ed, findStr, searchRow, searchCol, false, &mRow, &mCol, nullptr)) {
            break;
        }

        ed->curRow         = mRow;
        ed->curCol         = mCol;
        ed->highlightMatch = true;
        ed->matchRow       = mRow;
        ed->matchCol       = mCol;
        ed->matchLen       = findLen;
        ensureVisible(ed);

        char action = 'y';
        if (!replaceAll) {
            for (;;) {
                renderScreen(ed, "Replace this instance? (Y/N/A/^C)", "", 0, kRepRow1, 3, kRepRow2, 1);
                char ch = readKeyBlocking();
                if (ch == 'y' || ch == 'Y') { action = 'y'; break; }
                if (ch == 'n' || ch == 'N') { action = 'n'; break; }
                if (ch == 'a' || ch == 'A') { action = 'y'; replaceAll = true; break; }
                if (ch == '\x03' || ch == '\x18' || ch == '\033') { action = 'c'; break; }
            }
        }

        if (action == 'c') break;
        if (action == 'y') {
            if (!recordedUndo) {
                recordUndoSnapshot(ed);
                recordedUndo = true;
            }
            eraseSpanInLine(ed->lines[mRow], mCol, findLen);
            insertSpanInLine(ed->lines[mRow], mCol, repStr, repLen);
            ed->modified = true;
            ++repCount;
            searchRow = mRow;
            searchCol = mCol + repLen;
        } else {
            searchRow = mRow;
            searchCol = mCol + findLen;
        }
    }

    ed->highlightMatch = false;
    klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                   "Replaced %zu occurrence%s", repCount, (repCount == 1) ? "" : "s");
}

void executeGoToLine(EditorBuffer* ed) noexcept {
    char buf[48]{};
    if (!promptLineInput(ed, "Enter line number, column number: ", buf, sizeof(buf))) return;
    if (!buf[0]) return;

    char* endPtr = nullptr;
    long targetLine = klibc_strtol(buf, &endPtr, 10);
    long targetCol  = 1;
    if (endPtr && (*endPtr == ',' || *endPtr == ' ' || *endPtr == ':')) {
        targetCol = klibc_strtol(endPtr + 1, nullptr, 10);
    }

    if (targetLine <= 1) {
        ed->curRow = 0;
    } else if (static_cast<usize>(targetLine) >= ed->lineCount) {
        ed->curRow = ed->lineCount - 1;
    } else {
        ed->curRow = static_cast<usize>(targetLine - 1);
    }

    usize maxCol = ed->lines[ed->curRow].len;
    if (targetCol <= 1) {
        ed->curCol = 0;
    } else if (static_cast<usize>(targetCol - 1) > maxCol) {
        ed->curCol = maxCol;
    } else {
        ed->curCol = static_cast<usize>(targetCol - 1);
    }

    klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                   "Jumped to line %zu, col %zu", ed->curRow + 1, ed->curCol + 1);
}

void reportCursorLocation(EditorBuffer* ed) noexcept {
    usize totalChars = 0;
    usize charOffset = 0;
    for (usize r = 0; r < ed->lineCount; ++r) {
        if (r < ed->curRow) {
            charOffset += ed->lines[r].len + 1;
        } else if (r == ed->curRow) {
            charOffset += ed->curCol;
        }
        totalChars += ed->lines[r].len + 1;
    }
    usize linePct = (ed->lineCount > 0) ? ((ed->curRow + 1) * 100 / ed->lineCount) : 100;
    klibc_snprintf(ed->statusMsg, sizeof(ed->statusMsg),
                   "line %zu/%zu (%zu%%), col %zu/%zu, char %zu/%zu",
                   ed->curRow + 1, ed->lineCount, linePct,
                   ed->curCol + 1, ed->lines[ed->curRow].len + 1,
                   charOffset, totalChars);
}

void executeCompileCheck(EditorBuffer* ed) noexcept {
    if (!ed->filePath[0]) {
        char pathBuf[fs::PathResolver::MAX_PATH_LEN]{};
        if (!promptLineInput(ed, "Save file before TCC check as: ", pathBuf, sizeof(pathBuf)) || !pathBuf[0]) {
            return;
        }
        if (saveBuffer(ed, pathBuf) != Status::Ok) return;
    } else if (ed->modified) {
        if (saveBuffer(ed, ed->filePath) != Status::Ok) return;
    }

    char canonical[fs::PathResolver::MAX_PATH_LEN]{};
    fs::PathResolver::normalize(ed->cwd, ed->filePath, canonical, sizeof(canonical));

    auto& term = drivers::video::TerminalDriver::instance();
    term.clear();
    kprintf("\033[7m  Cortez nano 1.0 — TinyCC In-Kernel Compiler Check (%s)  \033[K\033[0m\n\n", canonical);

    Status st = module::ModuleManager::instance().checkCompile(canonical);
    if (st == Status::Ok) {
        str::copy(ed->statusMsg, "TCC: Compiled cleanly with 0 errors", sizeof(ed->statusMsg));
    } else {
        str::copy(ed->statusMsg, "TCC: Compilation failed (see diagnostics)", sizeof(ed->statusMsg));
    }

    kprintf("\n\033[7m Press any key to return to Cortez nano... \033[0m");
    (void)readKeyBlocking();
}

} // namespace

// =============================================================================
// Main Editor Entry Point & Event Loop
// =============================================================================

Status TextEditor::openAndEdit(const char* cwd,
                               const char* filePath,
                               usize initialLine,
                               usize initialCol) noexcept {
    auto* ed = new EditorBuffer();
    if (!ed) return Status::OutOfMemory;

    loadIntoBuffer(ed, cwd, filePath);

    if (initialLine > 0) {
        ed->curRow = (initialLine - 1 < ed->lineCount) ? (initialLine - 1) : (ed->lineCount - 1);
    }
    if (initialCol > 0) {
        usize maxCol = ed->lines[ed->curRow].len;
        ed->curCol   = (initialCol - 1 <= maxCol) ? (initialCol - 1) : maxCol;
    }

    auto& term = drivers::video::TerminalDriver::instance();
    term.clear();

    enum class EscState : u8 { None, Esc, Bracket, TildeParam };
    EscState escState = EscState::None;
    u32      escParam = 0;

    ensureVisible(ed);
    renderScreen(ed);

    for (;;) {
        char ch = readKeyBlocking();

        // 1. Escape / Alt (Meta) & CSI Sequence State Machine
        if (escState == EscState::Esc) {
            if (ch == '[' || ch == 'O') {
                escState = EscState::Bracket;
                escParam = 0;
                continue;
            }

            escState          = EscState::None;
            ed->typingInRun   = false;
            char lowerMeta    = str::toLower(ch);

            if (lowerMeta == 'n') { // Alt+N: Toggle Line Numbers
                ed->showLineNumbers = !ed->showLineNumbers;
                str::copy(ed->statusMsg,
                          ed->showLineNumbers ? "Line numbers enabled" : "Line numbers disabled",
                          sizeof(ed->statusMsg));
            } else if (lowerMeta == 'y') { // Alt+Y: Toggle Syntax Highlighting
                ed->syntaxHighlight = !ed->syntaxHighlight;
                str::copy(ed->statusMsg,
                          ed->syntaxHighlight ? "C syntax highlighting enabled" : "Syntax highlighting disabled",
                          sizeof(ed->statusMsg));
            } else if (lowerMeta == 'i') { // Alt+I: Toggle Auto-Indent
                ed->autoIndent = !ed->autoIndent;
                str::copy(ed->statusMsg,
                          ed->autoIndent ? "Auto-indent enabled" : "Auto-indent disabled",
                          sizeof(ed->statusMsg));
            } else if (lowerMeta == 'a') { // Alt+A: Mark Set
                ed->markActive = !ed->markActive;
                ed->markRow    = ed->curRow;
                ed->markCol    = ed->curCol;
                str::copy(ed->statusMsg, ed->markActive ? "Mark Set" : "Mark Unset", sizeof(ed->statusMsg));
            } else if (ch == '6') { // Alt+6: Copy Line / Region
                handleCopyLineOrRegion(ed);
            } else if (lowerMeta == 'w') { // Alt+W: Repeat Last Search
                executeSearch(ed, false);
            } else if (lowerMeta == 'u') { // Alt+U: Undo
                performUndo(ed);
            } else if (lowerMeta == 'e') { // Alt+E: Redo
                performRedo(ed);
            } else if (lowerMeta == 'g') { // Alt+G: Go To Line
                executeGoToLine(ed);
            } else if (ch == '\033') { // Double Esc clears mark/search highlight
                ed->markActive     = false;
                ed->highlightMatch = false;
                str::copy(ed->statusMsg, "Mark/Highlight cleared", sizeof(ed->statusMsg));
            }

            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (escState == EscState::Bracket || escState == EscState::TildeParam) {
            if (ch >= '0' && ch <= '9') {
                escParam = escParam * 10 + static_cast<u32>(ch - '0');
                escState = EscState::TildeParam;
                continue;
            }
            escState            = EscState::None;
            ed->typingInRun     = false;
            ed->lastKeyWasCut   = false;
            ed->highlightMatch  = false;

            u32 viewRows = (term.getRows() > 4) ? (term.getRows() - 4) : 1;
            if (ch == 'A') { // Up
                if (ed->curRow > 0) --ed->curRow;
            } else if (ch == 'B') { // Down
                if (ed->curRow + 1 < ed->lineCount) ++ed->curRow;
            } else if (ch == 'C') { // Right
                usize lineLen = ed->lines[ed->curRow].len;
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
                    ed->curCol = ed->lines[ed->curRow].len;
                }
            } else if (ch == 'H') { // Home
                ed->curCol = 0;
            } else if (ch == 'F') { // End
                ed->curCol = ed->lines[ed->curRow].len;
            } else if (ch == '~') {
                if (escParam == 1 || escParam == 7) {
                    ed->curCol = 0;
                } else if (escParam == 4 || escParam == 8) {
                    ed->curCol = ed->lines[ed->curRow].len;
                } else if (escParam == 5) { // PgUp
                    ed->curRow = (ed->curRow >= viewRows) ? (ed->curRow - viewRows) : 0;
                } else if (escParam == 6) { // PgDn
                    ed->curRow = (ed->curRow + viewRows < ed->lineCount)
                                     ? (ed->curRow + viewRows)
                                     : (ed->lineCount - 1);
                } else if (escParam == 3) { // Delete
                    recordUndoSnapshot(ed);
                    EditorLine& line = ed->lines[ed->curRow];
                    if (ed->curCol < line.len) {
                        eraseSpanInLine(line, ed->curCol, 1);
                        ed->modified = true;
                    } else if (ed->curRow + 1 < ed->lineCount) {
                        const EditorLine& nextLine = ed->lines[ed->curRow + 1];
                        insertSpanInLine(line, line.len, nextLine.data, nextLine.len);
                        removeLineAt(ed, ed->curRow + 1);
                        ed->modified = true;
                    }
                }
            }

            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\033') {
            escState = EscState::Esc;
            continue;
        }

        // 2. GNU nano Control Key Shortcuts
        if (ch != '\x0B') {
            ed->lastKeyWasCut = false;
        }
        ed->highlightMatch = false;

        if (ch == '\x07') { // ^G: Get Help
            ed->typingInRun = false;
            showHelpScreen();
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x0F') { // ^O: Write Out (Prompt for file name)
            ed->typingInRun = false;
            char target[fs::PathResolver::MAX_PATH_LEN]{};
            str::copy(target, ed->filePath, sizeof(target));
            if (promptLineInput(ed, "File Name to Write: ", target, sizeof(target))) {
                (void)saveBuffer(ed, target);
            }
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x13') { // ^S: Quick Save
            ed->typingInRun = false;
            if (ed->filePath[0]) {
                (void)saveBuffer(ed, ed->filePath);
            } else {
                char target[fs::PathResolver::MAX_PATH_LEN]{};
                if (promptLineInput(ed, "File Name to Write: ", target, sizeof(target))) {
                    (void)saveBuffer(ed, target);
                }
            }
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x18' || ch == '\x11') { // ^X or ^Q: Exit with modified buffer protection
            ed->typingInRun = false;
            if (ed->modified) {
                char ans = promptYesNoCancel(ed, "Save modified buffer? (Answering \"No\" will DISCARD changes.) ");
                if (ans == 'c') {
                    ensureVisible(ed);
                    renderScreen(ed);
                    continue;
                }
                if (ans == 'y') {
                    char target[fs::PathResolver::MAX_PATH_LEN]{};
                    str::copy(target, ed->filePath, sizeof(target));
                    if (!promptLineInput(ed, "File Name to Write: ", target, sizeof(target))) {
                        ensureVisible(ed);
                        renderScreen(ed);
                        continue;
                    }
                    if (saveBuffer(ed, target) != Status::Ok) {
                        ensureVisible(ed);
                        renderScreen(ed);
                        continue;
                    }
                }
            }
            term.clear();
            term.setCursorPos(0, 0);
            term.setCursorVisible(true);
            destroyEditor(ed);
            return Status::Ok;
        }

        if (ch == '\x12') { // ^R: Read File into buffer at cursor
            ed->typingInRun = false;
            char insPath[fs::PathResolver::MAX_PATH_LEN]{};
            if (promptLineInput(ed, "File to insert [from cwd]: ", insPath, sizeof(insPath)) && insPath[0]) {
                (void)insertFileIntoBuffer(ed, insPath);
            }
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x17') { // ^W: Where Is (Forward Search)
            ed->typingInRun = false;
            executeSearch(ed, true);
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x1C') { // ^\: Search and Replace
            ed->typingInRun = false;
            executeReplace(ed);
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x0B') { // ^K: Cut current line or marked region
            handleCutLineOrRegion(ed);
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x15') { // ^U: Paste (Uncut) cutbuffer
            handlePasteCutBuffer(ed);
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x1E') { // ^^ / ^6 / Ctrl+Space: Toggle Mark Set
            ed->typingInRun = false;
            ed->markActive  = !ed->markActive;
            ed->markRow     = ed->curRow;
            ed->markCol     = ed->curCol;
            str::copy(ed->statusMsg, ed->markActive ? "Mark Set" : "Mark Unset", sizeof(ed->statusMsg));
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x1F') { // ^_ / ^/: Go To Line and Column
            ed->typingInRun = false;
            executeGoToLine(ed);
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x03') { // ^C: Report Cursor Position
            ed->typingInRun = false;
            reportCursorLocation(ed);
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x14') { // ^T: TinyCC Compile Check
            ed->typingInRun = false;
            executeCompileCheck(ed);
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x1A') { // ^Z: Undo
            performUndo(ed);
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x0A') { // ^J / Enter handled below — check if raw ^J vs Enter
            // Note: PS/2 Enter emits '\n' (0x0A); auto-indent is toggled via Alt+I
        }

        if (ch == '\x01') { // ^A: Home
            ed->typingInRun = false;
            ed->curCol      = 0;
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x05') { // ^E: End
            ed->typingInRun = false;
            ed->curCol      = ed->lines[ed->curRow].len;
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x19') { // ^Y: Page Up (nano standard)
            ed->typingInRun = false;
            u32 viewRows    = (term.getRows() > 4) ? (term.getRows() - 4) : 1;
            ed->curRow      = (ed->curRow >= viewRows) ? (ed->curRow - viewRows) : 0;
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x16') { // ^V: Page Down (nano standard)
            ed->typingInRun = false;
            u32 viewRows    = (term.getRows() > 4) ? (term.getRows() - 4) : 1;
            ed->curRow      = (ed->curRow + viewRows < ed->lineCount)
                                  ? (ed->curRow + viewRows)
                                  : (ed->lineCount - 1);
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\x0C') { // ^L: Refresh screen
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        // 3. Document Editing Keys (Enter, Backspace, Tab, Printable ASCII)
        if (ch == '\r' || ch == '\n') { // Enter: Split line with optional auto-indent
            ed->typingInRun = false;
            if (ed->lineCount < MAX_LINES) {
                recordUndoSnapshot(ed);
                EditorLine& curr = ed->lines[ed->curRow];

                usize indent = 0;
                if (ed->autoIndent) {
                    while (indent < curr.len && indent < ed->curCol && curr.data[indent] == ' ') {
                        ++indent;
                    }
                    if (indent > 32) indent = 32;
                }

                const char* tailStr = curr.data + ed->curCol;
                usize       tailLen = curr.len - ed->curCol;

                if (insertLineAt(ed, ed->curRow + 1, "", 0)) {
                    EditorLine& next = ed->lines[ed->curRow + 1];
                    if (indent > 0) {
                        char spaces[32];
                        memset(spaces, ' ', indent);
                        insertSpanInLine(next, 0, spaces, indent);
                    }
                    insertSpanInLine(next, indent, tailStr, tailLen);

                    ed->lines[ed->curRow].len             = ed->curCol;
                    ed->lines[ed->curRow].data[ed->curCol] = '\0';
                    ++ed->curRow;
                    ed->curCol   = indent;
                    ed->modified = true;
                }
            }
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\b' || ch == 127) { // Backspace
            ed->typingInRun = false;
            if (ed->markActive) {
                copyOrCutRegion(ed, true);
                ensureVisible(ed);
                renderScreen(ed);
                continue;
            }
            EditorLine& line = ed->lines[ed->curRow];
            if (ed->curCol > 0) {
                recordUndoSnapshot(ed);
                eraseSpanInLine(line, ed->curCol - 1, 1);
                --ed->curCol;
                ed->modified = true;
            } else if (ed->curRow > 0) {
                recordUndoSnapshot(ed);
                EditorLine& prev = ed->lines[ed->curRow - 1];
                usize prevLen    = prev.len;
                insertSpanInLine(prev, prev.len, line.data, line.len);
                removeLineAt(ed, ed->curRow);
                --ed->curRow;
                ed->curCol   = prevLen;
                ed->modified = true;
            }
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (ch == '\t') { // Tab -> 4 spaces
            if (!ed->typingInRun) {
                recordUndoSnapshot(ed);
                ed->typingInRun = true;
            }
            u32 spaces = 4 - static_cast<u32>(ed->curCol % 4);
            const char pad[4] = { ' ', ' ', ' ', ' ' };
            if (insertSpanInLine(ed->lines[ed->curRow], ed->curCol, pad, spaces)) {
                ed->curCol  += spaces;
                ed->modified = true;
                ed->statusMsg[0] = '\0';
            }
            ensureVisible(ed);
            renderScreen(ed);
            continue;
        }

        if (static_cast<u8>(ch) >= 0x20U) {
            if (!ed->typingInRun || ch == ' ') {
                recordUndoSnapshot(ed);
                ed->typingInRun = (ch != ' ');
            }
            if (insertSpanInLine(ed->lines[ed->curRow], ed->curCol, &ch, 1)) {
                ++ed->curCol;
                ed->modified     = true;
                ed->statusMsg[0] = '\0';
            }
            ensureVisible(ed);
            renderScreen(ed);
        }
    }
}

} // namespace cortez::shell
