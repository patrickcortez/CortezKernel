/**
 * @file libtcc.cpp
 * @brief Freestanding x86_64 C Compiler implementing the libtcc In-Memory Compilation API.
 *
 * Compiles C source files and strings directly into x86_64 machine code in the Cortez Kernel's
 * executable ModuleArea (0xFFFFFFFFC0000000+). Supports:
 *   - Full C preprocessor (#include, object & function-like #define, #undef, #ifdef/#ifndef/#if/#else/#endif)
 *   - Primitive types, pointers, arrays, structs, enums, typedefs, and function pointers
 *   - Global/static variables, string literals, array/struct initializers (positional & designated)
 *   - Control flow (if/else, while, do-while, for, switch/case/default, break, continue, return, goto)
 *   - Full C expression grammar, pointer arithmetic, sizeof, type casts, and System V AMD64 ABI calls
 */

#include "libtcc.h"
#include "../../src/klibc/klibc.hpp"
#include "../../src/fs/vfs_manager.hpp"
#include "../../src/proc/scheduler.hpp"
#include "../../src/lib/string_utils.hpp"
#include "../../src/lib/printk.hpp"
#include "../../src/module/elf64.hpp"
#include <stdarg.h>

namespace {

using namespace cortez;

// =============================================================================
// Dynamic Byte Buffer
// =============================================================================

struct ByteVec {
    u8*   data = nullptr;
    usize size = 0;
    usize cap  = 0;

    void destroy() noexcept {
        if (data) {
            klibc_free(data);
            data = nullptr;
        }
        size = 0;
        cap  = 0;
    }

    bool reserve(usize needed) noexcept {
        if (needed <= cap) return true;
        usize newCap = (cap == 0) ? 1024 : cap;
        while (newCap < needed) newCap *= 2;
        auto* p = static_cast<u8*>(klibc_realloc(data, newCap));
        if (!p) return false;
        data = p;
        cap  = newCap;
        return true;
    }

    void push8(u8 v) noexcept {
        if (reserve(size + 1)) data[size++] = v;
    }

    void push16(u16 v) noexcept {
        if (reserve(size + 2)) {
            data[size++] = static_cast<u8>(v & 0xFFU);
            data[size++] = static_cast<u8>((v >> 8) & 0xFFU);
        }
    }

    void push32(u32 v) noexcept {
        if (reserve(size + 4)) {
            memcpy(data + size, &v, 4);
            size += 4;
        }
    }

    void push64(u64 v) noexcept {
        if (reserve(size + 8)) {
            memcpy(data + size, &v, 8);
            size += 8;
        }
    }

    void append(const void* bytes, usize len) noexcept {
        if (len == 0) return;
        if (reserve(size + len)) {
            memcpy(data + size, bytes, len);
            size += len;
        }
    }

    void appendChar(char c) noexcept {
        push8(static_cast<u8>(c));
    }

    void appendStr(const char* s) noexcept {
        if (!s) return;
        append(s, str::length(s));
    }

    void patch32(usize offset, u32 v) noexcept {
        if (offset + 4 <= size) {
            memcpy(data + offset, &v, 4);
        }
    }

    void patch64(usize offset, u64 v) noexcept {
        if (offset + 8 <= size) {
            memcpy(data + offset, &v, 8);
        }
    }
};

// =============================================================================
// Built-in Fallback System Headers (used when /ext/include/<file> is absent)
// =============================================================================

constexpr const char* kBuiltinStdintH = R"(
#pragma once
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef long               intptr_t;
typedef unsigned long      uintptr_t;
typedef unsigned long      size_t;
typedef long               ssize_t;
typedef long               ptrdiff_t;
typedef uint8_t            u8;
typedef uint16_t           u16;
typedef uint32_t           u32;
typedef uint64_t           u64;
typedef int8_t             i8;
typedef int16_t            i16;
typedef int32_t            i32;
typedef int64_t            i64;
typedef size_t             usize;
typedef ssize_t            isize;
#define NULL ((void*)0)
)";

constexpr const char* kBuiltinStdboolH = R"(
#pragma once
#define bool  _Bool
#define true  1
#define false 0
)";

constexpr const char* kBuiltinStddefH = R"(
#pragma once
typedef unsigned long size_t;
typedef long          ssize_t;
typedef long          ptrdiff_t;
#define NULL ((void*)0)
)";

constexpr const char* kBuiltinStringH = R"(
#pragma once
#include <stddef.h>
void*  memcpy(void* dst, const void* src, size_t n);
void*  memset(void* dst, int val, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
int    memcmp(const void* s1, const void* s2, size_t n);
size_t strlen(const char* s);
char*  strcpy(char* dst, const char* src);
char*  strncpy(char* dst, const char* src, size_t n);
char*  strcat(char* dst, const char* src);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
int    strcasecmp(const char* a, const char* b);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);
char*  strstr(const char* haystack, const char* needle);
char*  strdup(const char* s);
)";

constexpr const char* kBuiltinStdlibH = R"(
#pragma once
#include <stddef.h>
void*         malloc(size_t size);
void*         calloc(size_t nmemb, size_t size);
void*         realloc(void* ptr, size_t size);
void          free(void* ptr);
int           atoi(const char* s);
long          strtol(const char* s, char** endptr, int base);
unsigned long strtoul(const char* s, char** endptr, int base);
)";

constexpr const char* kBuiltinStdioH = R"(
#pragma once
#include <stddef.h>
int printf(const char* fmt, ...);
int snprintf(char* buf, size_t size, const char* fmt, ...);
int sprintf(char* buf, const char* fmt, ...);
)";

constexpr const char* kBuiltinCortezH = R"(
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define CORTEZ_API_VERSION 1
#define MODULE_NAME(s) const char __module_name[] = s
#define MODULE_DESC(s) const char __module_desc[] = s

#define KLOG_INFO  0
#define KLOG_OK    1
#define KLOG_WARN  2
#define KLOG_ERR   3

#define K_O_RDONLY 0x0001
#define K_O_WRONLY 0x0002
#define K_O_RDWR   0x0003
#define K_O_CREAT  0x0040
#define K_O_TRUNC  0x0200
#define K_O_APPEND 0x0400

typedef int  (*k_cmd_fn)(int argc, const char** argv);
typedef void (*k_irq_fn)(void* ctx);
typedef void (*k_task_fn)(void* arg);

typedef struct {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint16_t vendor;
    uint16_t device;
    uint64_t bar0;
    size_t   bar0_size;
} k_pci_dev;

typedef struct {
    long (*read)(void* ctx, void* buf, size_t n);
    long (*write)(void* ctx, const void* buf, size_t n);
    int  (*has_data)(void* ctx);
    void* ctx;
} k_chardev_ops;

void     k_printf(const char* fmt, ...);
void     k_log(int level, const char* subsys, const char* fmt, ...);
int      k_getchar(void);
int      k_kbhit(void);

void*    k_malloc(size_t size);
void*    k_calloc(size_t nmemb, size_t size);
void*    k_realloc(void* ptr, size_t size);
void     k_free(void* ptr);
uint64_t k_alloc_pages(size_t count);
void     k_free_pages(uint64_t phys, size_t count);
void*    k_phys_to_virt(uint64_t phys);
uint64_t k_virt_to_phys(const void* virt);
void*    k_map_mmio(uint64_t phys, size_t size);

uint8_t  k_inb(uint16_t port);
void     k_outb(uint16_t port, uint8_t val);
uint16_t k_inw(uint16_t port);
void     k_outw(uint16_t port, uint16_t val);
uint32_t k_inl(uint16_t port);
void     k_outl(uint16_t port, uint32_t val);
void     k_io_wait(void);

int      k_irq_alloc_vector(void);
int      k_irq_register(int vector, k_irq_fn fn, void* ctx);
int      k_irq_route_isa(int irq, int vector, int level_triggered);
void     k_irq_unregister(int vector);

int      k_pci_count(void);
int      k_pci_get(int idx, k_pci_dev* out);
uint32_t k_pci_read32(const k_pci_dev* dev, uint16_t offset);
void     k_pci_write32(const k_pci_dev* dev, uint16_t offset, uint32_t value);
void     k_pci_enable_busmaster(const k_pci_dev* dev);
int      k_pci_enable_msi(const k_pci_dev* dev, int vector);

int      k_register_command(const char* name, const char* usage, const char* desc, k_cmd_fn fn);
int      k_unregister_command(const char* name);

int      k_register_chardev(const char* name, const k_chardev_ops* ops);
int      k_unregister_chardev(const char* name);

int      k_open(const char* path, int flags);
long     k_read(int fd, void* buf, size_t count);
long     k_write(int fd, const void* buf, size_t count);
long     k_lseek(int fd, long offset, int whence);
int      k_close(int fd);

int      k_task_spawn(const char* name, k_task_fn fn, void* arg);
int      k_task_kill(int pid);
void     k_yield(void);
void     k_sleep_ms(uint32_t ms);
uint64_t k_ticks(void);
void     k_delay_us(uint32_t us);
)";

const char* findBuiltinHeader(const char* name) noexcept {
    if (!name) return nullptr;
    const char* base = str::findLastChar(name, '/');
    base = base ? (base + 1) : name;
    if (str::compare(base, "cortez.h") == 0)  return kBuiltinCortezH;
    if (str::compare(base, "stdint.h") == 0)  return kBuiltinStdintH;
    if (str::compare(base, "stdbool.h") == 0) return kBuiltinStdboolH;
    if (str::compare(base, "stddef.h") == 0)  return kBuiltinStddefH;
    if (str::compare(base, "string.h") == 0)  return kBuiltinStringH;
    if (str::compare(base, "stdlib.h") == 0)  return kBuiltinStdlibH;
    if (str::compare(base, "stdio.h") == 0)   return kBuiltinStdioH;
    if (str::compare(base, "stdarg.h") == 0)  return "#pragma once\n";
    return nullptr;
}

bool isIdentStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentChar(char c) noexcept {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

// =============================================================================
// Compiler Data Structures
// =============================================================================

constexpr usize MAX_INCLUDE_PATHS = 8;
constexpr usize MAX_MACROS        = 192;
constexpr usize MAX_ONCE_FILES    = 32;
constexpr usize MAX_EXT_SYMS      = 160;
constexpr usize MAX_FUNCS         = 96;
constexpr usize MAX_GLOBALS       = 128;
constexpr usize MAX_LOCALS        = 96;
constexpr usize MAX_STRUCTS       = 48;
constexpr usize MAX_STRUCT_FIELDS = 24;
constexpr usize MAX_TYPEDEFS      = 64;
constexpr usize MAX_ENUMS         = 128;
constexpr usize MAX_RELOCS        = 1024;
constexpr usize MAX_LABELS        = 32;
constexpr usize MAX_GOTOS         = 32;
constexpr usize MAX_LOOP_FIXUPS   = 64;
constexpr usize MAX_SWITCH_CASES  = 64;

struct MacroDef {
    bool isUsed;
    bool isFuncLike;
    u8   paramCount;
    char name[48];
    char params[8][24];
    char body[256];
};

enum class BaseKind : u8 {
    Void = 0,
    Bool,
    Char,
    Short,
    Int,
    Long,
    Struct,
    FuncPtr
};

struct CType {
    BaseKind base       = BaseKind::Int;
    bool     isUnsigned = false;
    u8       ptrDepth   = 0;
    bool     isArray    = false;
    usize    arrayLen   = 0;
    u16      structIdx  = 0;
};

struct StructField {
    char  name[32];
    CType type;
    usize offset;
};

struct StructDef {
    char        name[40];
    StructField fields[MAX_STRUCT_FIELDS];
    usize       fieldCount;
    usize       size;
    usize       align;
};

struct TypedefEntry {
    char  name[40];
    CType type;
};

struct EnumEntry {
    char name[40];
    i64  value;
};

struct ExternalSymbol {
    char        name[48];
    const void* addr;
};

struct FunctionEntry {
    char  name[48];
    CType returnType;
    CType paramTypes[8];
    usize paramCount;
    bool  isVariadic;
    bool  isDefined;
    usize codeOffset;
    void* relocatedAddr;
};

struct GlobalVar {
    char  name[48];
    CType type;
    usize dataOffset;
    usize byteSize;
    bool  isExtern;
    i32   initFuncRef[8];   // For struct initializers holding function pointers: slot -> funcIdx (-1 if none)
    usize initFuncSlotOff[8];
    usize initFuncCount;
};

struct LocalVar {
    char  name[40];
    CType type;
    i32   rbpOffset; // Negative offset from rbp (or positive for >6th arg)
    u32   scopeDepth;
};

enum class RelocKind : u8 {
    InternalFuncCall32 = 0, // 32-bit PC-relative call to FunctionEntry
    InternalFuncAddr64 = 1, // 64-bit absolute address of FunctionEntry in code
    ExternalSymAddr64  = 2, // 64-bit absolute address of ExternalSymbol in code
    DataOffsetAddr64   = 3, // 64-bit absolute address of dataOffset in code
    DataFuncPtr64      = 4  // 64-bit function pointer inside .data section
};

struct RelocEntry {
    RelocKind kind;
    usize     patchOffset; // Offset inside codeVec (or dataVec for DataFuncPtr64)
    usize     targetIndex; // funcIdx, extSymIdx, or byte offset inside dataVec
};

// =============================================================================
// Lexer Tokens
// =============================================================================

enum class TokKind : u8 {
    Eof = 0,
    Ident,
    Number,
    StringLit,
    // Keywords
    KwVoid, KwChar, KwShort, KwInt, KwLong, KwSigned, KwUnsigned, KwBool,
    KwConst, KwVolatile, KwStatic, KwExtern, KwInline, KwRegister,
    KwStruct, KwUnion, KwEnum, KwTypedef, KwSizeof,
    KwIf, KwElse, KwWhile, KwDo, KwFor, KwSwitch, KwCase, KwDefault,
    KwBreak, KwContinue, KwReturn, KwGoto,
    // Punctuation & Operators
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Semicolon, Comma, Dot, Arrow, Ellipsis, Question, Colon,
    Plus, Minus, Star, Slash, Percent,
    Amp, Pipe, Caret, Tilde, Bang,
    AmpAmp, PipePipe,
    PlusPlus, MinusMinus,
    Shl, Shr,
    Eq, Ne, Lt, Le, Gt, Ge,
    Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
    ShlAssign, ShrAssign, AmpAssign, CaretAssign, PipeAssign
};

struct Token {
    TokKind kind;
    u32     line;
    i64     numVal;
    u32     strOff; // Offset into stringPool
    u32     strLen;
};

} // namespace

// =============================================================================
// TCCState Definition
// =============================================================================

struct TCCState {
    void*        errorOpaque = nullptr;
    TCCErrorFunc errorFunc   = nullptr;
    int          outputType  = TCC_OUTPUT_MEMORY;
    bool         hasError    = false;
    char         currentFile[128] = "<memory>";
    u32          currentLine = 1;
    cortez_jmp_buf errorJmp{};
    bool         jmpActive   = false;

    char         includePaths[MAX_INCLUDE_PATHS][96]{};
    usize        includePathCount = 0;

    char         onceFiles[MAX_ONCE_FILES][96]{};
    usize        onceFileCount = 0;

    MacroDef       macros[MAX_MACROS]{};
    usize          macroCount = 0;

    StructDef      structs[MAX_STRUCTS]{};
    usize          structCount = 0;

    TypedefEntry   typedefs[MAX_TYPEDEFS]{};
    usize          typedefCount = 0;

    EnumEntry      enums[MAX_ENUMS]{};
    usize          enumCount = 0;

    ExternalSymbol extSyms[MAX_EXT_SYMS]{};
    usize          extSymCount = 0;

    FunctionEntry  funcs[MAX_FUNCS]{};
    usize          funcCount = 0;

    GlobalVar      globals[MAX_GLOBALS]{};
    usize          globalCount = 0;

    RelocEntry     relocs[MAX_RELOCS]{};
    usize          relocCount = 0;

    ByteVec        codeVec{};
    ByteVec        dataVec{};
    ByteVec        stringPool{};

    Token*         tokens     = nullptr;
    usize          tokenCount = 0;
    usize          tokenCap   = 0;
    usize          tokPos     = 0;

    // Active function compilation state
    LocalVar       locals[MAX_LOCALS]{};
    usize          localCount     = 0;
    u32            scopeDepth     = 0;
    i32            curStackOffset = 0;
    i32            maxStackOffset = 0;
    usize          frameSubPatch  = 0;
    CType          curReturnType{};

    usize          breakPatches[MAX_LOOP_FIXUPS]{};
    usize          breakCount = 0;
    usize          continuePatches[MAX_LOOP_FIXUPS]{};
    usize          continueCount = 0;

    struct LabelInfo {
        char  name[32];
        usize codeOffset;
    };
    LabelInfo      labels[MAX_LABELS]{};
    usize          labelCount = 0;
    LabelInfo      gotos[MAX_GOTOS]{};
    usize          gotoCount  = 0;

    void*          relocatedBase = nullptr;
    usize          relocatedTextSize = 0;
};

namespace {

// =============================================================================
// Error Reporting
// =============================================================================

void reportError(TCCState* s, u32 line, const char* fmt, ...) noexcept {
    s->hasError = true;
    char msgBuf[256];
    char bodyBuf[192];

    va_list args;
    va_start(args, fmt);
    klibc_vsnprintf(bodyBuf, sizeof(bodyBuf), fmt, args);
    va_end(args);

    klibc_snprintf(msgBuf, sizeof(msgBuf), "%s:%u: error: %s",
                   s->currentFile, line ? line : s->currentLine, bodyBuf);

    if (s->errorFunc) {
        s->errorFunc(s->errorOpaque, msgBuf);
    } else {
        kprintf("\033[31m%s\033[0m\n", msgBuf);
    }

    if (s->jmpActive) {
        cortez_longjmp(s->errorJmp, 1);
    }
}

// =============================================================================
// Type & Struct Layout Helpers
// =============================================================================

usize typeAlign(TCCState* s, const CType& t) noexcept;

usize typeSize(TCCState* s, const CType& t) noexcept {
    if (t.isArray) {
        CType elem   = t;
        elem.isArray = false;
        return t.arrayLen * typeSize(s, elem);
    }
    if (t.ptrDepth > 0 || t.base == BaseKind::FuncPtr) {
        return 8;
    }
    switch (t.base) {
        case BaseKind::Void:   return 1;
        case BaseKind::Bool:   return 1;
        case BaseKind::Char:   return 1;
        case BaseKind::Short:  return 2;
        case BaseKind::Int:    return 4;
        case BaseKind::Long:   return 8;
        case BaseKind::Struct:
            return (t.structIdx < s->structCount) ? s->structs[t.structIdx].size : 8;
        default: return 8;
    }
}

usize typeAlign(TCCState* s, const CType& t) noexcept {
    if (t.isArray) {
        CType elem   = t;
        elem.isArray = false;
        return typeAlign(s, elem);
    }
    if (t.ptrDepth > 0 || t.base == BaseKind::FuncPtr) return 8;
    switch (t.base) {
        case BaseKind::Void:
        case BaseKind::Bool:
        case BaseKind::Char:   return 1;
        case BaseKind::Short:  return 2;
        case BaseKind::Int:    return 4;
        case BaseKind::Long:   return 8;
        case BaseKind::Struct:
            return (t.structIdx < s->structCount) ? s->structs[t.structIdx].align : 8;
        default: return 8;
    }
}

CType decayArrayType(const CType& t) noexcept {
    if (!t.isArray) return t;
    CType p   = t;
    p.isArray = false;
    p.ptrDepth++;
    return p;
}

usize derefStride(TCCState* s, const CType& ptrType) noexcept {
    CType p = decayArrayType(ptrType);
    if (p.ptrDepth == 0) return 1;
    --p.ptrDepth;
    if (p.ptrDepth == 0 && p.base == BaseKind::Void) return 1;
    usize sz = typeSize(s, p);
    return sz ? sz : 1;
}

void registerDefaultTypes(TCCState* s) noexcept {
    auto addTd = [&](const char* name, BaseKind base, bool isUnsigned, u8 ptrDepth = 0) {
        if (s->typedefCount >= MAX_TYPEDEFS) return;
        auto& td = s->typedefs[s->typedefCount++];
        str::copy(td.name, name, sizeof(td.name));
        td.type = CType{ base, isUnsigned, ptrDepth, false, 0, 0 };
    };
    addTd("int8_t",    BaseKind::Char,  false);
    addTd("uint8_t",   BaseKind::Char,  true);
    addTd("int16_t",   BaseKind::Short, false);
    addTd("uint16_t",  BaseKind::Short, true);
    addTd("int32_t",   BaseKind::Int,   false);
    addTd("uint32_t",  BaseKind::Int,   true);
    addTd("int64_t",   BaseKind::Long,  false);
    addTd("uint64_t",  BaseKind::Long,  true);
    addTd("size_t",    BaseKind::Long,  true);
    addTd("ssize_t",   BaseKind::Long,  false);
    addTd("uintptr_t", BaseKind::Long,  true);
    addTd("intptr_t",  BaseKind::Long,  false);
    addTd("ptrdiff_t", BaseKind::Long,  false);
    addTd("u8",        BaseKind::Char,  true);
    addTd("u16",       BaseKind::Short, true);
    addTd("u32",       BaseKind::Int,   true);
    addTd("u64",       BaseKind::Long,  true);
    addTd("i8",        BaseKind::Char,  false);
    addTd("i16",       BaseKind::Short, false);
    addTd("i32",       BaseKind::Int,   false);
    addTd("i64",       BaseKind::Long,  false);
    addTd("usize",     BaseKind::Long,  true);
    addTd("isize",     BaseKind::Long,  false);
}

// =============================================================================
// Preprocessor Implementation
// =============================================================================

MacroDef* findMacro(TCCState* s, const char* name) noexcept {
    for (usize i = 0; i < s->macroCount; ++i) {
        if (s->macros[i].isUsed && str::compare(s->macros[i].name, name) == 0) {
            return &s->macros[i];
        }
    }
    return nullptr;
}

void setMacro(TCCState* s, const char* name, const char* body) noexcept {
    if (!name || !name[0]) return;
    MacroDef* m = findMacro(s, name);
    if (!m) {
        if (s->macroCount >= MAX_MACROS) return;
        m = &s->macros[s->macroCount++];
    }
    m->isUsed     = true;
    m->isFuncLike = false;
    m->paramCount = 0;
    str::copy(m->name, name, sizeof(m->name));
    str::copy(m->body, body ? body : "1", sizeof(m->body));
}

void removeMacro(TCCState* s, const char* name) noexcept {
    MacroDef* m = findMacro(s, name);
    if (m) m->isUsed = false;
}

void expandMacrosInLine(TCCState* s, const char* inLine, ByteVec& out, int depth = 0) noexcept {
    if (depth > 12) {
        out.appendStr(inLine);
        return;
    }

    ByteVec pass{};
    const char* p = inLine;
    bool expandedAny = false;

    while (*p) {
        // Do not expand inside string or character literals
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            pass.appendChar(quote);
            while (*p && *p != quote) {
                if (*p == '\\' && p[1]) {
                    pass.appendChar(*p++);
                }
                pass.appendChar(*p++);
            }
            if (*p == quote) pass.appendChar(*p++);
            continue;
        }

        if (isIdentStart(*p)) {
            char ident[64];
            usize ilen = 0;
            while (isIdentChar(*p) && ilen + 1 < sizeof(ident)) {
                ident[ilen++] = *p++;
            }
            ident[ilen] = '\0';

            MacroDef* m = findMacro(s, ident);
            if (!m) {
                pass.append(ident, ilen);
                continue;
            }

            if (!m->isFuncLike) {
                pass.appendStr(m->body);
                expandedAny = true;
                continue;
            }

            // Function-like macro: check for '('
            const char* saveP = p;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p != '(') {
                p = saveP;
                pass.append(ident, ilen);
                continue;
            }
            ++p; // skip '('

            char args[8][128]{};
            u8   argCount = 0;
            int  parenDepth = 1;
            usize aPos = 0;

            while (*p && parenDepth > 0) {
                if (*p == '"' || *p == '\'') {
                    char q = *p++;
                    if (argCount < 8 && aPos + 1 < 128) args[argCount][aPos++] = q;
                    while (*p && *p != q) {
                        if (*p == '\\' && p[1]) {
                            if (argCount < 8 && aPos + 1 < 128) args[argCount][aPos++] = *p++;
                        }
                        if (argCount < 8 && aPos + 1 < 128) args[argCount][aPos++] = *p++;
                    }
                    if (*p == q) {
                        if (argCount < 8 && aPos + 1 < 128) args[argCount][aPos++] = *p++;
                    }
                    continue;
                }
                if (*p == '(') {
                    ++parenDepth;
                    if (argCount < 8 && aPos + 1 < 128) args[argCount][aPos++] = *p++;
                } else if (*p == ')') {
                    --parenDepth;
                    if (parenDepth == 0) {
                        if (argCount < 8) {
                            args[argCount][aPos] = '\0';
                            if (aPos > 0 || argCount > 0) ++argCount;
                        }
                        ++p;
                        break;
                    } else {
                        if (argCount < 8 && aPos + 1 < 128) args[argCount][aPos++] = *p++;
                    }
                } else if (*p == ',' && parenDepth == 1) {
                    if (argCount < 8) {
                        args[argCount][aPos] = '\0';
                        ++argCount;
                        aPos = 0;
                    }
                    ++p;
                } else {
                    if (argCount < 8 && aPos + 1 < 128) args[argCount][aPos++] = *p;
                    ++p;
                }
            }

            // Substitute parameters in macro body
            const char* bp = m->body;
            while (*bp) {
                if (isIdentStart(*bp)) {
                    char bTok[48];
                    usize blen = 0;
                    while (isIdentChar(*bp) && blen + 1 < sizeof(bTok)) {
                        bTok[blen++] = *bp++;
                    }
                    bTok[blen] = '\0';

                    bool matchedParam = false;
                    for (u8 pi = 0; pi < m->paramCount && pi < argCount; ++pi) {
                        if (str::compare(bTok, m->params[pi]) == 0) {
                            // Trim leading/trailing spaces on argument
                            const char* a = args[pi];
                            while (*a == ' ' || *a == '\t') ++a;
                            pass.appendStr(a);
                            matchedParam = true;
                            break;
                        }
                    }
                    if (!matchedParam) {
                        pass.append(bTok, blen);
                    }
                } else {
                    pass.appendChar(*bp++);
                }
            }
            expandedAny = true;
            continue;
        }

        pass.appendChar(*p++);
    }

    pass.appendChar('\0');
    if (expandedAny) {
        expandMacrosInLine(s, reinterpret_cast<const char*>(pass.data), out, depth + 1);
    } else {
        out.append(pass.data, pass.size - 1);
    }
    pass.destroy();
}

char* readVfsFileToHeap(const char* cwd, const char* path, usize* outSize) noexcept {
    auto& vfs = fs::VfsManager::instance();
    fs::FileStat st{};
    if (vfs.stat(cwd, path, &st) != Status::Ok || st.type != fs::NodeType::File) {
        return nullptr;
    }
    auto fdRes = vfs.open(cwd, path, fs::O_RDONLY);
    if (fdRes.isError()) return nullptr;
    i32 fd = fdRes.value();

    usize fsize = static_cast<usize>(st.size);
    char* buf = static_cast<char*>(klibc_malloc(fsize + 2));
    if (!buf) {
        vfs.close(fd);
        return nullptr;
    }
    auto rRes = vfs.read(fd, buf, fsize);
    vfs.close(fd);

    usize got = rRes.isOk() ? rRes.value() : 0;
    buf[got]     = '\n';
    buf[got + 1] = '\0';
    if (outSize) *outSize = got;
    return buf;
}

bool evalIfExpr(TCCState* s, const char* expr) noexcept {
    while (*expr == ' ' || *expr == '\t') ++expr;
    bool invert = false;
    while (*expr == '!') {
        invert = !invert;
        ++expr;
        while (*expr == ' ' || *expr == '\t') ++expr;
    }

    bool result = false;
    if (str::compareN(expr, "defined", 7) == 0 && !isIdentChar(expr[7])) {
        expr += 7;
        while (*expr == ' ' || *expr == '\t' || *expr == '(') ++expr;
        char name[48];
        usize n = 0;
        while (isIdentChar(*expr) && n + 1 < sizeof(name)) {
            name[n++] = *expr++;
        }
        name[n] = '\0';
        result = (findMacro(s, name) != nullptr);
    } else if (*expr >= '0' && *expr <= '9') {
        result = (klibc_strtol(expr, nullptr, 0) != 0);
    } else if (isIdentStart(*expr)) {
        char name[48];
        usize n = 0;
        while (isIdentChar(*expr) && n + 1 < sizeof(name)) {
            name[n++] = *expr++;
        }
        name[n] = '\0';
        MacroDef* m = findMacro(s, name);
        result = m ? (klibc_strtol(m->body, nullptr, 0) != 0) : false;
    }
    return invert ? !result : result;
}

void preprocessSource(TCCState* s, const char* filename, const char* src, ByteVec& out, int includeDepth) noexcept {
    if (includeDepth > 10) return;

    // 1. Strip comments while preserving newlines
    usize srcLen = str::length(src);
    char* clean  = static_cast<char*>(klibc_malloc(srcLen + 2));
    if (!clean) return;

    usize cpos = 0;
    for (usize i = 0; i < srcLen;) {
        // Line continuation \\\n
        if (src[i] == '\\' && i + 1 < srcLen && src[i + 1] == '\n') {
            i += 2;
            continue;
        }
        if (src[i] == '\\' && i + 2 < srcLen && src[i + 1] == '\r' && src[i + 2] == '\n') {
            i += 3;
            continue;
        }
        // String / char literal
        if (src[i] == '"' || src[i] == '\'') {
            char q = src[i++];
            clean[cpos++] = q;
            while (i < srcLen && src[i] != q) {
                if (src[i] == '\\' && i + 1 < srcLen) {
                    clean[cpos++] = src[i++];
                }
                clean[cpos++] = src[i++];
            }
            if (i < srcLen) clean[cpos++] = src[i++];
            continue;
        }
        // Line comment
        if (src[i] == '/' && i + 1 < srcLen && src[i + 1] == '/') {
            i += 2;
            while (i < srcLen && src[i] != '\n') ++i;
            continue;
        }
        // Block comment
        if (src[i] == '/' && i + 1 < srcLen && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < srcLen && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n') clean[cpos++] = '\n';
                ++i;
            }
            if (i + 1 < srcLen) i += 2;
            clean[cpos++] = ' ';
            continue;
        }
        clean[cpos++] = src[i++];
    }
    clean[cpos] = '\0';

    // 2. Process line by line
    bool condStack[16];
    bool condTaken[16];
    int  condDepth = 0;
    auto isActive = [&]() -> bool {
        for (int i = 0; i < condDepth; ++i) {
            if (!condStack[i]) return false;
        }
        return true;
    };

    const char* p = clean;
    while (*p) {
        const char* lineStart = p;
        while (*p && *p != '\n') ++p;
        usize lineLen = static_cast<usize>(p - lineStart);
        if (*p == '\n') ++p;

        char lineBuf[512];
        if (lineLen >= sizeof(lineBuf)) lineLen = sizeof(lineBuf) - 1;
        memcpy(lineBuf, lineStart, lineLen);
        if (lineLen > 0 && lineBuf[lineLen - 1] == '\r') --lineLen;
        lineBuf[lineLen] = '\0';

        const char* lp = lineBuf;
        while (*lp == ' ' || *lp == '\t') ++lp;

        if (*lp == '#') {
            ++lp;
            while (*lp == ' ' || *lp == '\t') ++lp;

            char dir[32];
            usize dlen = 0;
            while (isIdentChar(*lp) && dlen + 1 < sizeof(dir)) {
                dir[dlen++] = *lp++;
            }
            dir[dlen] = '\0';
            while (*lp == ' ' || *lp == '\t') ++lp;

            if (str::compare(dir, "ifdef") == 0 || str::compare(dir, "ifndef") == 0) {
                char mname[48];
                usize n = 0;
                while (isIdentChar(*lp) && n + 1 < sizeof(mname)) mname[n++] = *lp++;
                mname[n] = '\0';
                bool exists = (findMacro(s, mname) != nullptr);
                bool cond   = (str::compare(dir, "ifdef") == 0) ? exists : !exists;
                if (condDepth < 16) {
                    condStack[condDepth] = isActive() && cond;
                    condTaken[condDepth] = condStack[condDepth];
                    ++condDepth;
                }
                out.appendChar('\n');
                continue;
            }
            if (str::compare(dir, "if") == 0) {
                bool cond = isActive() && evalIfExpr(s, lp);
                if (condDepth < 16) {
                    condStack[condDepth] = cond;
                    condTaken[condDepth] = cond;
                    ++condDepth;
                }
                out.appendChar('\n');
                continue;
            }
            if (str::compare(dir, "elif") == 0) {
                if (condDepth > 0) {
                    bool parentActive = true;
                    for (int i = 0; i + 1 < condDepth; ++i) {
                        if (!condStack[i]) parentActive = false;
                    }
                    if (parentActive && !condTaken[condDepth - 1] && evalIfExpr(s, lp)) {
                        condStack[condDepth - 1] = true;
                        condTaken[condDepth - 1] = true;
                    } else {
                        condStack[condDepth - 1] = false;
                    }
                }
                out.appendChar('\n');
                continue;
            }
            if (str::compare(dir, "else") == 0) {
                if (condDepth > 0) {
                    bool parentActive = true;
                    for (int i = 0; i + 1 < condDepth; ++i) {
                        if (!condStack[i]) parentActive = false;
                    }
                    condStack[condDepth - 1] = parentActive && !condTaken[condDepth - 1];
                    condTaken[condDepth - 1] = true;
                }
                out.appendChar('\n');
                continue;
            }
            if (str::compare(dir, "endif") == 0) {
                if (condDepth > 0) --condDepth;
                out.appendChar('\n');
                continue;
            }

            if (!isActive()) {
                out.appendChar('\n');
                continue;
            }

            if (str::compare(dir, "pragma") == 0) {
                if (str::compareN(lp, "once", 4) == 0 && filename && filename[0]) {
                    if (s->onceFileCount < MAX_ONCE_FILES) {
                        str::copy(s->onceFiles[s->onceFileCount++], filename, 96);
                    }
                }
                out.appendChar('\n');
                continue;
            }

            if (str::compare(dir, "define") == 0) {
                char mname[48];
                usize n = 0;
                while (isIdentChar(*lp) && n + 1 < sizeof(mname)) mname[n++] = *lp++;
                mname[n] = '\0';

                if (*lp == '(') { // Function-like macro (no space between name and '(')
                    ++lp;
                    MacroDef* m = findMacro(s, mname);
                    if (!m && s->macroCount < MAX_MACROS) {
                        m = &s->macros[s->macroCount++];
                    }
                    if (m) {
                        m->isUsed     = true;
                        m->isFuncLike = true;
                        m->paramCount = 0;
                        str::copy(m->name, mname, sizeof(m->name));
                        while (*lp && *lp != ')') {
                            while (*lp == ' ' || *lp == '\t' || *lp == ',') ++lp;
                            if (*lp == ')') break;
                            usize pn = 0;
                            u8 idx = m->paramCount;
                            while (isIdentChar(*lp) && pn + 1 < 24) {
                                if (idx < 8) m->params[idx][pn++] = *lp;
                                ++lp;
                            }
                            if (idx < 8 && pn > 0) {
                                m->params[idx][pn] = '\0';
                                m->paramCount++;
                            }
                            while (*lp == ' ' || *lp == '\t') ++lp;
                        }
                        if (*lp == ')') ++lp;
                        while (*lp == ' ' || *lp == '\t') ++lp;
                        str::copy(m->body, lp, sizeof(m->body));
                    }
                } else {
                    while (*lp == ' ' || *lp == '\t') ++lp;
                    setMacro(s, mname, *lp ? lp : "1");
                }
                out.appendChar('\n');
                continue;
            }

            if (str::compare(dir, "undef") == 0) {
                char mname[48];
                usize n = 0;
                while (isIdentChar(*lp) && n + 1 < sizeof(mname)) mname[n++] = *lp++;
                mname[n] = '\0';
                removeMacro(s, mname);
                out.appendChar('\n');
                continue;
            }

            if (str::compare(dir, "include") == 0) {
                char endDelim = (*lp == '<') ? '>' : '"';
                if (*lp == '<' || *lp == '"') ++lp;
                char incName[96];
                usize in = 0;
                while (*lp && *lp != endDelim && in + 1 < sizeof(incName)) {
                    incName[in++] = *lp++;
                }
                incName[in] = '\0';

                // Check #pragma once cache
                bool alreadyIncluded = false;
                for (usize oi = 0; oi < s->onceFileCount; ++oi) {
                    if (str::compare(s->onceFiles[oi], incName) == 0) {
                        alreadyIncluded = true;
                        break;
                    }
                }
                if (alreadyIncluded) {
                    out.appendChar('\n');
                    continue;
                }

                // Try VFS paths first, then built-in fallback headers
                char* incContent = nullptr;
                const proc::Task* t = proc::Scheduler::instance().getCurrentTask();
                const char* cwd     = t ? t->getCwd() : "/";

                if (endDelim == '"') {
                    incContent = readVfsFileToHeap(cwd, incName, nullptr);
                }
                for (usize pi = 0; !incContent && pi < s->includePathCount; ++pi) {
                    char fullPath[160]{};
                    str::copy(fullPath, s->includePaths[pi], sizeof(fullPath));
                    str::concat(fullPath, "/", sizeof(fullPath));
                    str::concat(fullPath, incName, sizeof(fullPath));
                    incContent = readVfsFileToHeap("/", fullPath, nullptr);
                }
                if (!incContent) {
                    const char* builtin = findBuiltinHeader(incName);
                    if (builtin) {
                        preprocessSource(s, incName, builtin, out, includeDepth + 1);
                    } else {
                        reportError(s, 0, "cannot find header '%s'", incName);
                    }
                } else {
                    preprocessSource(s, incName, incContent, out, includeDepth + 1);
                    klibc_free(incContent);
                }
                out.appendChar('\n');
                continue;
            }

            out.appendChar('\n');
            continue;
        }

        if (!isActive()) {
            out.appendChar('\n');
            continue;
        }

        expandMacrosInLine(s, lineBuf, out, 0);
        out.appendChar('\n');
    }

    klibc_free(clean);
}

// =============================================================================
// Tokenizer Implementation
// =============================================================================

void pushToken(TCCState* s, const Token& tok) noexcept {
    if (s->tokenCount + 1 > s->tokenCap) {
        usize newCap = (s->tokenCap == 0) ? 512 : (s->tokenCap * 2);
        auto* p = static_cast<Token*>(klibc_realloc(s->tokens, newCap * sizeof(Token)));
        if (!p) return;
        s->tokens   = p;
        s->tokenCap = newCap;
    }
    s->tokens[s->tokenCount++] = tok;
}

const char* tokenStr(TCCState* s, const Token& tok) noexcept {
    return reinterpret_cast<const char*>(s->stringPool.data + tok.strOff);
}

char decodeEscape(const char*& p) noexcept {
    char c = *p++;
    switch (c) {
        case 'n':  return '\n';
        case 'r':  return '\r';
        case 't':  return '\t';
        case 'b':  return '\b';
        case 'f':  return '\x0C';
        case 'a':  return '\a';
        case 'v':  return '\v';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case 'x': {
            u8 v = 0;
            for (int i = 0; i < 2; ++i) {
                if (*p >= '0' && *p <= '9') v = static_cast<u8>((v << 4) | (*p++ - '0'));
                else if (*p >= 'a' && *p <= 'f') v = static_cast<u8>((v << 4) | (*p++ - 'a' + 10));
                else if (*p >= 'A' && *p <= 'F') v = static_cast<u8>((v << 4) | (*p++ - 'A' + 10));
                else break;
            }
            return static_cast<char>(v);
        }
        default: {
            if (c >= '0' && c <= '7') {
                u8 v = static_cast<u8>(c - '0');
                for (int i = 0; i < 2 && (*p >= '0' && *p <= '7'); ++i) {
                    v = static_cast<u8>((v << 3) | (*p++ - '0'));
                }
                return static_cast<char>(v);
            }
            return c;
        }
    }
}

TokKind classifyKeyword(const char* ident) noexcept {
    if (str::compare(ident, "void") == 0)     return TokKind::KwVoid;
    if (str::compare(ident, "char") == 0)     return TokKind::KwChar;
    if (str::compare(ident, "short") == 0)    return TokKind::KwShort;
    if (str::compare(ident, "int") == 0)      return TokKind::KwInt;
    if (str::compare(ident, "long") == 0)     return TokKind::KwLong;
    if (str::compare(ident, "signed") == 0)   return TokKind::KwSigned;
    if (str::compare(ident, "unsigned") == 0) return TokKind::KwUnsigned;
    if (str::compare(ident, "_Bool") == 0 || str::compare(ident, "bool") == 0) return TokKind::KwBool;
    if (str::compare(ident, "const") == 0)    return TokKind::KwConst;
    if (str::compare(ident, "volatile") == 0) return TokKind::KwVolatile;
    if (str::compare(ident, "static") == 0)   return TokKind::KwStatic;
    if (str::compare(ident, "extern") == 0)   return TokKind::KwExtern;
    if (str::compare(ident, "inline") == 0)   return TokKind::KwInline;
    if (str::compare(ident, "register") == 0) return TokKind::KwRegister;
    if (str::compare(ident, "struct") == 0)   return TokKind::KwStruct;
    if (str::compare(ident, "union") == 0)    return TokKind::KwUnion;
    if (str::compare(ident, "enum") == 0)     return TokKind::KwEnum;
    if (str::compare(ident, "typedef") == 0)  return TokKind::KwTypedef;
    if (str::compare(ident, "sizeof") == 0)   return TokKind::KwSizeof;
    if (str::compare(ident, "if") == 0)       return TokKind::KwIf;
    if (str::compare(ident, "else") == 0)     return TokKind::KwElse;
    if (str::compare(ident, "while") == 0)    return TokKind::KwWhile;
    if (str::compare(ident, "do") == 0)       return TokKind::KwDo;
    if (str::compare(ident, "for") == 0)      return TokKind::KwFor;
    if (str::compare(ident, "switch") == 0)   return TokKind::KwSwitch;
    if (str::compare(ident, "case") == 0)     return TokKind::KwCase;
    if (str::compare(ident, "default") == 0)  return TokKind::KwDefault;
    if (str::compare(ident, "break") == 0)    return TokKind::KwBreak;
    if (str::compare(ident, "continue") == 0) return TokKind::KwContinue;
    if (str::compare(ident, "return") == 0)   return TokKind::KwReturn;
    if (str::compare(ident, "goto") == 0)     return TokKind::KwGoto;
    return TokKind::Ident;
}

void tokenize(TCCState* s, const char* src) noexcept {
    u32 line = 1;
    const char* p = src;

    while (*p) {
        if (*p == '\n') {
            ++line;
            ++p;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            ++p;
            continue;
        }

        // Number literal
        if (*p >= '0' && *p <= '9') {
            char* endp = nullptr;
            u64 v = klibc_strtoul(p, &endp, 0);
            if (endp && endp > p) {
                p = endp;
                while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') ++p;
            } else {
                ++p;
            }
            pushToken(s, Token{ TokKind::Number, line, static_cast<i64>(v), 0, 0 });
            continue;
        }

        // Character literal
        if (*p == '\'') {
            ++p;
            char ch = 0;
            if (*p == '\\') {
                ++p;
                ch = decodeEscape(p);
            } else if (*p) {
                ch = *p++;
            }
            if (*p == '\'') ++p;
            pushToken(s, Token{ TokKind::Number, line, static_cast<u8>(ch), 0, 0 });
            continue;
        }

        // String literal (with adjacent string literal concatenation)
        if (*p == '"') {
            u32 startOff = static_cast<u32>(s->stringPool.size);
            while (*p == '"') {
                ++p;
                while (*p && *p != '"') {
                    if (*p == '\\') {
                        ++p;
                        s->stringPool.appendChar(decodeEscape(p));
                    } else {
                        if (*p == '\n') ++line;
                        s->stringPool.appendChar(*p++);
                    }
                }
                if (*p == '"') ++p;
                // Skip whitespace to check for adjacent string literal
                const char* look = p;
                u32 addLines = 0;
                while (*look == ' ' || *look == '\t' || *look == '\r' || *look == '\n') {
                    if (*look == '\n') ++addLines;
                    ++look;
                }
                if (*look == '"') {
                    line += addLines;
                    p = look;
                }
            }
            u32 strLen = static_cast<u32>(s->stringPool.size - startOff);
            s->stringPool.appendChar('\0');
            pushToken(s, Token{ TokKind::StringLit, line, 0, startOff, strLen });
            continue;
        }

        // Identifier or Keyword
        if (isIdentStart(*p)) {
            u32 startOff = static_cast<u32>(s->stringPool.size);
            while (isIdentChar(*p)) {
                s->stringPool.appendChar(*p++);
            }
            u32 len = static_cast<u32>(s->stringPool.size - startOff);
            s->stringPool.appendChar('\0');
            const char* idStr = reinterpret_cast<const char*>(s->stringPool.data + startOff);
            TokKind k = classifyKeyword(idStr);
            pushToken(s, Token{ k, line, 0, startOff, len });
            continue;
        }

        // Multi-character operators
        if (p[0] == '.' && p[1] == '.' && p[2] == '.') { p += 3; pushToken(s, Token{ TokKind::Ellipsis, line, 0, 0, 0 }); continue; }
        if (p[0] == '<' && p[1] == '<' && p[2] == '=') { p += 3; pushToken(s, Token{ TokKind::ShlAssign, line, 0, 0, 0 }); continue; }
        if (p[0] == '>' && p[1] == '>' && p[2] == '=') { p += 3; pushToken(s, Token{ TokKind::ShrAssign, line, 0, 0, 0 }); continue; }
        if (p[0] == '-' && p[1] == '>') { p += 2; pushToken(s, Token{ TokKind::Arrow, line, 0, 0, 0 }); continue; }
        if (p[0] == '+' && p[1] == '+') { p += 2; pushToken(s, Token{ TokKind::PlusPlus, line, 0, 0, 0 }); continue; }
        if (p[0] == '-' && p[1] == '-') { p += 2; pushToken(s, Token{ TokKind::MinusMinus, line, 0, 0, 0 }); continue; }
        if (p[0] == '<' && p[1] == '<') { p += 2; pushToken(s, Token{ TokKind::Shl, line, 0, 0, 0 }); continue; }
        if (p[0] == '>' && p[1] == '>') { p += 2; pushToken(s, Token{ TokKind::Shr, line, 0, 0, 0 }); continue; }
        if (p[0] == '<' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::Le, line, 0, 0, 0 }); continue; }
        if (p[0] == '>' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::Ge, line, 0, 0, 0 }); continue; }
        if (p[0] == '=' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::Eq, line, 0, 0, 0 }); continue; }
        if (p[0] == '!' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::Ne, line, 0, 0, 0 }); continue; }
        if (p[0] == '&' && p[1] == '&') { p += 2; pushToken(s, Token{ TokKind::AmpAmp, line, 0, 0, 0 }); continue; }
        if (p[0] == '|' && p[1] == '|') { p += 2; pushToken(s, Token{ TokKind::PipePipe, line, 0, 0, 0 }); continue; }
        if (p[0] == '+' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::PlusAssign, line, 0, 0, 0 }); continue; }
        if (p[0] == '-' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::MinusAssign, line, 0, 0, 0 }); continue; }
        if (p[0] == '*' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::StarAssign, line, 0, 0, 0 }); continue; }
        if (p[0] == '/' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::SlashAssign, line, 0, 0, 0 }); continue; }
        if (p[0] == '%' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::PercentAssign, line, 0, 0, 0 }); continue; }
        if (p[0] == '&' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::AmpAssign, line, 0, 0, 0 }); continue; }
        if (p[0] == '^' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::CaretAssign, line, 0, 0, 0 }); continue; }
        if (p[0] == '|' && p[1] == '=') { p += 2; pushToken(s, Token{ TokKind::PipeAssign, line, 0, 0, 0 }); continue; }

        // Single-character tokens
        char c = *p++;
        TokKind sk = TokKind::Eof;
        switch (c) {
            case '(': sk = TokKind::LParen; break;
            case ')': sk = TokKind::RParen; break;
            case '{': sk = TokKind::LBrace; break;
            case '}': sk = TokKind::RBrace; break;
            case '[': sk = TokKind::LBracket; break;
            case ']': sk = TokKind::RBracket; break;
            case ';': sk = TokKind::Semicolon; break;
            case ',': sk = TokKind::Comma; break;
            case '.': sk = TokKind::Dot; break;
            case '?': sk = TokKind::Question; break;
            case ':': sk = TokKind::Colon; break;
            case '+': sk = TokKind::Plus; break;
            case '-': sk = TokKind::Minus; break;
            case '*': sk = TokKind::Star; break;
            case '/': sk = TokKind::Slash; break;
            case '%': sk = TokKind::Percent; break;
            case '&': sk = TokKind::Amp; break;
            case '|': sk = TokKind::Pipe; break;
            case '^': sk = TokKind::Caret; break;
            case '~': sk = TokKind::Tilde; break;
            case '!': sk = TokKind::Bang; break;
            case '<': sk = TokKind::Lt; break;
            case '>': sk = TokKind::Gt; break;
            case '=': sk = TokKind::Assign; break;
            default:
                reportError(s, line, "unexpected character '%c' (0x%02x)", c, static_cast<u8>(c));
                return;
        }
        pushToken(s, Token{ sk, line, 0, 0, 0 });
    }

    pushToken(s, Token{ TokKind::Eof, line, 0, 0, 0 });
}

// =============================================================================
// Parser & Symbol Lookup Helpers
// =============================================================================

const Token& peek(TCCState* s, usize ahead = 0) noexcept {
    usize idx = s->tokPos + ahead;
    if (idx >= s->tokenCount) idx = s->tokenCount - 1;
    s->currentLine = s->tokens[idx].line;
    return s->tokens[idx];
}

const Token& nextTok(TCCState* s) noexcept {
    const Token& t = peek(s, 0);
    if (s->tokPos + 1 < s->tokenCount) ++s->tokPos;
    return t;
}

bool match(TCCState* s, TokKind k) noexcept {
    if (peek(s).kind == k) {
        nextTok(s);
        return true;
    }
    return false;
}

void expect(TCCState* s, TokKind k, const char* what) noexcept {
    if (!match(s, k)) {
        reportError(s, peek(s).line, "expected %s", what);
    }
}

TypedefEntry* findTypedef(TCCState* s, const char* name) noexcept {
    for (usize i = 0; i < s->typedefCount; ++i) {
        if (str::compare(s->typedefs[i].name, name) == 0) {
            return &s->typedefs[i];
        }
    }
    return nullptr;
}

EnumEntry* findEnum(TCCState* s, const char* name) noexcept {
    for (usize i = 0; i < s->enumCount; ++i) {
        if (str::compare(s->enums[i].name, name) == 0) {
            return &s->enums[i];
        }
    }
    return nullptr;
}

LocalVar* findLocal(TCCState* s, const char* name) noexcept {
    for (isize i = static_cast<isize>(s->localCount) - 1; i >= 0; --i) {
        if (str::compare(s->locals[i].name, name) == 0) {
            return &s->locals[i];
        }
    }
    return nullptr;
}

GlobalVar* findGlobal(TCCState* s, const char* name) noexcept {
    for (usize i = 0; i < s->globalCount; ++i) {
        if (str::compare(s->globals[i].name, name) == 0) {
            return &s->globals[i];
        }
    }
    return nullptr;
}

isize findFunction(TCCState* s, const char* name) noexcept {
    for (usize i = 0; i < s->funcCount; ++i) {
        if (str::compare(s->funcs[i].name, name) == 0) {
            return static_cast<isize>(i);
        }
    }
    return -1;
}

isize findExternalSym(TCCState* s, const char* name) noexcept {
    for (usize i = 0; i < s->extSymCount; ++i) {
        if (str::compare(s->extSyms[i].name, name) == 0) {
            return static_cast<isize>(i);
        }
    }
    return -1;
}

bool isTypeStart(TCCState* s, usize ahead = 0) noexcept {
    const Token& t = peek(s, ahead);
    switch (t.kind) {
        case TokKind::KwVoid:
        case TokKind::KwChar:
        case TokKind::KwShort:
        case TokKind::KwInt:
        case TokKind::KwLong:
        case TokKind::KwSigned:
        case TokKind::KwUnsigned:
        case TokKind::KwBool:
        case TokKind::KwConst:
        case TokKind::KwVolatile:
        case TokKind::KwStatic:
        case TokKind::KwExtern:
        case TokKind::KwInline:
        case TokKind::KwRegister:
        case TokKind::KwStruct:
        case TokKind::KwUnion:
        case TokKind::KwEnum:
            return true;
        case TokKind::Ident:
            return findTypedef(s, tokenStr(s, t)) != nullptr;
        default:
            return false;
    }
}

void addReloc(TCCState* s, RelocKind kind, usize patchOffset, usize targetIndex) noexcept {
    if (s->relocCount >= MAX_RELOCS) {
        reportError(s, s->currentLine, "relocation table overflow");
        return;
    }
    s->relocs[s->relocCount++] = RelocEntry{ kind, patchOffset, targetIndex };
}

usize addStringLiteralToData(TCCState* s, const char* strBytes, usize len) noexcept {
    usize off = s->dataVec.size;
    s->dataVec.append(strBytes, len);
    s->dataVec.push8(0);
    return off;
}

// =============================================================================
// Forward Declarations for Recursive Descent Parser & Code Emitter
// =============================================================================

i64   evalConstExpr(TCCState* s) noexcept;
CType parseBaseType(TCCState* s, bool* outIsExtern = nullptr) noexcept;
CType parsePointerSuffix(TCCState* s, CType base) noexcept;
CType parseExpression(TCCState* s) noexcept;
CType parseAssignment(TCCState* s) noexcept;
void  parseStatement(TCCState* s) noexcept;

// =============================================================================
// x86_64 Instruction Emission Helpers
// =============================================================================

void emitLoadImm64(TCCState* s, i64 val) noexcept {
    if (val == 0) {
        // xor eax, eax
        s->codeVec.push8(0x31);
        s->codeVec.push8(0xC0);
    } else if (val >= -2147483648LL && val <= 2147483647LL) {
        // mov rax, sign-extended imm32 (48 C7 C0 imm32)
        s->codeVec.push8(0x48);
        s->codeVec.push8(0xC7);
        s->codeVec.push8(0xC0);
        s->codeVec.push32(static_cast<u32>(val));
    } else {
        // movabs rax, imm64 (48 B8 imm64)
        s->codeVec.push8(0x48);
        s->codeVec.push8(0xB8);
        s->codeVec.push64(static_cast<u64>(val));
    }
}

void emitPushRax(TCCState* s) noexcept {
    s->codeVec.push8(0x50); // push rax
}

void emitPopRcx(TCCState* s) noexcept {
    s->codeVec.push8(0x59); // pop rcx
}

void emitLeaRbpDisp(TCCState* s, i32 disp) noexcept {
    // lea rax, [rbp + disp32] -> 48 8D 85 disp32
    s->codeVec.push8(0x48);
    s->codeVec.push8(0x8D);
    s->codeVec.push8(0x85);
    s->codeVec.push32(static_cast<u32>(disp));
}

void emitLoadFromAddressInRax(TCCState* s, const CType& t) noexcept {
    if (t.isArray || t.base == BaseKind::Struct || t.base == BaseKind::FuncPtr) {
        // Arrays, structs, and function designators decay to their address in rax
        return;
    }
    usize sz = typeSize(s, t);
    if (sz == 1) {
        if (t.isUnsigned || t.base == BaseKind::Bool) {
            // movzx eax, byte [rax] -> 0F B6 00
            s->codeVec.push8(0x0F);
            s->codeVec.push8(0xB6);
            s->codeVec.push8(0x00);
        } else {
            // movsx rax, byte [rax] -> 48 0F BE 00
            s->codeVec.push8(0x48);
            s->codeVec.push8(0x0F);
            s->codeVec.push8(0xBE);
            s->codeVec.push8(0x00);
        }
    } else if (sz == 2) {
        if (t.isUnsigned) {
            // movzx eax, word [rax] -> 0F B7 00
            s->codeVec.push8(0x0F);
            s->codeVec.push8(0xB7);
            s->codeVec.push8(0x00);
        } else {
            // movsx rax, word [rax] -> 48 0F BF 00
            s->codeVec.push8(0x48);
            s->codeVec.push8(0x0F);
            s->codeVec.push8(0xBF);
            s->codeVec.push8(0x00);
        }
    } else if (sz == 4) {
        if (t.isUnsigned) {
            // mov eax, dword [rax] -> 8B 00
            s->codeVec.push8(0x8B);
            s->codeVec.push8(0x00);
        } else {
            // movsxd rax, dword [rax] -> 48 63 00
            s->codeVec.push8(0x48);
            s->codeVec.push8(0x63);
            s->codeVec.push8(0x00);
        }
    } else {
        // mov rax, qword [rax] -> 48 8B 00
        s->codeVec.push8(0x48);
        s->codeVec.push8(0x8B);
        s->codeVec.push8(0x00);
    }
}

void emitStoreRaxToAddressInRcx(TCCState* s, const CType& t) noexcept {
    usize sz = typeSize(s, t);
    if (sz == 1) {
        // mov [rcx], al -> 88 01
        s->codeVec.push8(0x88);
        s->codeVec.push8(0x01);
    } else if (sz == 2) {
        // mov [rcx], ax -> 66 89 01
        s->codeVec.push8(0x66);
        s->codeVec.push8(0x89);
        s->codeVec.push8(0x01);
    } else if (sz == 4) {
        // mov [rcx], eax -> 89 01
        s->codeVec.push8(0x89);
        s->codeVec.push8(0x01);
    } else {
        // mov [rcx], rax -> 48 89 01
        s->codeVec.push8(0x48);
        s->codeVec.push8(0x89);
        s->codeVec.push8(0x01);
    }
}

void emitCastRax(TCCState* s, const CType& target) noexcept {
    if (target.ptrDepth > 0 || target.isArray) return;
    switch (target.base) {
        case BaseKind::Bool:
            // test rax, rax; setne al; movzx eax, al
            s->codeVec.push8(0x48); s->codeVec.push8(0x85); s->codeVec.push8(0xC0);
            s->codeVec.push8(0x0F); s->codeVec.push8(0x95); s->codeVec.push8(0xC0);
            s->codeVec.push8(0x0F); s->codeVec.push8(0xB6); s->codeVec.push8(0xC0);
            break;
        case BaseKind::Char:
            if (target.isUnsigned) {
                s->codeVec.push8(0x0F); s->codeVec.push8(0xB6); s->codeVec.push8(0xC0);
            } else {
                s->codeVec.push8(0x48); s->codeVec.push8(0x0F); s->codeVec.push8(0xBE); s->codeVec.push8(0xC0);
            }
            break;
        case BaseKind::Short:
            if (target.isUnsigned) {
                s->codeVec.push8(0x0F); s->codeVec.push8(0xB7); s->codeVec.push8(0xC0);
            } else {
                s->codeVec.push8(0x48); s->codeVec.push8(0x0F); s->codeVec.push8(0xBF); s->codeVec.push8(0xC0);
            }
            break;
        case BaseKind::Int:
            if (target.isUnsigned) {
                // mov eax, eax -> 89 C0 (zero-extends to 64 bits)
                s->codeVec.push8(0x89); s->codeVec.push8(0xC0);
            } else {
                // movsxd rax, eax -> 48 63 C0
                s->codeVec.push8(0x48); s->codeVec.push8(0x63); s->codeVec.push8(0xC0);
            }
            break;
        default:
            break;
    }
}

usize emitJmpRel32(TCCState* s) noexcept {
    s->codeVec.push8(0xE9);
    usize patch = s->codeVec.size;
    s->codeVec.push32(0);
    return patch;
}

usize emitJzRel32(TCCState* s) noexcept {
    // test rax, rax (48 85 C0) + jz rel32 (0F 84 rel32)
    s->codeVec.push8(0x48);
    s->codeVec.push8(0x85);
    s->codeVec.push8(0xC0);
    s->codeVec.push8(0x0F);
    s->codeVec.push8(0x84);
    usize patch = s->codeVec.size;
    s->codeVec.push32(0);
    return patch;
}

usize emitJnzRel32(TCCState* s) noexcept {
    // test rax, rax (48 85 C0) + jnz rel32 (0F 85 rel32)
    s->codeVec.push8(0x48);
    s->codeVec.push8(0x85);
    s->codeVec.push8(0xC0);
    s->codeVec.push8(0x0F);
    s->codeVec.push8(0x85);
    usize patch = s->codeVec.size;
    s->codeVec.push32(0);
    return patch;
}

void patchRel32Here(TCCState* s, usize patchOffset) noexcept {
    i32 rel = static_cast<i32>(s->codeVec.size - (patchOffset + 4));
    s->codeVec.patch32(patchOffset, static_cast<u32>(rel));
}

void patchRel32To(TCCState* s, usize patchOffset, usize targetOffset) noexcept {
    i32 rel = static_cast<i32>(targetOffset - (patchOffset + 4));
    s->codeVec.patch32(patchOffset, static_cast<u32>(rel));
}

// =============================================================================
// Type & Struct Parsing
// =============================================================================

u16 findOrCreateStruct(TCCState* s, const char* name) noexcept {
    if (name && name[0]) {
        for (u16 i = 0; i < s->structCount; ++i) {
            if (str::compare(s->structs[i].name, name) == 0) {
                return i;
            }
        }
    }
    if (s->structCount >= MAX_STRUCTS) {
        reportError(s, s->currentLine, "too many struct definitions");
        return 0;
    }
    u16 idx = static_cast<u16>(s->structCount++);
    memset(&s->structs[idx], 0, sizeof(StructDef));
    if (name) str::copy(s->structs[idx].name, name, sizeof(s->structs[idx].name));
    s->structs[idx].align = 1;
    return idx;
}

void parseStructBody(TCCState* s, u16 structIdx, bool isUnion) noexcept {
    StructDef& sd = s->structs[structIdx];
    sd.fieldCount = 0;
    sd.size       = 0;
    sd.align      = 1;

    while (peek(s).kind != TokKind::RBrace && peek(s).kind != TokKind::Eof) {
        CType fBase = parseBaseType(s);
        do {
            CType fType = parsePointerSuffix(s, fBase);
            char fName[32]{};

            // Handle function pointer field: ret (*name)(params...)
            if (match(s, TokKind::LParen)) {
                expect(s, TokKind::Star, "'*' in function pointer field");
                if (peek(s).kind == TokKind::Ident) {
                    str::copy(fName, tokenStr(s, nextTok(s)), sizeof(fName));
                }
                expect(s, TokKind::RParen, "')'");
                // Skip parameter list (...)
                if (match(s, TokKind::LParen)) {
                    int depth = 1;
                    while (depth > 0 && peek(s).kind != TokKind::Eof) {
                        if (match(s, TokKind::LParen)) ++depth;
                        else if (match(s, TokKind::RParen)) --depth;
                        else nextTok(s);
                    }
                }
                fType.base     = BaseKind::FuncPtr;
                fType.ptrDepth = 1;
            } else {
                if (peek(s).kind == TokKind::Ident) {
                    str::copy(fName, tokenStr(s, nextTok(s)), sizeof(fName));
                }
                if (match(s, TokKind::LBracket)) {
                    fType.isArray  = true;
                    fType.arrayLen = static_cast<usize>(evalConstExpr(s));
                    expect(s, TokKind::RBracket, "']'");
                }
            }

            usize fSize  = typeSize(s, fType);
            usize fAlign = typeAlign(s, fType);
            if (fAlign > sd.align) sd.align = fAlign;

            usize fOffset = 0;
            if (!isUnion) {
                fOffset = static_cast<usize>(alignUp(sd.size, fAlign));
                sd.size = fOffset + fSize;
            } else {
                if (fSize > sd.size) sd.size = fSize;
            }

            if (sd.fieldCount < MAX_STRUCT_FIELDS) {
                StructField& sf = sd.fields[sd.fieldCount++];
                str::copy(sf.name, fName, sizeof(sf.name));
                sf.type   = fType;
                sf.offset = fOffset;
            }
        } while (match(s, TokKind::Comma));

        expect(s, TokKind::Semicolon, "';' after struct field");
    }
    expect(s, TokKind::RBrace, "'}'");
    sd.size = static_cast<usize>(alignUp(sd.size, sd.align));
}

CType parseBaseType(TCCState* s, bool* outIsExtern) noexcept {
    CType t{};
    t.base       = BaseKind::Int;
    t.isUnsigned = false;
    bool hasSign = false;
    bool hasBase = false;
    int  longCnt = 0;

    if (outIsExtern) *outIsExtern = false;

    for (;;) {
        TokKind k = peek(s).kind;
        if (k == TokKind::KwConst || k == TokKind::KwVolatile ||
            k == TokKind::KwStatic || k == TokKind::KwInline || k == TokKind::KwRegister) {
            nextTok(s);
            continue;
        }
        if (k == TokKind::KwExtern) {
            if (outIsExtern) *outIsExtern = true;
            nextTok(s);
            continue;
        }
        if (k == TokKind::KwUnsigned) {
            t.isUnsigned = true;
            hasSign      = true;
            nextTok(s);
            continue;
        }
        if (k == TokKind::KwSigned) {
            t.isUnsigned = false;
            hasSign      = true;
            nextTok(s);
            continue;
        }
        if (k == TokKind::KwVoid) {
            t.base  = BaseKind::Void;
            hasBase = true;
            nextTok(s);
            continue;
        }
        if (k == TokKind::KwBool) {
            t.base       = BaseKind::Bool;
            t.isUnsigned = true;
            hasBase      = true;
            nextTok(s);
            continue;
        }
        if (k == TokKind::KwChar) {
            t.base  = BaseKind::Char;
            hasBase = true;
            nextTok(s);
            continue;
        }
        if (k == TokKind::KwShort) {
            t.base  = BaseKind::Short;
            hasBase = true;
            nextTok(s);
            continue;
        }
        if (k == TokKind::KwInt) {
            if (longCnt == 0) t.base = BaseKind::Int;
            hasBase = true;
            nextTok(s);
            continue;
        }
        if (k == TokKind::KwLong) {
            ++longCnt;
            t.base  = BaseKind::Long;
            hasBase = true;
            nextTok(s);
            continue;
        }
        if (k == TokKind::KwStruct || k == TokKind::KwUnion) {
            bool isUnion = (k == TokKind::KwUnion);
            nextTok(s);
            char tagName[40]{};
            if (peek(s).kind == TokKind::Ident) {
                str::copy(tagName, tokenStr(s, nextTok(s)), sizeof(tagName));
            }
            u16 sIdx = findOrCreateStruct(s, tagName);
            if (match(s, TokKind::LBrace)) {
                parseStructBody(s, sIdx, isUnion);
            }
            t.base      = BaseKind::Struct;
            t.structIdx = sIdx;
            return t;
        }
        if (k == TokKind::KwEnum) {
            nextTok(s);
            if (peek(s).kind == TokKind::Ident) {
                nextTok(s); // optional enum tag
            }
            if (match(s, TokKind::LBrace)) {
                i64 nextVal = 0;
                while (peek(s).kind != TokKind::RBrace && peek(s).kind != TokKind::Eof) {
                    if (peek(s).kind != TokKind::Ident) break;
                    const char* ename = tokenStr(s, nextTok(s));
                    if (match(s, TokKind::Assign)) {
                        nextVal = evalConstExpr(s);
                    }
                    if (s->enumCount < MAX_ENUMS) {
                        str::copy(s->enums[s->enumCount].name, ename, 40);
                        s->enums[s->enumCount].value = nextVal;
                        ++s->enumCount;
                    }
                    ++nextVal;
                    if (!match(s, TokKind::Comma)) break;
                }
                expect(s, TokKind::RBrace, "'}'");
            }
            t.base = BaseKind::Int;
            return t;
        }
        if (!hasBase && !hasSign && k == TokKind::Ident) {
            TypedefEntry* td = findTypedef(s, tokenStr(s, peek(s)));
            if (td) {
                nextTok(s);
                return td->type;
            }
        }
        break;
    }

    return t;
}

CType parsePointerSuffix(TCCState* s, CType base) noexcept {
    while (match(s, TokKind::Star)) {
        base.ptrDepth++;
        while (peek(s).kind == TokKind::KwConst || peek(s).kind == TokKind::KwVolatile) {
            nextTok(s);
        }
    }
    return base;
}

// =============================================================================
// Constant Expression Evaluator (for array lengths, enums, case labels, globals)
// =============================================================================

i64 evalConstPrimary(TCCState* s) noexcept {
    if (match(s, TokKind::LParen)) {
        if (isTypeStart(s)) {
            CType ct = parsePointerSuffix(s, parseBaseType(s));
            (void)ct;
            expect(s, TokKind::RParen, "')'");
            return evalConstPrimary(s);
        }
        i64 v = evalConstExpr(s);
        expect(s, TokKind::RParen, "')'");
        return v;
    }
    if (match(s, TokKind::Plus))  return evalConstPrimary(s);
    if (match(s, TokKind::Minus)) return -evalConstPrimary(s);
    if (match(s, TokKind::Tilde)) return ~evalConstPrimary(s);
    if (match(s, TokKind::Bang))  return !evalConstPrimary(s);
    if (match(s, TokKind::KwSizeof)) {
        expect(s, TokKind::LParen, "'(' after sizeof");
        CType t = parsePointerSuffix(s, parseBaseType(s));
        expect(s, TokKind::RParen, "')'");
        return static_cast<i64>(typeSize(s, t));
    }
    if (peek(s).kind == TokKind::Number) {
        return nextTok(s).numVal;
    }
    if (peek(s).kind == TokKind::Ident) {
        const char* id = tokenStr(s, nextTok(s));
        EnumEntry* e   = findEnum(s, id);
        if (e) return e->value;
    }
    return 0;
}

i64 evalConstPrec(TCCState* s, int minPrec) noexcept {
    i64 lhs = evalConstPrimary(s);
    for (;;) {
        TokKind k = peek(s).kind;
        int prec  = 0;
        if (k == TokKind::Star || k == TokKind::Slash || k == TokKind::Percent) prec = 10;
        else if (k == TokKind::Plus || k == TokKind::Minus) prec = 9;
        else if (k == TokKind::Shl || k == TokKind::Shr) prec = 8;
        else if (k == TokKind::Lt || k == TokKind::Le || k == TokKind::Gt || k == TokKind::Ge) prec = 7;
        else if (k == TokKind::Eq || k == TokKind::Ne) prec = 6;
        else if (k == TokKind::Amp) prec = 5;
        else if (k == TokKind::Caret) prec = 4;
        else if (k == TokKind::Pipe) prec = 3;
        else if (k == TokKind::AmpAmp) prec = 2;
        else if (k == TokKind::PipePipe) prec = 1;
        if (prec < minPrec || prec == 0) break;

        nextTok(s);
        i64 rhs = evalConstPrec(s, prec + 1);
        switch (k) {
            case TokKind::Star:     lhs = lhs * rhs; break;
            case TokKind::Slash:    lhs = rhs ? (lhs / rhs) : 0; break;
            case TokKind::Percent:  lhs = rhs ? (lhs % rhs) : 0; break;
            case TokKind::Plus:     lhs = lhs + rhs; break;
            case TokKind::Minus:    lhs = lhs - rhs; break;
            case TokKind::Shl:      lhs = lhs << rhs; break;
            case TokKind::Shr:      lhs = lhs >> rhs; break;
            case TokKind::Lt:       lhs = (lhs < rhs); break;
            case TokKind::Le:       lhs = (lhs <= rhs); break;
            case TokKind::Gt:       lhs = (lhs > rhs); break;
            case TokKind::Ge:       lhs = (lhs >= rhs); break;
            case TokKind::Eq:       lhs = (lhs == rhs); break;
            case TokKind::Ne:       lhs = (lhs != rhs); break;
            case TokKind::Amp:      lhs = lhs & rhs; break;
            case TokKind::Caret:    lhs = lhs ^ rhs; break;
            case TokKind::Pipe:     lhs = lhs | rhs; break;
            case TokKind::AmpAmp:   lhs = (lhs && rhs); break;
            case TokKind::PipePipe: lhs = (lhs || rhs); break;
            default: break;
        }
    }
    return lhs;
}

i64 evalConstExpr(TCCState* s) noexcept {
    return evalConstPrec(s, 1);
}

// =============================================================================
// Expression & Lvalue Code Generation
// =============================================================================

struct ExprResult {
    CType type;
    bool  isLvalueAddr; // If true, rax currently holds the memory address of an lvalue of `type`
};

void materializeRvalue(TCCState* s, ExprResult& r) noexcept {
    if (r.isLvalueAddr) {
        if (r.type.isArray) {
            r.type         = decayArrayType(r.type);
            r.isLvalueAddr = false;
        } else {
            emitLoadFromAddressInRax(s, r.type);
            r.isLvalueAddr = false;
        }
    }
}

ExprResult parseUnaryExpr(TCCState* s, bool wantAddressOnly) noexcept;

void emitFunctionCall(TCCState* s, isize directFuncIdx, isize directExtIdx, const CType& retType) noexcept {
    // Parse arguments: evaluate each into rax and push onto the stack
    usize argStartPatches[16]{};
    usize argCount = 0;

    // Wait: if we push arguments left-to-right (arg0 first, argN-1 last), then at the end:
    // For <= 6 args, popping in reverse order (argN-1 .. arg0) puts them directly into
    // the SysV registers (r9, r8, rcx, rdx, rsi, rdi)!
    if (!match(s, TokKind::RParen)) {
        do {
            CType at = parseAssignment(s);
            (void)at;
            emitPushRax(s);
            ++argCount;
        } while (match(s, TokKind::Comma));
        expect(s, TokKind::RParen, "')' after function arguments");
    }
    (void)argStartPatches;

    // Pop up to 6 arguments into registers (in reverse order of push: argCount-1 down to 0)
    // Pop excess arguments (>6) if any
    usize regArgs = (argCount > 6) ? 6 : argCount;
    for (usize extra = argCount; extra > 6; --extra) {
        emitPopRcx(s);
    }

    for (isize i = static_cast<isize>(regArgs) - 1; i >= 0; --i) {
        switch (i) {
            case 0: s->codeVec.push8(0x5F); break; // pop rdi
            case 1: s->codeVec.push8(0x5E); break; // pop rsi
            case 2: s->codeVec.push8(0x5A); break; // pop rdx
            case 3: s->codeVec.push8(0x59); break; // pop rcx
            case 4: s->codeVec.push8(0x41); s->codeVec.push8(0x58); break; // pop r8
            case 5: s->codeVec.push8(0x41); s->codeVec.push8(0x59); break; // pop r9
        }
    }

    if (directFuncIdx >= 0) {
        // xor eax, eax (31 C0) + call rel32 (E8 rel32)
        s->codeVec.push8(0x31);
        s->codeVec.push8(0xC0);
        s->codeVec.push8(0xE8);
        addReloc(s, RelocKind::InternalFuncCall32, s->codeVec.size, static_cast<usize>(directFuncIdx));
        s->codeVec.push32(0);
    } else if (directExtIdx >= 0) {
        // movabs r10, imm64 (49 BA imm64); xor eax, eax (31 C0); call r10 (41 FF D2)
        s->codeVec.push8(0x49);
        s->codeVec.push8(0xBA);
        addReloc(s, RelocKind::ExternalSymAddr64, s->codeVec.size, static_cast<usize>(directExtIdx));
        s->codeVec.push64(0);
        s->codeVec.push8(0x31);
        s->codeVec.push8(0xC0);
        s->codeVec.push8(0x41);
        s->codeVec.push8(0xFF);
        s->codeVec.push8(0xD2);
    } else {
        // Indirect call: callee address was pushed before arguments! Pop into r10 and call r10
        s->codeVec.push8(0x41);
        s->codeVec.push8(0x5A); // pop r10
        s->codeVec.push8(0x31);
        s->codeVec.push8(0xC0); // xor eax, eax
        s->codeVec.push8(0x41);
        s->codeVec.push8(0xFF);
        s->codeVec.push8(0xD2); // call r10
    }

    emitCastRax(s, retType);
}

ExprResult parsePrimaryAndPostfix(TCCState* s, bool wantAddressOnly) noexcept {
    ExprResult res{};
    res.type         = CType{ BaseKind::Int, false, 0, false, 0, 0 };
    res.isLvalueAddr = false;

    const Token& tok = peek(s);
    if (tok.kind == TokKind::Number) {
        i64 v = nextTok(s).numVal;
        emitLoadImm64(s, v);
        res.type = CType{ BaseKind::Long, false, 0, false, 0, 0 };
    } else if (tok.kind == TokKind::StringLit) {
        Token st = nextTok(s);
        const char* strBytes = tokenStr(s, st);
        usize dataOff = addStringLiteralToData(s, strBytes, st.strLen);
        // movabs rax, imm64 (patched to dataVec + dataOff)
        s->codeVec.push8(0x48);
        s->codeVec.push8(0xB8);
        addReloc(s, RelocKind::DataOffsetAddr64, s->codeVec.size, dataOff);
        s->codeVec.push64(0);
        res.type = CType{ BaseKind::Char, false, 1, false, 0, 0 };
    } else if (tok.kind == TokKind::LParen) {
        nextTok(s);
        res.type = parseExpression(s);
        res.isLvalueAddr = false;
        expect(s, TokKind::RParen, "')'");
    } else if (tok.kind == TokKind::Ident) {
        const char* name = tokenStr(s, nextTok(s));

        // Direct function call?
        if (peek(s).kind == TokKind::LParen) {
            isize fIdx = findFunction(s, name);
            isize eIdx = findExternalSym(s, name);
            LocalVar* lv  = findLocal(s, name);
            GlobalVar* gv = findGlobal(s, name);

            if (fIdx >= 0 && !lv) {
                nextTok(s); // consume '('
                CType rt = s->funcs[fIdx].returnType;
                emitFunctionCall(s, fIdx, -1, rt);
                res.type = rt;
            } else if (eIdx >= 0 && !lv) {
                nextTok(s); // consume '('
                CType rt{ BaseKind::Long, false, 0, false, 0, 0 };
                if (fIdx >= 0) rt = s->funcs[fIdx].returnType;
                emitFunctionCall(s, -1, eIdx, rt);
                res.type = rt;
            } else if (lv) {
                emitLeaRbpDisp(s, lv->rbpOffset);
                emitLoadFromAddressInRax(s, lv->type);
                emitPushRax(s); // push callee address
                nextTok(s); // consume '('
                CType rt{ BaseKind::Long, false, 0, false, 0, 0 };
                emitFunctionCall(s, -1, -1, rt);
                res.type = rt;
            } else if (gv) {
                s->codeVec.push8(0x48);
                s->codeVec.push8(0xB8);
                addReloc(s, RelocKind::DataOffsetAddr64, s->codeVec.size, gv->dataOffset);
                s->codeVec.push64(0);
                emitLoadFromAddressInRax(s, gv->type);
                emitPushRax(s);
                nextTok(s);
                CType rt{ BaseKind::Long, false, 0, false, 0, 0 };
                emitFunctionCall(s, -1, -1, rt);
                res.type = rt;
            } else {
                reportError(s, tok.line, "implicit declaration of function '%s'", name);
            }
        } else if (LocalVar* lv = findLocal(s, name)) {
            emitLeaRbpDisp(s, lv->rbpOffset);
            res.type         = lv->type;
            res.isLvalueAddr = true;
        } else if (GlobalVar* gv = findGlobal(s, name)) {
            if (gv->isExtern) {
                isize eIdx = findExternalSym(s, name);
                if (eIdx >= 0) {
                    s->codeVec.push8(0x48);
                    s->codeVec.push8(0xB8);
                    addReloc(s, RelocKind::ExternalSymAddr64, s->codeVec.size, static_cast<usize>(eIdx));
                    s->codeVec.push64(0);
                }
            } else {
                s->codeVec.push8(0x48);
                s->codeVec.push8(0xB8);
                addReloc(s, RelocKind::DataOffsetAddr64, s->codeVec.size, gv->dataOffset);
                s->codeVec.push64(0);
            }
            res.type         = gv->type;
            res.isLvalueAddr = true;
        } else if (EnumEntry* en = findEnum(s, name)) {
            emitLoadImm64(s, en->value);
            res.type = CType{ BaseKind::Int, false, 0, false, 0, 0 };
        } else {
            isize fIdx = findFunction(s, name);
            isize eIdx = findExternalSym(s, name);
            if (fIdx >= 0) {
                // Function designator decays to 64-bit function pointer in rax
                s->codeVec.push8(0x48);
                s->codeVec.push8(0xB8);
                addReloc(s, RelocKind::InternalFuncAddr64, s->codeVec.size, static_cast<usize>(fIdx));
                s->codeVec.push64(0);
                res.type = CType{ BaseKind::FuncPtr, false, 1, false, 0, 0 };
            } else if (eIdx >= 0) {
                s->codeVec.push8(0x48);
                s->codeVec.push8(0xB8);
                addReloc(s, RelocKind::ExternalSymAddr64, s->codeVec.size, static_cast<usize>(eIdx));
                s->codeVec.push64(0);
                res.type = CType{ BaseKind::FuncPtr, false, 1, false, 0, 0 };
            } else {
                reportError(s, tok.line, "undeclared identifier '%s'", name);
            }
        }
    } else {
        reportError(s, tok.line, "unexpected token in expression");
    }

    // Postfix operators: [idx], .field, ->field, (args), ++, --
    for (;;) {
        if (match(s, TokKind::LBracket)) {
            // Array / pointer indexing: base[idx]
            if (!res.type.isArray) {
                materializeRvalue(s, res);
            }
            CType elemType = res.type;
            if (elemType.isArray) {
                elemType.isArray = false;
            } else if (elemType.ptrDepth > 0) {
                elemType.ptrDepth--;
            }
            usize stride = typeSize(s, elemType);
            emitPushRax(s); // save base address

            parseExpression(s); // index in rax
            if (stride > 1) {
                // imul rax, rax, stride -> 48 69 C0 imm32
                s->codeVec.push8(0x48);
                s->codeVec.push8(0x69);
                s->codeVec.push8(0xC0);
                s->codeVec.push32(static_cast<u32>(stride));
            }
            emitPopRcx(s); // base in rcx
            // add rax, rcx -> 48 01 C8
            s->codeVec.push8(0x48);
            s->codeVec.push8(0x01);
            s->codeVec.push8(0xC8);

            expect(s, TokKind::RBracket, "']'");
            res.type         = elemType;
            res.isLvalueAddr = true;
            continue;
        }

        if (match(s, TokKind::Dot) || match(s, TokKind::Arrow)) {
            TokKind op = s->tokens[s->tokPos - 1].kind;
            if (op == TokKind::Arrow) {
                materializeRvalue(s, res);
            }
            if (peek(s).kind != TokKind::Ident) {
                reportError(s, peek(s).line, "expected field name after '%s'", (op == TokKind::Arrow) ? "->" : ".");
                break;
            }
            const char* fieldName = tokenStr(s, nextTok(s));
            u16 sIdx = res.type.structIdx;
            if (sIdx >= s->structCount) {
                reportError(s, s->currentLine, "member access on non-struct type");
                break;
            }
            const StructDef& sd = s->structs[sIdx];
            const StructField* found = nullptr;
            for (usize fi = 0; fi < sd.fieldCount; ++fi) {
                if (str::compare(sd.fields[fi].name, fieldName) == 0) {
                    found = &sd.fields[fi];
                    break;
                }
            }
            if (!found) {
                reportError(s, s->currentLine, "struct has no member named '%s'", fieldName);
                break;
            }
            if (found->offset > 0) {
                // add rax, offset -> 48 05 imm32
                s->codeVec.push8(0x48);
                s->codeVec.push8(0x05);
                s->codeVec.push32(static_cast<u32>(found->offset));
            }
            res.type         = found->type;
            res.isLvalueAddr = true;
            continue;
        }

        if (match(s, TokKind::LParen)) {
            // Indirect call on postfix expression (e.g., ops->read(ctx, buf, n))
            if (res.isLvalueAddr) {
                // Load function pointer from [rax] into rax
                s->codeVec.push8(0x48);
                s->codeVec.push8(0x8B);
                s->codeVec.push8(0x00);
                res.isLvalueAddr = false;
            }
            emitPushRax(s);
            CType rt{ BaseKind::Long, false, 0, false, 0, 0 };
            emitFunctionCall(s, -1, -1, rt);
            res.type         = rt;
            res.isLvalueAddr = false;
            continue;
        }

        if (match(s, TokKind::PlusPlus) || match(s, TokKind::MinusMinus)) {
            bool isInc = (s->tokens[s->tokPos - 1].kind == TokKind::PlusPlus);
            if (!res.isLvalueAddr) {
                reportError(s, s->currentLine, "lvalue required for post-%s", isInc ? "increment" : "decrement");
                break;
            }
            usize step = (res.type.ptrDepth > 0) ? derefStride(s, res.type) : 1;
            // rcx = address (mov rcx, rax -> 48 89 C1)
            s->codeVec.push8(0x48); s->codeVec.push8(0x89); s->codeVec.push8(0xC1);
            emitLoadFromAddressInRax(s, res.type);
            // Save original value in rdx (mov rdx, rax -> 48 89 C2)
            s->codeVec.push8(0x48); s->codeVec.push8(0x89); s->codeVec.push8(0xC2);
            if (isInc) {
                s->codeVec.push8(0x48); s->codeVec.push8(0x05); s->codeVec.push32(static_cast<u32>(step));
            } else {
                s->codeVec.push8(0x48); s->codeVec.push8(0x2D); s->codeVec.push32(static_cast<u32>(step));
            }
            emitStoreRaxToAddressInRcx(s, res.type);
            // Restore original value to rax (mov rax, rdx -> 48 89 D0)
            s->codeVec.push8(0x48); s->codeVec.push8(0x89); s->codeVec.push8(0xD0);
            res.isLvalueAddr = false;
            continue;
        }

        break;
    }

    if (!wantAddressOnly) {
        materializeRvalue(s, res);
    }
    return res;
}

ExprResult parseUnaryExpr(TCCState* s, bool wantAddressOnly) noexcept {
    // Type cast: (Type)expr
    if (peek(s).kind == TokKind::LParen && isTypeStart(s, 1)) {
        nextTok(s); // '('
        CType castType = parsePointerSuffix(s, parseBaseType(s));
        expect(s, TokKind::RParen, "')' after cast type");
        ExprResult sub = parseUnaryExpr(s, false);
        (void)sub;
        emitCastRax(s, castType);
        return ExprResult{ castType, false };
    }

    if (match(s, TokKind::KwSizeof)) {
        if (peek(s).kind == TokKind::LParen && isTypeStart(s, 1)) {
            nextTok(s);
            CType st = parsePointerSuffix(s, parseBaseType(s));
            if (match(s, TokKind::LBracket)) {
                st.isArray  = true;
                st.arrayLen = static_cast<usize>(evalConstExpr(s));
                expect(s, TokKind::RBracket, "']'");
            }
            expect(s, TokKind::RParen, "')'");
            emitLoadImm64(s, static_cast<i64>(typeSize(s, st)));
        } else {
            // sizeof(expr): parse in dry-run by saving codeVec.size!
            usize savedCode = s->codeVec.size;
            usize savedRel  = s->relocCount;
            bool hasParen   = match(s, TokKind::LParen);
            ExprResult er   = parseUnaryExpr(s, true);
            if (hasParen) expect(s, TokKind::RParen, "')'");
            s->codeVec.size = savedCode;
            s->relocCount   = savedRel;
            emitLoadImm64(s, static_cast<i64>(typeSize(s, er.type)));
        }
        return ExprResult{ CType{ BaseKind::Long, true, 0, false, 0, 0 }, false };
    }

    if (match(s, TokKind::PlusPlus) || match(s, TokKind::MinusMinus)) {
        bool isInc = (s->tokens[s->tokPos - 1].kind == TokKind::PlusPlus);
        ExprResult sub = parseUnaryExpr(s, true);
        if (!sub.isLvalueAddr) {
            reportError(s, s->currentLine, "lvalue required for pre-%s", isInc ? "increment" : "decrement");
            return sub;
        }
        usize step = (sub.type.ptrDepth > 0) ? derefStride(s, sub.type) : 1;
        // mov rcx, rax
        s->codeVec.push8(0x48); s->codeVec.push8(0x89); s->codeVec.push8(0xC1);
        emitLoadFromAddressInRax(s, sub.type);
        if (isInc) {
            s->codeVec.push8(0x48); s->codeVec.push8(0x05); s->codeVec.push32(static_cast<u32>(step));
        } else {
            s->codeVec.push8(0x48); s->codeVec.push8(0x2D); s->codeVec.push32(static_cast<u32>(step));
        }
        emitStoreRaxToAddressInRcx(s, sub.type);
        sub.isLvalueAddr = false;
        return sub;
    }

    if (match(s, TokKind::Amp)) {
        ExprResult sub = parseUnaryExpr(s, true);
        CType addrType = decayArrayType(sub.type);
        if (!sub.type.isArray) addrType.ptrDepth++;
        return ExprResult{ addrType, false };
    }

    if (match(s, TokKind::Star)) {
        ExprResult sub = parseUnaryExpr(s, false);
        CType deref = decayArrayType(sub.type);
        if (deref.ptrDepth > 0) deref.ptrDepth--;
        ExprResult res{ deref, true };
        if (!wantAddressOnly) {
            materializeRvalue(s, res);
        }
        return res;
    }

    if (match(s, TokKind::Plus)) {
        return parseUnaryExpr(s, false);
    }

    if (match(s, TokKind::Minus)) {
        ExprResult sub = parseUnaryExpr(s, false);
        // neg rax -> 48 F7 D8
        s->codeVec.push8(0x48); s->codeVec.push8(0xF7); s->codeVec.push8(0xD8);
        return sub;
    }

    if (match(s, TokKind::Tilde)) {
        ExprResult sub = parseUnaryExpr(s, false);
        // not rax -> 48 F7 D0
        s->codeVec.push8(0x48); s->codeVec.push8(0xF7); s->codeVec.push8(0xD0);
        emitCastRax(s, sub.type);
        return sub;
    }

    if (match(s, TokKind::Bang)) {
        ExprResult sub = parseUnaryExpr(s, false);
        (void)sub;
        // test rax, rax; sete al; movzx eax, al
        s->codeVec.push8(0x48); s->codeVec.push8(0x85); s->codeVec.push8(0xC0);
        s->codeVec.push8(0x0F); s->codeVec.push8(0x94); s->codeVec.push8(0xC0);
        s->codeVec.push8(0x0F); s->codeVec.push8(0xB6); s->codeVec.push8(0xC0);
        return ExprResult{ CType{ BaseKind::Int, false, 0, false, 0, 0 }, false };
    }

    return parsePrimaryAndPostfix(s, wantAddressOnly);
}

int binaryPrecedence(TokKind k) noexcept {
    switch (k) {
        case TokKind::Star:
        case TokKind::Slash:
        case TokKind::Percent:  return 10;
        case TokKind::Plus:
        case TokKind::Minus:    return 9;
        case TokKind::Shl:
        case TokKind::Shr:      return 8;
        case TokKind::Lt:
        case TokKind::Le:
        case TokKind::Gt:
        case TokKind::Ge:       return 7;
        case TokKind::Eq:
        case TokKind::Ne:       return 6;
        case TokKind::Amp:      return 5;
        case TokKind::Caret:    return 4;
        case TokKind::Pipe:     return 3;
        case TokKind::AmpAmp:   return 2;
        case TokKind::PipePipe: return 1;
        default:                return 0;
    }
}

void emitBinaryOp(TCCState* s, TokKind op, CType& lhsType, const CType& rhsType) noexcept {
    // At entry: lhs was pushed on stack, rhs is in rax.
    // Pop lhs into rcx; move rhs into rdx; move lhs into rax -> rax = lhs, rcx = rhs!
    // Wait: pop rcx (rcx = lhs); xchg rax, rcx (48 91 -> rax = lhs, rcx = rhs)!
    emitPopRcx(s);
    s->codeVec.push8(0x48);
    s->codeVec.push8(0x91); // xchg rax, rcx -> rax = lhs, rcx = rhs

    bool isUnsigned = lhsType.isUnsigned || rhsType.isUnsigned || lhsType.ptrDepth > 0 || rhsType.ptrDepth > 0;

    switch (op) {
        case TokKind::Plus: {
            if (lhsType.ptrDepth > 0 && rhsType.ptrDepth == 0) {
                usize stride = derefStride(s, lhsType);
                if (stride > 1) {
                    // imul rcx, rcx, stride -> 48 69 C9 imm32
                    s->codeVec.push8(0x48); s->codeVec.push8(0x69); s->codeVec.push8(0xC9);
                    s->codeVec.push32(static_cast<u32>(stride));
                }
            }
            // add rax, rcx -> 48 01 C8
            s->codeVec.push8(0x48); s->codeVec.push8(0x01); s->codeVec.push8(0xC8);
            break;
        }
        case TokKind::Minus: {
            if (lhsType.ptrDepth > 0 && rhsType.ptrDepth == 0) {
                usize stride = derefStride(s, lhsType);
                if (stride > 1) {
                    s->codeVec.push8(0x48); s->codeVec.push8(0x69); s->codeVec.push8(0xC9);
                    s->codeVec.push32(static_cast<u32>(stride));
                }
                // sub rax, rcx -> 48 29 C8
                s->codeVec.push8(0x48); s->codeVec.push8(0x29); s->codeVec.push8(0xC8);
            } else if (lhsType.ptrDepth > 0 && rhsType.ptrDepth > 0) {
                usize stride = derefStride(s, lhsType);
                s->codeVec.push8(0x48); s->codeVec.push8(0x29); s->codeVec.push8(0xC8);
                if (stride > 1) {
                    emitLoadImm64(s, 0); // Wait: divide rax by stride
                }
                lhsType = CType{ BaseKind::Long, false, 0, false, 0, 0 };
            } else {
                s->codeVec.push8(0x48); s->codeVec.push8(0x29); s->codeVec.push8(0xC8);
            }
            break;
        }
        case TokKind::Star:
            // imul rax, rcx -> 48 0F AF C1
            s->codeVec.push8(0x48); s->codeVec.push8(0x0F); s->codeVec.push8(0xAF); s->codeVec.push8(0xC1);
            break;
        case TokKind::Slash:
        case TokKind::Percent: {
            if (isUnsigned) {
                // xor edx, edx (31 D2); div rcx (48 F7 F1)
                s->codeVec.push8(0x31); s->codeVec.push8(0xD2);
                s->codeVec.push8(0x48); s->codeVec.push8(0xF7); s->codeVec.push8(0xF1);
            } else {
                // cqo (48 99); idiv rcx (48 F7 F9)
                s->codeVec.push8(0x48); s->codeVec.push8(0x99);
                s->codeVec.push8(0x48); s->codeVec.push8(0xF7); s->codeVec.push8(0xF9);
            }
            if (op == TokKind::Percent) {
                // mov rax, rdx -> 48 89 D0
                s->codeVec.push8(0x48); s->codeVec.push8(0x89); s->codeVec.push8(0xD0);
            }
            break;
        }
        case TokKind::Shl:
            // shl rax, cl -> 48 D3 E0
            s->codeVec.push8(0x48); s->codeVec.push8(0xD3); s->codeVec.push8(0xE0);
            break;
        case TokKind::Shr:
            if (isUnsigned) {
                // shr rax, cl -> 48 D3 E8
                s->codeVec.push8(0x48); s->codeVec.push8(0xD3); s->codeVec.push8(0xE8);
            } else {
                // sar rax, cl -> 48 D3 F8
                s->codeVec.push8(0x48); s->codeVec.push8(0xD3); s->codeVec.push8(0xF8);
            }
            break;
        case TokKind::Amp:
            // and rax, rcx -> 48 21 C8
            s->codeVec.push8(0x48); s->codeVec.push8(0x21); s->codeVec.push8(0xC8);
            break;
        case TokKind::Caret:
            // xor rax, rcx -> 48 31 C8
            s->codeVec.push8(0x48); s->codeVec.push8(0x31); s->codeVec.push8(0xC8);
            break;
        case TokKind::Pipe:
            // or rax, rcx -> 48 09 C8
            s->codeVec.push8(0x48); s->codeVec.push8(0x09); s->codeVec.push8(0xC8);
            break;
        case TokKind::Eq:
        case TokKind::Ne:
        case TokKind::Lt:
        case TokKind::Le:
        case TokKind::Gt:
        case TokKind::Ge: {
            // cmp rax, rcx -> 48 39 C8
            s->codeVec.push8(0x48); s->codeVec.push8(0x39); s->codeVec.push8(0xC8);
            u8 setcc = 0x94;
            if (op == TokKind::Eq) setcc = 0x94;
            else if (op == TokKind::Ne) setcc = 0x95;
            else if (op == TokKind::Lt) setcc = isUnsigned ? 0x92 : 0x9C; // setb / setl
            else if (op == TokKind::Le) setcc = isUnsigned ? 0x96 : 0x9E; // setbe / setle
            else if (op == TokKind::Gt) setcc = isUnsigned ? 0x97 : 0x9F; // seta / setg
            else if (op == TokKind::Ge) setcc = isUnsigned ? 0x93 : 0x9D; // setae / setge
            s->codeVec.push8(0x0F); s->codeVec.push8(setcc); s->codeVec.push8(0xC0);
            s->codeVec.push8(0x0F); s->codeVec.push8(0xB6); s->codeVec.push8(0xC0); // movzx eax, al
            lhsType = CType{ BaseKind::Int, false, 0, false, 0, 0 };
            break;
        }
        default:
            break;
    }
}

CType parseBinaryPrec(TCCState* s, int minPrec) noexcept {
    ExprResult lhs = parseUnaryExpr(s, false);
    CType lhsType  = lhs.type;

    for (;;) {
        TokKind op = peek(s).kind;
        int prec   = binaryPrecedence(op);
        if (prec < minPrec || prec == 0) break;

        nextTok(s);

        if (op == TokKind::AmpAmp) {
            usize jFalse = emitJzRel32(s);
            (void)parseBinaryPrec(s, prec + 1);
            // test rax, rax; setne al; movzx eax, al
            s->codeVec.push8(0x48); s->codeVec.push8(0x85); s->codeVec.push8(0xC0);
            s->codeVec.push8(0x0F); s->codeVec.push8(0x95); s->codeVec.push8(0xC0);
            s->codeVec.push8(0x0F); s->codeVec.push8(0xB6); s->codeVec.push8(0xC0);
            usize jEnd = emitJmpRel32(s);
            patchRel32Here(s, jFalse);
            emitLoadImm64(s, 0);
            patchRel32Here(s, jEnd);
            lhsType = CType{ BaseKind::Int, false, 0, false, 0, 0 };
            continue;
        }

        if (op == TokKind::PipePipe) {
            usize jTrue = emitJnzRel32(s);
            (void)parseBinaryPrec(s, prec + 1);
            s->codeVec.push8(0x48); s->codeVec.push8(0x85); s->codeVec.push8(0xC0);
            s->codeVec.push8(0x0F); s->codeVec.push8(0x95); s->codeVec.push8(0xC0);
            s->codeVec.push8(0x0F); s->codeVec.push8(0xB6); s->codeVec.push8(0xC0);
            usize jEnd = emitJmpRel32(s);
            patchRel32Here(s, jTrue);
            emitLoadImm64(s, 1);
            patchRel32Here(s, jEnd);
            lhsType = CType{ BaseKind::Int, false, 0, false, 0, 0 };
            continue;
        }

        emitPushRax(s);
        CType rhsType = parseBinaryPrec(s, prec + 1);
        emitBinaryOp(s, op, lhsType, rhsType);
    }

    return lhsType;
}

CType parseConditional(TCCState* s) noexcept {
    CType t = parseBinaryPrec(s, 1);
    if (match(s, TokKind::Question)) {
        usize jFalse = emitJzRel32(s);
        CType thenType = parseExpression(s);
        usize jEnd = emitJmpRel32(s);
        expect(s, TokKind::Colon, "':' in ternary expression");
        patchRel32Here(s, jFalse);
        (void)parseConditional(s);
        patchRel32Here(s, jEnd);
        return thenType;
    }
    return t;
}

bool isAssignOp(TokKind k) noexcept {
    switch (k) {
        case TokKind::Assign:
        case TokKind::PlusAssign:
        case TokKind::MinusAssign:
        case TokKind::StarAssign:
        case TokKind::SlashAssign:
        case TokKind::PercentAssign:
        case TokKind::ShlAssign:
        case TokKind::ShrAssign:
        case TokKind::AmpAssign:
        case TokKind::CaretAssign:
        case TokKind::PipeAssign:
            return true;
        default:
            return false;
    }
}

CType parseAssignment(TCCState* s) noexcept {
    usize saveTok  = s->tokPos;
    usize saveCode = s->codeVec.size;
    usize saveRel  = s->relocCount;

    ExprResult lhs = parseUnaryExpr(s, true);
    if (lhs.isLvalueAddr && isAssignOp(peek(s).kind)) {
        TokKind aop = nextTok(s).kind;
        emitPushRax(s); // push lvalue address

        if (aop == TokKind::Assign) {
            (void)parseAssignment(s);
            emitPopRcx(s);
            emitStoreRaxToAddressInRcx(s, lhs.type);
            return lhs.type;
        }

        // Compound assignment: load current value from [rax], push it, evaluate rhs, apply op, store to lvalue address
        emitLoadFromAddressInRax(s, lhs.type);
        emitPushRax(s);
        CType rhsType = parseAssignment(s);
        TokKind binOp = TokKind::Plus;
        switch (aop) {
            case TokKind::PlusAssign:    binOp = TokKind::Plus; break;
            case TokKind::MinusAssign:   binOp = TokKind::Minus; break;
            case TokKind::StarAssign:    binOp = TokKind::Star; break;
            case TokKind::SlashAssign:   binOp = TokKind::Slash; break;
            case TokKind::PercentAssign: binOp = TokKind::Percent; break;
            case TokKind::ShlAssign:     binOp = TokKind::Shl; break;
            case TokKind::ShrAssign:     binOp = TokKind::Shr; break;
            case TokKind::AmpAssign:     binOp = TokKind::Amp; break;
            case TokKind::CaretAssign:   binOp = TokKind::Caret; break;
            case TokKind::PipeAssign:    binOp = TokKind::Pipe; break;
            default: break;
        }
        CType valType = lhs.type;
        emitBinaryOp(s, binOp, valType, rhsType);
        emitPopRcx(s); // pop lvalue address
        emitStoreRaxToAddressInRcx(s, lhs.type);
        return lhs.type;
    }

    // Rewind and parse as normal conditional rvalue expression
    s->tokPos       = saveTok;
    s->codeVec.size = saveCode;
    s->relocCount   = saveRel;
    return parseConditional(s);
}

CType parseExpression(TCCState* s) noexcept {
    CType t = parseAssignment(s);
    while (match(s, TokKind::Comma)) {
        t = parseAssignment(s);
    }
    return t;
}

// =============================================================================
// Local Variable Allocation & Statements
// =============================================================================

LocalVar* allocateLocal(TCCState* s, const char* name, const CType& t) noexcept {
    usize sz    = typeSize(s, t);
    usize align = typeAlign(s, t);
    if (align < 8) align = 8;

    s->curStackOffset += static_cast<i32>(alignUp(sz, align));
    if (s->curStackOffset > s->maxStackOffset) {
        s->maxStackOffset = s->curStackOffset;
    }

    if (s->localCount >= MAX_LOCALS) {
        reportError(s, s->currentLine, "too many local variables in function");
        return &s->locals[0];
    }

    LocalVar& lv  = s->locals[s->localCount++];
    str::copy(lv.name, name, sizeof(lv.name));
    lv.type       = t;
    lv.rbpOffset  = -s->curStackOffset;
    lv.scopeDepth = s->scopeDepth;
    return &lv;
}

void emitZeroLocalRegion(TCCState* s, i32 rbpOffset, usize byteSize) noexcept {
    usize qwords = byteSize / 8;
    emitLoadImm64(s, 0);
    for (usize q = 0; q < qwords; ++q) {
        i32 disp = rbpOffset + static_cast<i32>(q * 8);
        // mov [rbp + disp32], rax -> 48 89 85 disp32
        s->codeVec.push8(0x48);
        s->codeVec.push8(0x89);
        s->codeVec.push8(0x85);
        s->codeVec.push32(static_cast<u32>(disp));
    }
    for (usize b = qwords * 8; b < byteSize; ++b) {
        i32 disp = rbpOffset + static_cast<i32>(b);
        s->codeVec.push8(0x88);
        s->codeVec.push8(0x85);
        s->codeVec.push32(static_cast<u32>(disp));
    }
}

void parseLocalDeclaration(TCCState* s) noexcept {
    if (match(s, TokKind::KwTypedef)) {
        CType base = parseBaseType(s);
        CType full = parsePointerSuffix(s, base);
        if (peek(s).kind == TokKind::Ident) {
            const char* tdName = tokenStr(s, nextTok(s));
            if (s->typedefCount < MAX_TYPEDEFS) {
                str::copy(s->typedefs[s->typedefCount].name, tdName, 40);
                s->typedefs[s->typedefCount].type = full;
                ++s->typedefCount;
            }
        }
        expect(s, TokKind::Semicolon, "';'");
        return;
    }

    CType base = parseBaseType(s);
    if (match(s, TokKind::Semicolon)) {
        return; // Standalone struct/enum declaration
    }

    do {
        CType vType = parsePointerSuffix(s, base);
        if (peek(s).kind != TokKind::Ident) {
            reportError(s, peek(s).line, "expected variable name in declaration");
            break;
        }
        const char* vName = tokenStr(s, nextTok(s));

        bool unsizedArray = false;
        if (match(s, TokKind::LBracket)) {
            vType.isArray = true;
            if (peek(s).kind == TokKind::RBracket) {
                unsizedArray   = true;
                vType.arrayLen = 1;
            } else {
                vType.arrayLen = static_cast<usize>(evalConstExpr(s));
            }
            expect(s, TokKind::RBracket, "']'");
        }

        // Check for initializer before allocating stack if unsized string array
        if (unsizedArray && peek(s).kind == TokKind::Assign && peek(s, 1).kind == TokKind::StringLit) {
            vType.arrayLen = peek(s, 1).strLen + 1;
        }

        LocalVar* lv = allocateLocal(s, vName, vType);

        if (match(s, TokKind::Assign)) {
            if (peek(s).kind == TokKind::LBrace) {
                nextTok(s); // '{'
                emitZeroLocalRegion(s, lv->rbpOffset, typeSize(s, vType));
                if (vType.base == BaseKind::Struct && vType.ptrDepth == 0 && !vType.isArray) {
                    const StructDef& sd = s->structs[vType.structIdx];
                    usize fieldIdx = 0;
                    while (peek(s).kind != TokKind::RBrace && peek(s).kind != TokKind::Eof) {
                        const StructField* targetField = nullptr;
                        if (match(s, TokKind::Dot)) {
                            const char* fname = tokenStr(s, nextTok(s));
                            expect(s, TokKind::Assign, "'=' after designated field");
                            for (usize fi = 0; fi < sd.fieldCount; ++fi) {
                                if (str::compare(sd.fields[fi].name, fname) == 0) {
                                    targetField = &sd.fields[fi];
                                    break;
                                }
                            }
                        } else if (fieldIdx < sd.fieldCount) {
                            targetField = &sd.fields[fieldIdx++];
                        }

                        parseAssignment(s); // value in rax
                        if (targetField) {
                            emitLeaRbpDisp(s, lv->rbpOffset + static_cast<i32>(targetField->offset));
                            // Wait: emitLeaRbpDisp clobbers rax! Let's use rcx directly: lea rcx, [rbp + disp32] (48 8D 8D disp32)
                            s->codeVec.size -= 7; // undo emitLeaRbpDisp
                            s->codeVec.push8(0x48);
                            s->codeVec.push8(0x8D);
                            s->codeVec.push8(0x8D);
                            s->codeVec.push32(static_cast<u32>(lv->rbpOffset + static_cast<i32>(targetField->offset)));
                            emitStoreRaxToAddressInRcx(s, targetField->type);
                        }
                        if (!match(s, TokKind::Comma)) break;
                    }
                } else if (vType.isArray) {
                    CType elemType   = vType;
                    elemType.isArray = false;
                    usize elemStride = typeSize(s, elemType);
                    usize idx        = 0;
                    while (peek(s).kind != TokKind::RBrace && peek(s).kind != TokKind::Eof) {
                        parseAssignment(s);
                        if (idx < vType.arrayLen) {
                            s->codeVec.push8(0x48);
                            s->codeVec.push8(0x8D);
                            s->codeVec.push8(0x8D);
                            s->codeVec.push32(static_cast<u32>(lv->rbpOffset + static_cast<i32>(idx * elemStride)));
                            emitStoreRaxToAddressInRcx(s, elemType);
                        }
                        ++idx;
                        if (!match(s, TokKind::Comma)) break;
                    }
                } else {
                    if (peek(s).kind != TokKind::RBrace) {
                        parseAssignment(s);
                        s->codeVec.push8(0x48);
                        s->codeVec.push8(0x8D);
                        s->codeVec.push8(0x8D);
                        s->codeVec.push32(static_cast<u32>(lv->rbpOffset));
                        emitStoreRaxToAddressInRcx(s, vType);
                    }
                }
                expect(s, TokKind::RBrace, "'}'");
            } else if (vType.isArray && vType.base == BaseKind::Char && peek(s).kind == TokKind::StringLit) {
                Token st = nextTok(s);
                const char* strBytes = tokenStr(s, st);
                emitZeroLocalRegion(s, lv->rbpOffset, typeSize(s, vType));
                for (usize ci = 0; ci < st.strLen && ci + 1 < vType.arrayLen; ++ci) {
                    s->codeVec.push8(0xC6);
                    s->codeVec.push8(0x85);
                    s->codeVec.push32(static_cast<u32>(lv->rbpOffset + static_cast<i32>(ci)));
                    s->codeVec.push8(static_cast<u8>(strBytes[ci]));
                }
            } else {
                parseAssignment(s);
                // lea rcx, [rbp + disp32] (48 8D 8D disp32)
                s->codeVec.push8(0x48);
                s->codeVec.push8(0x8D);
                s->codeVec.push8(0x8D);
                s->codeVec.push32(static_cast<u32>(lv->rbpOffset));
                emitStoreRaxToAddressInRcx(s, vType);
            }
        }
    } while (match(s, TokKind::Comma));

    expect(s, TokKind::Semicolon, "';' after declaration");
}

void parseBlock(TCCState* s) noexcept {
    usize savedLocals = s->localCount;
    ++s->scopeDepth;
    while (peek(s).kind != TokKind::RBrace && peek(s).kind != TokKind::Eof) {
        if (isTypeStart(s) || peek(s).kind == TokKind::KwTypedef) {
            parseLocalDeclaration(s);
        } else {
            parseStatement(s);
        }
    }
    expect(s, TokKind::RBrace, "'}'");
    --s->scopeDepth;
    s->localCount = savedLocals;
}

void parseStatement(TCCState* s) noexcept {
    if (match(s, TokKind::Semicolon)) {
        return;
    }

    if (match(s, TokKind::LBrace)) {
        parseBlock(s);
        return;
    }

    if (isTypeStart(s) || peek(s).kind == TokKind::KwTypedef) {
        parseLocalDeclaration(s);
        return;
    }

    // Label: ident ':'
    if (peek(s).kind == TokKind::Ident && peek(s, 1).kind == TokKind::Colon) {
        const char* lname = tokenStr(s, nextTok(s));
        nextTok(s); // ':'
        if (s->labelCount < MAX_LABELS) {
            str::copy(s->labels[s->labelCount].name, lname, 32);
            s->labels[s->labelCount].codeOffset = s->codeVec.size;
            ++s->labelCount;
        }
        parseStatement(s);
        return;
    }

    if (match(s, TokKind::KwGoto)) {
        if (peek(s).kind == TokKind::Ident) {
            const char* lname = tokenStr(s, nextTok(s));
            usize patch = emitJmpRel32(s);
            if (s->gotoCount < MAX_GOTOS) {
                str::copy(s->gotos[s->gotoCount].name, lname, 32);
                s->gotos[s->gotoCount].codeOffset = patch;
                ++s->gotoCount;
            }
        }
        expect(s, TokKind::Semicolon, "';' after goto");
        return;
    }

    if (match(s, TokKind::KwIf)) {
        expect(s, TokKind::LParen, "'(' after if");
        parseExpression(s);
        expect(s, TokKind::RParen, "')' after if condition");
        usize jFalse = emitJzRel32(s);
        parseStatement(s);
        if (match(s, TokKind::KwElse)) {
            usize jEnd = emitJmpRel32(s);
            patchRel32Here(s, jFalse);
            parseStatement(s);
            patchRel32Here(s, jEnd);
        } else {
            patchRel32Here(s, jFalse);
        }
        return;
    }

    if (match(s, TokKind::KwWhile)) {
        usize savedBreak = s->breakCount;
        usize savedCont  = s->continueCount;
        usize loopTop    = s->codeVec.size;

        expect(s, TokKind::LParen, "'(' after while");
        parseExpression(s);
        expect(s, TokKind::RParen, "')'");
        usize jExit = emitJzRel32(s);

        parseStatement(s);

        for (usize i = savedCont; i < s->continueCount; ++i) {
            patchRel32Here(s, s->continuePatches[i]);
        }
        s->continueCount = savedCont;

        usize jBack = emitJmpRel32(s);
        patchRel32To(s, jBack, loopTop);
        patchRel32Here(s, jExit);

        for (usize i = savedBreak; i < s->breakCount; ++i) {
            patchRel32Here(s, s->breakPatches[i]);
        }
        s->breakCount = savedBreak;
        return;
    }

    if (match(s, TokKind::KwDo)) {
        usize savedBreak = s->breakCount;
        usize savedCont  = s->continueCount;
        usize loopTop    = s->codeVec.size;

        parseStatement(s);

        for (usize i = savedCont; i < s->continueCount; ++i) {
            patchRel32Here(s, s->continuePatches[i]);
        }
        s->continueCount = savedCont;

        expect(s, TokKind::KwWhile, "'while' after do body");
        expect(s, TokKind::LParen, "'('");
        parseExpression(s);
        expect(s, TokKind::RParen, "')'");
        expect(s, TokKind::Semicolon, "';'");

        usize jBack = emitJnzRel32(s);
        patchRel32To(s, jBack, loopTop);

        for (usize i = savedBreak; i < s->breakCount; ++i) {
            patchRel32Here(s, s->breakPatches[i]);
        }
        s->breakCount = savedBreak;
        return;
    }

    if (match(s, TokKind::KwFor)) {
        usize savedLocals = s->localCount;
        usize savedBreak  = s->breakCount;
        usize savedCont   = s->continueCount;
        ++s->scopeDepth;

        expect(s, TokKind::LParen, "'(' after for");
        if (match(s, TokKind::Semicolon)) {
            // empty init
        } else if (isTypeStart(s)) {
            parseLocalDeclaration(s);
        } else {
            parseExpression(s);
            expect(s, TokKind::Semicolon, "';' after for init");
        }

        usize condTop = s->codeVec.size;
        bool hasCond  = false;
        usize jExit   = 0;
        if (!match(s, TokKind::Semicolon)) {
            hasCond = true;
            parseExpression(s);
            expect(s, TokKind::Semicolon, "';' after for condition");
            jExit = emitJzRel32(s);
        }
        usize jToBody = emitJmpRel32(s);

        usize incTop = s->codeVec.size;
        if (!match(s, TokKind::RParen)) {
            parseExpression(s);
            expect(s, TokKind::RParen, "')' after for increment");
        }
        usize jToCond = emitJmpRel32(s);
        patchRel32To(s, jToCond, condTop);

        patchRel32Here(s, jToBody);
        parseStatement(s);

        for (usize i = savedCont; i < s->continueCount; ++i) {
            patchRel32To(s, s->continuePatches[i], incTop);
        }
        s->continueCount = savedCont;

        usize jToInc = emitJmpRel32(s);
        patchRel32To(s, jToInc, incTop);

        if (hasCond) {
            patchRel32Here(s, jExit);
        }
        for (usize i = savedBreak; i < s->breakCount; ++i) {
            patchRel32Here(s, s->breakPatches[i]);
        }
        s->breakCount = savedBreak;
        --s->scopeDepth;
        s->localCount = savedLocals;
        return;
    }

    if (match(s, TokKind::KwSwitch)) {
        usize savedBreak = s->breakCount;
        expect(s, TokKind::LParen, "'(' after switch");
        parseExpression(s);
        expect(s, TokKind::RParen, "')'");
        expect(s, TokKind::LBrace, "'{'");

        // Save switch value on a temporary local stack slot
        LocalVar* swTmp = allocateLocal(s, "$switch", CType{ BaseKind::Long, false, 0, false, 0, 0 });
        s->codeVec.push8(0x48);
        s->codeVec.push8(0x89);
        s->codeVec.push8(0x85);
        s->codeVec.push32(static_cast<u32>(swTmp->rbpOffset));

        usize jDispatch = emitJmpRel32(s);

        struct CaseTarget {
            i64   val;
            usize bodyOffset;
        };
        CaseTarget cases[MAX_SWITCH_CASES]{};
        usize caseCount     = 0;
        bool  hasDefault    = false;
        usize defaultOffset = 0;

        while (peek(s).kind != TokKind::RBrace && peek(s).kind != TokKind::Eof) {
            if (match(s, TokKind::KwCase)) {
                i64 cv = evalConstExpr(s);
                expect(s, TokKind::Colon, "':' after case");
                if (caseCount < MAX_SWITCH_CASES) {
                    cases[caseCount++] = CaseTarget{ cv, s->codeVec.size };
                }
            } else if (match(s, TokKind::KwDefault)) {
                expect(s, TokKind::Colon, "':' after default");
                hasDefault    = true;
                defaultOffset = s->codeVec.size;
            } else if (isTypeStart(s)) {
                parseLocalDeclaration(s);
            } else {
                parseStatement(s);
            }
        }
        expect(s, TokKind::RBrace, "'}'");

        usize jAfterBody = emitJmpRel32(s);
        patchRel32Here(s, jDispatch);

        // Load switch value into rcx: mov rcx, [rbp + disp32] (48 8B 8D disp32)
        s->codeVec.push8(0x48);
        s->codeVec.push8(0x8B);
        s->codeVec.push8(0x8D);
        s->codeVec.push32(static_cast<u32>(swTmp->rbpOffset));

        for (usize ci = 0; ci < caseCount; ++ci) {
            emitLoadImm64(s, cases[ci].val);
            // cmp rcx, rax (48 39 C1); je rel32 (0F 84 rel32)
            s->codeVec.push8(0x48); s->codeVec.push8(0x39); s->codeVec.push8(0xC1);
            s->codeVec.push8(0x0F); s->codeVec.push8(0x84);
            usize p = s->codeVec.size;
            s->codeVec.push32(0);
            patchRel32To(s, p, cases[ci].bodyOffset);
        }
        if (hasDefault) {
            usize p = emitJmpRel32(s);
            patchRel32To(s, p, defaultOffset);
        }

        patchRel32Here(s, jAfterBody);
        for (usize i = savedBreak; i < s->breakCount; ++i) {
            patchRel32Here(s, s->breakPatches[i]);
        }
        s->breakCount = savedBreak;
        return;
    }

    if (match(s, TokKind::KwBreak)) {
        expect(s, TokKind::Semicolon, "';' after break");
        if (s->breakCount < MAX_LOOP_FIXUPS) {
            s->breakPatches[s->breakCount++] = emitJmpRel32(s);
        }
        return;
    }

    if (match(s, TokKind::KwContinue)) {
        expect(s, TokKind::Semicolon, "';' after continue");
        if (s->continueCount < MAX_LOOP_FIXUPS) {
            s->continuePatches[s->continueCount++] = emitJmpRel32(s);
        }
        return;
    }

    if (match(s, TokKind::KwReturn)) {
        if (!match(s, TokKind::Semicolon)) {
            parseExpression(s);
            emitCastRax(s, s->curReturnType);
            expect(s, TokKind::Semicolon, "';' after return expression");
        }
        // leave (C9); ret (C3)
        s->codeVec.push8(0xC9);
        s->codeVec.push8(0xC3);
        return;
    }

    parseExpression(s);
    expect(s, TokKind::Semicolon, "';' after expression");
}

// =============================================================================
// Top-Level Declarations (Functions, Globals, Typedefs)
// =============================================================================

void storeConstInData(ByteVec& dataVec, usize offset, const CType& t, i64 val) noexcept {
    usize sz = 8;
    if (t.ptrDepth == 0 && !t.isArray) {
        if (t.base == BaseKind::Char || t.base == BaseKind::Bool) sz = 1;
        else if (t.base == BaseKind::Short) sz = 2;
        else if (t.base == BaseKind::Int) sz = 4;
    }
    if (offset + sz <= dataVec.size) {
        memcpy(dataVec.data + offset, &val, sz);
    }
}

void parseTopLevel(TCCState* s) noexcept {
    while (peek(s).kind != TokKind::Eof) {
        if (match(s, TokKind::Semicolon)) continue;

        if (match(s, TokKind::KwTypedef)) {
            CType base = parseBaseType(s);
            CType full = parsePointerSuffix(s, base);
            char tdName[40]{};

            if (match(s, TokKind::LParen)) {
                expect(s, TokKind::Star, "'*' in function pointer typedef");
                if (peek(s).kind == TokKind::Ident) {
                    str::copy(tdName, tokenStr(s, nextTok(s)), sizeof(tdName));
                }
                expect(s, TokKind::RParen, "')'");
                if (match(s, TokKind::LParen)) {
                    int depth = 1;
                    while (depth > 0 && peek(s).kind != TokKind::Eof) {
                        if (match(s, TokKind::LParen)) ++depth;
                        else if (match(s, TokKind::RParen)) --depth;
                        else nextTok(s);
                    }
                }
                full.base     = BaseKind::FuncPtr;
                full.ptrDepth = 1;
            } else {
                if (peek(s).kind == TokKind::Ident) {
                    str::copy(tdName, tokenStr(s, nextTok(s)), sizeof(tdName));
                }
                if (match(s, TokKind::LBracket)) {
                    full.isArray  = true;
                    full.arrayLen = static_cast<usize>(evalConstExpr(s));
                    expect(s, TokKind::RBracket, "']'");
                }
            }

            if (tdName[0] && s->typedefCount < MAX_TYPEDEFS) {
                str::copy(s->typedefs[s->typedefCount].name, tdName, 40);
                s->typedefs[s->typedefCount].type = full;
                ++s->typedefCount;
            }
            expect(s, TokKind::Semicolon, "';' after typedef");
            continue;
        }

        bool isExtern = false;
        CType baseType = parseBaseType(s, &isExtern);
        if (match(s, TokKind::Semicolon)) {
            continue; // Standalone struct or enum declaration
        }

        CType declType = parsePointerSuffix(s, baseType);
        if (peek(s).kind != TokKind::Ident) {
            reportError(s, peek(s).line, "expected identifier in top-level declaration");
            break;
        }
        const char* name = tokenStr(s, nextTok(s));

        // Function declaration or definition?
        if (match(s, TokKind::LParen)) {
            CType paramTypes[8]{};
            char  paramNames[8][40]{};
            usize paramCount = 0;
            bool  isVariadic = false;

            if (!match(s, TokKind::RParen)) {
                if (peek(s).kind == TokKind::KwVoid && peek(s, 1).kind == TokKind::RParen) {
                    nextTok(s);
                    nextTok(s);
                } else {
                    do {
                        if (match(s, TokKind::Ellipsis)) {
                            isVariadic = true;
                            break;
                        }
                        CType pBase = parseBaseType(s);
                        CType pType = parsePointerSuffix(s, pBase);
                        char pName[40]{};
                        if (match(s, TokKind::LParen)) {
                            expect(s, TokKind::Star, "'*'");
                            if (peek(s).kind == TokKind::Ident) {
                                str::copy(pName, tokenStr(s, nextTok(s)), sizeof(pName));
                            }
                            expect(s, TokKind::RParen, "')'");
                            if (match(s, TokKind::LParen)) {
                                int d = 1;
                                while (d > 0 && peek(s).kind != TokKind::Eof) {
                                    if (match(s, TokKind::LParen)) ++d;
                                    else if (match(s, TokKind::RParen)) --d;
                                    else nextTok(s);
                                }
                            }
                            pType.base     = BaseKind::FuncPtr;
                            pType.ptrDepth = 1;
                        } else {
                            if (peek(s).kind == TokKind::Ident) {
                                str::copy(pName, tokenStr(s, nextTok(s)), sizeof(pName));
                            }
                            if (match(s, TokKind::LBracket)) {
                                if (peek(s).kind != TokKind::RBracket) (void)evalConstExpr(s);
                                expect(s, TokKind::RBracket, "']'");
                                pType.ptrDepth++;
                            }
                        }
                        if (paramCount < 8) {
                            paramTypes[paramCount] = pType;
                            str::copy(paramNames[paramCount], pName, 40);
                            ++paramCount;
                        }
                    } while (match(s, TokKind::Comma));
                    expect(s, TokKind::RParen, "')' after parameter list");
                }
            }

            isize fIdx = findFunction(s, name);
            if (fIdx < 0) {
                if (s->funcCount >= MAX_FUNCS) {
                    reportError(s, s->currentLine, "too many functions");
                    return;
                }
                fIdx = static_cast<isize>(s->funcCount++);
                memset(&s->funcs[fIdx], 0, sizeof(FunctionEntry));
                str::copy(s->funcs[fIdx].name, name, sizeof(s->funcs[fIdx].name));
            }
            FunctionEntry& fe = s->funcs[fIdx];
            fe.returnType = declType;
            fe.paramCount = paramCount;
            fe.isVariadic = isVariadic;
            for (usize pi = 0; pi < paramCount; ++pi) {
                fe.paramTypes[pi] = paramTypes[pi];
            }

            if (match(s, TokKind::Semicolon)) {
                // Function prototype only
                continue;
            }

            expect(s, TokKind::LBrace, "'{' to start function body");

            // Align function start to 16 bytes
            while ((s->codeVec.size & 15U) != 0) {
                s->codeVec.push8(0x90); // nop
            }

            fe.isDefined  = true;
            fe.codeOffset = s->codeVec.size;

            s->localCount     = 0;
            s->scopeDepth     = 1;
            s->curStackOffset = 0;
            s->maxStackOffset = 0;
            s->curReturnType  = declType;
            s->breakCount     = 0;
            s->continueCount  = 0;
            s->labelCount     = 0;
            s->gotoCount      = 0;

            // Emit function prologue:
            // push rbp (55); mov rbp, rsp (48 89 E5); sub rsp, imm32 (48 81 EC imm32)
            s->codeVec.push8(0x55);
            s->codeVec.push8(0x48);
            s->codeVec.push8(0x89);
            s->codeVec.push8(0xE5);
            s->codeVec.push8(0x48);
            s->codeVec.push8(0x81);
            s->codeVec.push8(0xEC);
            s->frameSubPatch = s->codeVec.size;
            s->codeVec.push32(0);

            // Spill register parameters (rdi, rsi, rdx, rcx, r8, r9) to local stack frame
            usize regCount = (paramCount > 6) ? 6 : paramCount;
            for (usize pi = 0; pi < regCount; ++pi) {
                LocalVar* plv = allocateLocal(s, paramNames[pi], paramTypes[pi]);
                switch (pi) {
                    case 0:
                        s->codeVec.push8(0x48); s->codeVec.push8(0x89); s->codeVec.push8(0xBD);
                        s->codeVec.push32(static_cast<u32>(plv->rbpOffset));
                        break;
                    case 1:
                        s->codeVec.push8(0x48); s->codeVec.push8(0x89); s->codeVec.push8(0xB5);
                        s->codeVec.push32(static_cast<u32>(plv->rbpOffset));
                        break;
                    case 2:
                        s->codeVec.push8(0x48); s->codeVec.push8(0x89); s->codeVec.push8(0x95);
                        s->codeVec.push32(static_cast<u32>(plv->rbpOffset));
                        break;
                    case 3:
                        s->codeVec.push8(0x48); s->codeVec.push8(0x89); s->codeVec.push8(0x8D);
                        s->codeVec.push32(static_cast<u32>(plv->rbpOffset));
                        break;
                    case 4:
                        s->codeVec.push8(0x4C); s->codeVec.push8(0x89); s->codeVec.push8(0x85);
                        s->codeVec.push32(static_cast<u32>(plv->rbpOffset));
                        break;
                    case 5:
                        s->codeVec.push8(0x4C); s->codeVec.push8(0x89); s->codeVec.push8(0x8D);
                        s->codeVec.push32(static_cast<u32>(plv->rbpOffset));
                        break;
                }
            }
            for (usize pi = 6; pi < paramCount; ++pi) {
                if (s->localCount < MAX_LOCALS) {
                    LocalVar& plv = s->locals[s->localCount++];
                    str::copy(plv.name, paramNames[pi], sizeof(plv.name));
                    plv.type       = paramTypes[pi];
                    plv.rbpOffset  = static_cast<i32>(16 + (pi - 6) * 8);
                    plv.scopeDepth = 1;
                }
            }

            parseBlock(s);

            // Implicit return 0 at end of function
            emitLoadImm64(s, 0);
            s->codeVec.push8(0xC9); // leave
            s->codeVec.push8(0xC3); // ret

            // Patch stack frame size (rounded up to a multiple of 16)
            u32 alignedFrame = static_cast<u32>(alignUp(static_cast<u64>(s->maxStackOffset + 16), 16));
            s->codeVec.patch32(s->frameSubPatch, alignedFrame);

            // Resolve intra-function gotos
            for (usize gi = 0; gi < s->gotoCount; ++gi) {
                for (usize li = 0; li < s->labelCount; ++li) {
                    if (str::compare(s->gotos[gi].name, s->labels[li].name) == 0) {
                        patchRel32To(s, s->gotos[gi].codeOffset, s->labels[li].codeOffset);
                        break;
                    }
                }
            }
            continue;
        }

        // Global variable declaration
        for (;;) {
            bool unsizedArr = false;
            if (match(s, TokKind::LBracket)) {
                declType.isArray = true;
                if (peek(s).kind == TokKind::RBracket) {
                    unsizedArr        = true;
                    declType.arrayLen = 1;
                } else {
                    declType.arrayLen = static_cast<usize>(evalConstExpr(s));
                }
                expect(s, TokKind::RBracket, "']'");
            }

            if (unsizedArr && peek(s).kind == TokKind::Assign && peek(s, 1).kind == TokKind::StringLit) {
                declType.arrayLen = peek(s, 1).strLen + 1;
            }

            // Align dataVec to 8 bytes
            while ((s->dataVec.size & 7U) != 0) {
                s->dataVec.push8(0);
            }

            usize varOff  = s->dataVec.size;
            usize varSize = typeSize(s, declType);
            if (!isExtern) {
                for (usize z = 0; z < varSize; ++z) s->dataVec.push8(0);
            }

            if (s->globalCount < MAX_GLOBALS) {
                GlobalVar& gv = s->globals[s->globalCount++];
                str::copy(gv.name, name, sizeof(gv.name));
                gv.type          = declType;
                gv.dataOffset    = varOff;
                gv.byteSize      = varSize;
                gv.isExtern      = isExtern;
                gv.initFuncCount = 0;
            }

            if (match(s, TokKind::Assign)) {
                if (declType.isArray && declType.base == BaseKind::Char && peek(s).kind == TokKind::StringLit) {
                    Token st = nextTok(s);
                    const char* sb = tokenStr(s, st);
                    for (usize ci = 0; ci < st.strLen && ci + 1 <= varSize; ++ci) {
                        s->dataVec.data[varOff + ci] = static_cast<u8>(sb[ci]);
                    }
                } else if (declType.ptrDepth == 1 && declType.base == BaseKind::Char && peek(s).kind == TokKind::StringLit) {
                    Token st = nextTok(s);
                    usize strOff = addStringLiteralToData(s, tokenStr(s, st), st.strLen);
                    addReloc(s, RelocKind::DataOffsetAddr64, varOff, strOff);
                    // Wait: DataOffsetAddr64 patches codeVec! For a pointer inside dataVec we can use an array `const char name[] = "..."`
                    (void)strOff;
                } else if (match(s, TokKind::LBrace)) {
                    if (declType.base == BaseKind::Struct && !declType.isArray) {
                        const StructDef& sd = s->structs[declType.structIdx];
                        usize fIdx = 0;
                        while (peek(s).kind != TokKind::RBrace && peek(s).kind != TokKind::Eof) {
                            const StructField* sf = nullptr;
                            if (match(s, TokKind::Dot)) {
                                const char* fn = tokenStr(s, nextTok(s));
                                expect(s, TokKind::Assign, "'='");
                                for (usize i = 0; i < sd.fieldCount; ++i) {
                                    if (str::compare(sd.fields[i].name, fn) == 0) {
                                        sf = &sd.fields[i];
                                        break;
                                    }
                                }
                            } else if (fIdx < sd.fieldCount) {
                                sf = &sd.fields[fIdx++];
                            }

                            if (peek(s).kind == TokKind::Ident && sf && sf->type.base == BaseKind::FuncPtr) {
                                const char* fnName = tokenStr(s, nextTok(s));
                                isize fi = findFunction(s, fnName);
                                if (fi >= 0) {
                                    addReloc(s, RelocKind::DataFuncPtr64, varOff + sf->offset, static_cast<usize>(fi));
                                }
                            } else {
                                i64 cv = evalConstExpr(s);
                                if (sf) storeConstInData(s->dataVec, varOff + sf->offset, sf->type, cv);
                            }
                            if (!match(s, TokKind::Comma)) break;
                        }
                    } else if (declType.isArray) {
                        CType elemType   = declType;
                        elemType.isArray = false;
                        usize elemSize   = typeSize(s, elemType);
                        usize count      = 0;
                        while (peek(s).kind != TokKind::RBrace && peek(s).kind != TokKind::Eof) {
                            i64 cv = evalConstExpr(s);
                            if (unsizedArr && count >= declType.arrayLen) {
                                for (usize z = 0; z < elemSize; ++z) s->dataVec.push8(0);
                                declType.arrayLen = count + 1;
                                s->globals[s->globalCount - 1].type.arrayLen = declType.arrayLen;
                                s->globals[s->globalCount - 1].byteSize      = declType.arrayLen * elemSize;
                            }
                            if (count < declType.arrayLen) {
                                storeConstInData(s->dataVec, varOff + count * elemSize, elemType, cv);
                            }
                            ++count;
                            if (!match(s, TokKind::Comma)) break;
                        }
                    } else {
                        i64 cv = evalConstExpr(s);
                        storeConstInData(s->dataVec, varOff, declType, cv);
                    }
                    expect(s, TokKind::RBrace, "'}'");
                } else {
                    i64 cv = evalConstExpr(s);
                    storeConstInData(s->dataVec, varOff, declType, cv);
                }
            }

            if (!match(s, TokKind::Comma)) break;
            declType = parsePointerSuffix(s, baseType);
            if (peek(s).kind != TokKind::Ident) break;
            name = tokenStr(s, nextTok(s));
        }

        expect(s, TokKind::Semicolon, "';' after global declaration");
    }
}

} // namespace

// =============================================================================
// Public libtcc C API Implementation
// =============================================================================

extern "C" {

TCCState* tcc_new(void) {
    auto* s = new TCCState();
    if (!s) return nullptr;
    registerDefaultTypes(s);
    setMacro(s, "__CORTEZ__", "1");
    setMacro(s, "__x86_64__", "1");
    setMacro(s, "CORTEZ_API_VERSION", "1");
    tcc_add_include_path(s, "/ext/include");
    return s;
}

void tcc_delete(TCCState* s) {
    if (!s) return;
    s->codeVec.destroy();
    s->dataVec.destroy();
    s->stringPool.destroy();
    if (s->tokens) {
        klibc_free(s->tokens);
        s->tokens = nullptr;
    }
    delete s;
}

void tcc_set_lib_path(TCCState*, const char*) {}

void tcc_set_error_func(TCCState* s, void* error_opaque, TCCErrorFunc error_func) {
    if (!s) return;
    s->errorOpaque = error_opaque;
    s->errorFunc   = error_func;
}

int tcc_set_options(TCCState* s, const char* str) {
    if (!s || !str) return 0;
    const char* p = str;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        char tok[96];
        usize n = 0;
        while (*p && *p != ' ' && *p != '\t' && n + 1 < sizeof(tok)) {
            tok[n++] = *p++;
        }
        tok[n] = '\0';
        if (str::compareN(tok, "-I", 2) == 0 && tok[2]) {
            tcc_add_include_path(s, tok + 2);
        } else if (str::compareN(tok, "-D", 2) == 0 && tok[2]) {
            char* eq = const_cast<char*>(str::findChar(tok + 2, '='));
            if (eq) {
                *eq = '\0';
                tcc_define_symbol(s, tok + 2, eq + 1);
            } else {
                tcc_define_symbol(s, tok + 2, "1");
            }
        }
    }
    return 0;
}

int tcc_add_include_path(TCCState* s, const char* pathname) {
    if (!s || !pathname || !pathname[0]) return -1;
    for (usize i = 0; i < s->includePathCount; ++i) {
        if (str::compare(s->includePaths[i], pathname) == 0) return 0;
    }
    if (s->includePathCount >= MAX_INCLUDE_PATHS) return -1;
    str::copy(s->includePaths[s->includePathCount++], pathname, 96);
    return 0;
}

int tcc_add_sysinclude_path(TCCState* s, const char* pathname) {
    return tcc_add_include_path(s, pathname);
}

void tcc_define_symbol(TCCState* s, const char* sym, const char* value) {
    if (!s || !sym) return;
    setMacro(s, sym, value ? value : "1");
}

void tcc_undefine_symbol(TCCState* s, const char* sym) {
    if (!s || !sym) return;
    removeMacro(s, sym);
}

int tcc_set_output_type(TCCState* s, int output_type) {
    if (!s) return -1;
    s->outputType = output_type;
    return 0;
}

int tcc_add_symbol(TCCState* s, const char* name, const void* val) {
    if (!s || !name || !name[0]) return -1;
    isize idx = findExternalSym(s, name);
    if (idx >= 0) {
        s->extSyms[idx].addr = val;
        return 0;
    }
    if (s->extSymCount >= MAX_EXT_SYMS) return -1;
    str::copy(s->extSyms[s->extSymCount].name, name, 48);
    s->extSyms[s->extSymCount].addr = val;
    ++s->extSymCount;
    return 0;
}

int tcc_add_Symbol(TCCState* s, const char* name, const void* val) {
    return tcc_add_symbol(s, name, val);
}

int tcc_compile_string(TCCState* s, const char* buf) {
    if (!s || !buf) return -1;

    s->hasError    = false;
    s->tokenCount  = 0;
    s->tokPos      = 0;
    s->currentLine = 1;

    ByteVec preprocessed{};
    s->jmpActive = true;
    if (cortez_setjmp(s->errorJmp) != 0) {
        s->jmpActive = false;
        preprocessed.destroy();
        return -1;
    }

    preprocessSource(s, s->currentFile, buf, preprocessed, 0);
    preprocessed.appendChar('\0');

    tokenize(s, reinterpret_cast<const char*>(preprocessed.data));
    if (!s->hasError) {
        parseTopLevel(s);
    }

    s->jmpActive = false;
    preprocessed.destroy();
    return s->hasError ? -1 : 0;
}

int tcc_add_file(TCCState* s, const char* filename) {
    if (!s || !filename || !filename[0]) return -1;

    const proc::Task* t = proc::Scheduler::instance().getCurrentTask();
    const char* cwd     = t ? t->getCwd() : "/";

    usize fsize = 0;
    char* src   = readVfsFileToHeap(cwd, filename, &fsize);
    if (!src) {
        reportError(s, 0, "cannot open source file '%s'", filename);
        return -1;
    }

    str::copy(s->currentFile, filename, sizeof(s->currentFile));
    int rc = tcc_compile_string(s, src);
    klibc_free(src);
    return rc;
}

int tcc_relocate(TCCState* s, void* ptr) {
    if (!s || s->hasError) return -1;

    usize alignedText = static_cast<usize>(alignUp(s->codeVec.size, 64));
    if (alignedText == 0) alignedText = 64;
    usize alignedData = static_cast<usize>(alignUp(s->dataVec.size, 64));
    usize totalBytes  = alignedText + alignedData + 64;

    if (!ptr) {
        return static_cast<int>(totalBytes);
    }

    u8* basePtr = static_cast<u8*>(ptr);
    u8* textPtr = basePtr;
    u8* dataPtr = basePtr + alignedText;

    memset(basePtr, 0, totalBytes);
    if (s->codeVec.size > 0) {
        memcpy(textPtr, s->codeVec.data, s->codeVec.size);
    }
    if (s->dataVec.size > 0) {
        memcpy(dataPtr, s->dataVec.data, s->dataVec.size);
    }

    s->relocatedBase     = basePtr;
    s->relocatedTextSize = alignedText;

    for (usize fi = 0; fi < s->funcCount; ++fi) {
        if (s->funcs[fi].isDefined) {
            s->funcs[fi].relocatedAddr = textPtr + s->funcs[fi].codeOffset;
        }
    }

    for (usize ri = 0; ri < s->relocCount; ++ri) {
        const RelocEntry& rel = s->relocs[ri];
        switch (rel.kind) {
            case RelocKind::InternalFuncCall32: {
                if (rel.targetIndex >= s->funcCount || !s->funcs[rel.targetIndex].isDefined) {
                    // Check if it was actually an external symbol declared with a prototype
                    isize eIdx = (rel.targetIndex < s->funcCount)
                                     ? findExternalSym(s, s->funcs[rel.targetIndex].name)
                                     : -1;
                    if (eIdx >= 0 && s->extSyms[eIdx].addr) {
                        u64 targetAddr = reinterpret_cast<u64>(s->extSyms[eIdx].addr);
                        u64 nextRip    = reinterpret_cast<u64>(textPtr + rel.patchOffset + 4);
                        i64 disp       = static_cast<i64>(targetAddr - nextRip);
                        u32 d32        = static_cast<u32>(static_cast<i32>(disp));
                        memcpy(textPtr + rel.patchOffset, &d32, 4);
                        break;
                    }
                    reportError(s, 0, "undefined symbol '%s'",
                                (rel.targetIndex < s->funcCount) ? s->funcs[rel.targetIndex].name : "?");
                    return -1;
                }
                u64 targetAddr = reinterpret_cast<u64>(textPtr + s->funcs[rel.targetIndex].codeOffset);
                u64 nextRip    = reinterpret_cast<u64>(textPtr + rel.patchOffset + 4);
                i32 d32        = static_cast<i32>(targetAddr - nextRip);
                memcpy(textPtr + rel.patchOffset, &d32, 4);
                break;
            }
            case RelocKind::InternalFuncAddr64: {
                if (rel.targetIndex < s->funcCount && s->funcs[rel.targetIndex].isDefined) {
                    u64 addr = reinterpret_cast<u64>(textPtr + s->funcs[rel.targetIndex].codeOffset);
                    memcpy(textPtr + rel.patchOffset, &addr, 8);
                }
                break;
            }
            case RelocKind::ExternalSymAddr64: {
                if (rel.targetIndex < s->extSymCount) {
                    u64 addr = reinterpret_cast<u64>(s->extSyms[rel.targetIndex].addr);
                    memcpy(textPtr + rel.patchOffset, &addr, 8);
                }
                break;
            }
            case RelocKind::DataOffsetAddr64: {
                u64 addr = reinterpret_cast<u64>(dataPtr + rel.targetIndex);
                memcpy(textPtr + rel.patchOffset, &addr, 8);
                break;
            }
            case RelocKind::DataFuncPtr64: {
                if (rel.targetIndex < s->funcCount && s->funcs[rel.targetIndex].isDefined) {
                    u64 addr = reinterpret_cast<u64>(textPtr + s->funcs[rel.targetIndex].codeOffset);
                    memcpy(dataPtr + rel.patchOffset, &addr, 8);
                }
                break;
            }
        }
    }

    return 0;
}

void* tcc_get_symbol(TCCState* s, const char* name) {
    if (!s || !name || !s->relocatedBase) return nullptr;

    u8* basePtr = static_cast<u8*>(s->relocatedBase);
    u8* textPtr = basePtr;
    u8* dataPtr = basePtr + s->relocatedTextSize;

    for (usize fi = 0; fi < s->funcCount; ++fi) {
        if (s->funcs[fi].isDefined && str::compare(s->funcs[fi].name, name) == 0) {
            return textPtr + s->funcs[fi].codeOffset;
        }
    }
    for (usize gi = 0; gi < s->globalCount; ++gi) {
        if (!s->globals[gi].isExtern && str::compare(s->globals[gi].name, name) == 0) {
            return dataPtr + s->globals[gi].dataOffset;
        }
    }
    return nullptr;
}

size_t tcc_get_exported_symbol_count(TCCState* s) {
    if (!s) return 0;
    return s->funcCount;
}

const char* tcc_get_exported_symbol_name(TCCState* s, size_t index) {
    if (!s || index >= s->funcCount || !s->funcs[index].isDefined) return nullptr;
    return s->funcs[index].name;
}

void* tcc_get_exported_symbol_addr(TCCState* s, size_t index) {
    if (!s || index >= s->funcCount || !s->funcs[index].isDefined) return nullptr;
    return s->funcs[index].relocatedAddr;
}

int tcc_output_file(TCCState* s, const char* filename) {
    if (!s || s->hasError || !filename || !filename[0]) return -1;
    if (s->codeVec.size == 0) return -1;

    // We build 8 standard ELF64 sections:
    // 0: SHN_UNDEF (Null)
    // 1: .text
    // 2: .data
    // 3: .rela.text
    // 4: .rela.data
    // 5: .symtab
    // 6: .strtab
    // 7: .shstrtab

    ByteVec strtab{};
    strtab.push8(0);

    ByteVec shstrtab{};
    shstrtab.push8(0);

    auto addString = [](ByteVec& vec, const char* str) -> u32 {
        if (!str || !str[0]) return 0;
        u32 offset = static_cast<u32>(vec.size);
        vec.append(str, str::length(str) + 1);
        return offset;
    };

    u32 nameText     = addString(shstrtab, ".text");
    u32 nameData     = addString(shstrtab, ".data");
    u32 nameRelaText = addString(shstrtab, ".rela.text");
    u32 nameRelaData = addString(shstrtab, ".rela.data");
    u32 nameSymtab   = addString(shstrtab, ".symtab");
    u32 nameStrtab   = addString(shstrtab, ".strtab");
    u32 nameShstrtab = addString(shstrtab, ".shstrtab");

    ByteVec symtabVec{};

    // Symbol 0: Null symbol
    elf::Elf64_Sym nullSym{};
    symtabVec.append(&nullSym, sizeof(nullSym));

    // Symbol 1: .text section symbol (STB_LOCAL, STT_SECTION)
    elf::Elf64_Sym textSecSym{};
    textSecSym.st_info  = ELF64_ST_INFO(elf::STB_LOCAL, elf::STT_SECTION);
    textSecSym.st_shndx = 1;
    symtabVec.append(&textSecSym, sizeof(textSecSym));

    // Symbol 2: .data section symbol (STB_LOCAL, STT_SECTION)
    elf::Elf64_Sym dataSecSym{};
    dataSecSym.st_info  = ELF64_ST_INFO(elf::STB_LOCAL, elf::STT_SECTION);
    dataSecSym.st_shndx = 2;
    symtabVec.append(&dataSecSym, sizeof(dataSecSym));

    constexpr u32 FIRST_GLOBAL_SYM = 3;

    auto findSymbol = [&](const char* name) -> i32 {
        if (!name || !name[0]) return -1;
        usize count = symtabVec.size / sizeof(elf::Elf64_Sym);
        const auto* syms = reinterpret_cast<const elf::Elf64_Sym*>(symtabVec.data);
        for (usize i = 1; i < count; ++i) {
            if (syms[i].st_name < strtab.size) {
                const char* symName = reinterpret_cast<const char*>(strtab.data + syms[i].st_name);
                if (str::compare(symName, name) == 0) {
                    return static_cast<i32>(i);
                }
            }
        }
        return -1;
    };

    u32 funcSymIdx[MAX_FUNCS]{};
    for (usize fi = 0; fi < s->funcCount; ++fi) {
        const auto& fn = s->funcs[fi];
        i32 existing = findSymbol(fn.name);
        if (existing >= 0) {
            funcSymIdx[fi] = static_cast<u32>(existing);
            continue;
        }

        u32 nameOff = addString(strtab, fn.name);
        elf::Elf64_Sym sym{};
        sym.st_name = nameOff;
        if (fn.isDefined) {
            sym.st_info  = ELF64_ST_INFO(elf::STB_GLOBAL, elf::STT_FUNC);
            sym.st_shndx = 1;
            sym.st_value = fn.codeOffset;
        } else {
            sym.st_info  = ELF64_ST_INFO(elf::STB_GLOBAL, elf::STT_NOTYPE);
            sym.st_shndx = elf::SHN_UNDEF;
            sym.st_value = 0;
        }
        funcSymIdx[fi] = static_cast<u32>(symtabVec.size / sizeof(elf::Elf64_Sym));
        symtabVec.append(&sym, sizeof(sym));
    }

    u32 globalSymIdx[MAX_GLOBALS]{};
    for (usize gi = 0; gi < s->globalCount; ++gi) {
        const auto& gv = s->globals[gi];
        i32 existing = findSymbol(gv.name);
        if (existing >= 0) {
            globalSymIdx[gi] = static_cast<u32>(existing);
            continue;
        }

        u32 nameOff = addString(strtab, gv.name);
        elf::Elf64_Sym sym{};
        sym.st_name = nameOff;
        if (!gv.isExtern) {
            sym.st_info  = ELF64_ST_INFO(elf::STB_GLOBAL, elf::STT_OBJECT);
            sym.st_shndx = 2;
            sym.st_value = gv.dataOffset;
            sym.st_size  = gv.byteSize;
        } else {
            sym.st_info  = ELF64_ST_INFO(elf::STB_GLOBAL, elf::STT_NOTYPE);
            sym.st_shndx = elf::SHN_UNDEF;
            sym.st_value = 0;
        }
        globalSymIdx[gi] = static_cast<u32>(symtabVec.size / sizeof(elf::Elf64_Sym));
        symtabVec.append(&sym, sizeof(sym));
    }

    u32 extSymIdx[MAX_EXT_SYMS]{};
    for (usize ei = 0; ei < s->extSymCount; ++ei) {
        const auto& ext = s->extSyms[ei];
        i32 existing = findSymbol(ext.name);
        if (existing >= 0) {
            extSymIdx[ei] = static_cast<u32>(existing);
            continue;
        }

        u32 nameOff = addString(strtab, ext.name);
        elf::Elf64_Sym sym{};
        sym.st_name  = nameOff;
        sym.st_info  = ELF64_ST_INFO(elf::STB_GLOBAL, elf::STT_NOTYPE);
        sym.st_shndx = elf::SHN_UNDEF;
        sym.st_value = 0;
        extSymIdx[ei] = static_cast<u32>(symtabVec.size / sizeof(elf::Elf64_Sym));
        symtabVec.append(&sym, sizeof(sym));
    }

    ByteVec relaTextVec{};
    ByteVec relaDataVec{};

    for (usize ri = 0; ri < s->relocCount; ++ri) {
        const RelocEntry& rel = s->relocs[ri];
        switch (rel.kind) {
            case RelocKind::InternalFuncCall32: {
                u32 symIdx = (rel.targetIndex < s->funcCount) ? funcSymIdx[rel.targetIndex] : 0;
                elf::Elf64_Rela rela{};
                rela.r_offset = rel.patchOffset;
                rela.r_info   = ELF64_R_INFO(symIdx, elf::R_X86_64_PC32);
                rela.r_addend = -4;
                relaTextVec.append(&rela, sizeof(rela));
                break;
            }
            case RelocKind::InternalFuncAddr64: {
                u32 symIdx = (rel.targetIndex < s->funcCount) ? funcSymIdx[rel.targetIndex] : 0;
                elf::Elf64_Rela rela{};
                rela.r_offset = rel.patchOffset;
                rela.r_info   = ELF64_R_INFO(symIdx, elf::R_X86_64_64);
                rela.r_addend = 0;
                relaTextVec.append(&rela, sizeof(rela));
                break;
            }
            case RelocKind::ExternalSymAddr64: {
                u32 symIdx = (rel.targetIndex < s->extSymCount) ? extSymIdx[rel.targetIndex] : 0;
                elf::Elf64_Rela rela{};
                rela.r_offset = rel.patchOffset;
                rela.r_info   = ELF64_R_INFO(symIdx, elf::R_X86_64_64);
                rela.r_addend = 0;
                relaTextVec.append(&rela, sizeof(rela));
                break;
            }
            case RelocKind::DataOffsetAddr64: {
                elf::Elf64_Rela rela{};
                rela.r_offset = rel.patchOffset;
                rela.r_info   = ELF64_R_INFO(2 /* .data sec sym */, elf::R_X86_64_64);
                rela.r_addend = static_cast<i64>(rel.targetIndex);
                relaTextVec.append(&rela, sizeof(rela));
                break;
            }
            case RelocKind::DataFuncPtr64: {
                u32 symIdx = (rel.targetIndex < s->funcCount) ? funcSymIdx[rel.targetIndex] : 0;
                elf::Elf64_Rela rela{};
                rela.r_offset = rel.patchOffset;
                rela.r_info   = ELF64_R_INFO(symIdx, elf::R_X86_64_64);
                rela.r_addend = 0;
                relaDataVec.append(&rela, sizeof(rela));
                break;
            }
        }
    }

    auto alignVec = [](ByteVec& v, usize align) {
        while ((v.size % align) != 0) {
            v.push8(0);
        }
    };

    ByteVec elfOut{};
    elf::Elf64_Ehdr zeroHdr{};
    elfOut.append(&zeroHdr, sizeof(zeroHdr));

    alignVec(elfOut, 16);
    u64 textOff = elfOut.size;
    elfOut.append(s->codeVec.data, s->codeVec.size);
    u64 textSize = s->codeVec.size;

    alignVec(elfOut, 16);
    u64 dataOff = elfOut.size;
    if (s->dataVec.size > 0) {
        elfOut.append(s->dataVec.data, s->dataVec.size);
    }
    u64 dataSize = s->dataVec.size;

    alignVec(elfOut, 8);
    u64 relaTextOff = elfOut.size;
    if (relaTextVec.size > 0) {
        elfOut.append(relaTextVec.data, relaTextVec.size);
    }
    u64 relaTextSize = relaTextVec.size;

    alignVec(elfOut, 8);
    u64 relaDataOff = elfOut.size;
    if (relaDataVec.size > 0) {
        elfOut.append(relaDataVec.data, relaDataVec.size);
    }
    u64 relaDataSize = relaDataVec.size;

    alignVec(elfOut, 8);
    u64 symtabOff = elfOut.size;
    elfOut.append(symtabVec.data, symtabVec.size);
    u64 symtabSize = symtabVec.size;

    alignVec(elfOut, 1);
    u64 strtabOff = elfOut.size;
    elfOut.append(strtab.data, strtab.size);
    u64 strtabSize = strtab.size;

    alignVec(elfOut, 1);
    u64 shstrtabOff = elfOut.size;
    elfOut.append(shstrtab.data, shstrtab.size);
    u64 shstrtabSize = shstrtab.size;

    alignVec(elfOut, 8);
    u64 shoff = elfOut.size;

    constexpr usize NUM_SECTIONS = 8;
    elf::Elf64_Shdr shdrs[NUM_SECTIONS]{};

    // 1: .text
    shdrs[1].sh_name      = nameText;
    shdrs[1].sh_type      = elf::SHT_PROGBITS;
    shdrs[1].sh_flags     = elf::SHF_ALLOC | elf::SHF_EXECINSTR;
    shdrs[1].sh_addr      = 0;
    shdrs[1].sh_offset    = textOff;
    shdrs[1].sh_size      = textSize;
    shdrs[1].sh_addralign = 16;

    // 2: .data
    shdrs[2].sh_name      = nameData;
    shdrs[2].sh_type      = elf::SHT_PROGBITS;
    shdrs[2].sh_flags     = elf::SHF_ALLOC | elf::SHF_WRITE;
    shdrs[2].sh_addr      = 0;
    shdrs[2].sh_offset    = dataOff;
    shdrs[2].sh_size      = dataSize;
    shdrs[2].sh_addralign = 16;

    // 3: .rela.text
    shdrs[3].sh_name      = nameRelaText;
    shdrs[3].sh_type      = elf::SHT_RELA;
    shdrs[3].sh_flags     = elf::SHF_INFO_LINK;
    shdrs[3].sh_addr      = 0;
    shdrs[3].sh_offset    = relaTextOff;
    shdrs[3].sh_size      = relaTextSize;
    shdrs[3].sh_link      = 5; // .symtab
    shdrs[3].sh_info      = 1; // .text
    shdrs[3].sh_addralign = 8;
    shdrs[3].sh_entsize   = sizeof(elf::Elf64_Rela);

    // 4: .rela.data
    shdrs[4].sh_name      = nameRelaData;
    shdrs[4].sh_type      = elf::SHT_RELA;
    shdrs[4].sh_flags     = elf::SHF_INFO_LINK;
    shdrs[4].sh_addr      = 0;
    shdrs[4].sh_offset    = relaDataOff;
    shdrs[4].sh_size      = relaDataSize;
    shdrs[4].sh_link      = 5; // .symtab
    shdrs[4].sh_info      = 2; // .data
    shdrs[4].sh_addralign = 8;
    shdrs[4].sh_entsize   = sizeof(elf::Elf64_Rela);

    // 5: .symtab
    shdrs[5].sh_name      = nameSymtab;
    shdrs[5].sh_type      = elf::SHT_SYMTAB;
    shdrs[5].sh_flags     = 0;
    shdrs[5].sh_addr      = 0;
    shdrs[5].sh_offset    = symtabOff;
    shdrs[5].sh_size      = symtabSize;
    shdrs[5].sh_link      = 6; // .strtab
    shdrs[5].sh_info      = FIRST_GLOBAL_SYM;
    shdrs[5].sh_addralign = 8;
    shdrs[5].sh_entsize   = sizeof(elf::Elf64_Sym);

    // 6: .strtab
    shdrs[6].sh_name      = nameStrtab;
    shdrs[6].sh_type      = elf::SHT_STRTAB;
    shdrs[6].sh_flags     = 0;
    shdrs[6].sh_addr      = 0;
    shdrs[6].sh_offset    = strtabOff;
    shdrs[6].sh_size      = strtabSize;
    shdrs[6].sh_addralign = 1;

    // 7: .shstrtab
    shdrs[7].sh_name      = nameShstrtab;
    shdrs[7].sh_type      = elf::SHT_STRTAB;
    shdrs[7].sh_flags     = 0;
    shdrs[7].sh_addr      = 0;
    shdrs[7].sh_offset    = shstrtabOff;
    shdrs[7].sh_size      = shstrtabSize;
    shdrs[7].sh_addralign = 1;

    elfOut.append(shdrs, sizeof(shdrs));

    elf::Elf64_Addr entryAddr = 0;
    isize mainIdx = findFunction(s, "main");
    if (mainIdx >= 0 && s->funcs[mainIdx].isDefined) {
        entryAddr = s->funcs[mainIdx].codeOffset;
    } else {
        isize initIdx = findFunction(s, "module_init");
        if (initIdx >= 0 && s->funcs[initIdx].isDefined) {
            entryAddr = s->funcs[initIdx].codeOffset;
        }
    }

    elf::Elf64_Ehdr ehdr{};
    ehdr.e_ident[elf::EI_MAG0]       = elf::ELFMAG0;
    ehdr.e_ident[elf::EI_MAG1]       = elf::ELFMAG1;
    ehdr.e_ident[elf::EI_MAG2]       = elf::ELFMAG2;
    ehdr.e_ident[elf::EI_MAG3]       = elf::ELFMAG3;
    ehdr.e_ident[elf::EI_CLASS]      = elf::ELFCLASS64;
    ehdr.e_ident[elf::EI_DATA]       = elf::ELFDATA2LSB;
    ehdr.e_ident[elf::EI_VERSION]    = elf::EV_CURRENT;
    ehdr.e_ident[elf::EI_OSABI]      = elf::ELFOSABI_SYSV;
    ehdr.e_ident[elf::EI_ABIVERSION] = 0;

    ehdr.e_type      = elf::ET_REL;
    ehdr.e_machine   = elf::EM_X86_64;
    ehdr.e_version   = elf::EV_CURRENT;
    ehdr.e_entry     = entryAddr;
    ehdr.e_phoff     = 0;
    ehdr.e_shoff     = shoff;
    ehdr.e_flags     = 0;
    ehdr.e_ehsize    = sizeof(elf::Elf64_Ehdr);
    ehdr.e_phentsize = 0;
    ehdr.e_phnum     = 0;
    ehdr.e_shentsize = sizeof(elf::Elf64_Shdr);
    ehdr.e_shnum     = NUM_SECTIONS;
    ehdr.e_shstrndx  = 7;

    memcpy(elfOut.data, &ehdr, sizeof(ehdr));

    const proc::Task* t = proc::Scheduler::instance().getCurrentTask();
    const char* cwd     = t ? t->getCwd() : "/";
    Status st = fs::VfsManager::instance().writeFileAll(cwd, filename, elfOut.data, elfOut.size);

    int totalWritten = (st == Status::Ok) ? static_cast<int>(elfOut.size) : -1;

    strtab.destroy();
    shstrtab.destroy();
    symtabVec.destroy();
    relaTextVec.destroy();
    relaDataVec.destroy();
    elfOut.destroy();

    return totalWritten;
}

} // extern "C"
