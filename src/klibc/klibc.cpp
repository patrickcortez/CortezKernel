/**
 * @file klibc.cpp
 * @brief Implementation of freestanding C standard library functions for compiler and runtime modules.
 */

#include "klibc.hpp"
#include "../memory/heap_allocator.hpp"
#include "../lib/string_utils.hpp"

extern "C" {

void* klibc_malloc(size_t size) noexcept {
    return cortez::memory::KernelHeap::instance().allocateRaw(size, 16, false);
}

void* klibc_calloc(size_t nmemb, size_t size) noexcept {
    if (nmemb == 0 || size == 0) return nullptr;
    size_t total = nmemb * size;
    if (total / nmemb != size) return nullptr;
    return cortez::memory::KernelHeap::instance().allocateRaw(total, 16, true);
}

void* klibc_realloc(void* ptr, size_t size) noexcept {
    return cortez::memory::KernelHeap::instance().reallocate(ptr, size, 16);
}

void klibc_free(void* ptr) noexcept {
    cortez::memory::KernelHeap::instance().deallocate(ptr);
}

size_t klibc_strlen(const char* s) noexcept {
    return cortez::str::length(s);
}

char* klibc_strcpy(char* dst, const char* src) noexcept {
    if (!dst) return nullptr;
    if (!src) {
        dst[0] = '\0';
        return dst;
    }
    char* d = dst;
    while ((*d++ = *src++) != '\0') {}
    return dst;
}

char* klibc_strncpy(char* dst, const char* src, size_t n) noexcept {
    if (!dst || n == 0) return dst;
    size_t i = 0;
    if (src) {
        for (; i < n && src[i] != '\0'; ++i) {
            dst[i] = src[i];
        }
    }
    for (; i < n; ++i) {
        dst[i] = '\0';
    }
    return dst;
}

char* klibc_strcat(char* dst, const char* src) noexcept {
    if (!dst || !src) return dst;
    char* d = dst + cortez::str::length(dst);
    while ((*d++ = *src++) != '\0') {}
    return dst;
}

int klibc_strcmp(const char* a, const char* b) noexcept {
    return cortez::str::compare(a, b);
}

int klibc_strncmp(const char* a, const char* b, size_t n) noexcept {
    return cortez::str::compareN(a, b, n);
}

int klibc_strcasecmp(const char* a, const char* b) noexcept {
    return cortez::str::compareCaseInsensitive(a, b);
}

char* klibc_strchr(const char* s, int c) noexcept {
    return const_cast<char*>(cortez::str::findChar(s, static_cast<char>(c)));
}

char* klibc_strrchr(const char* s, int c) noexcept {
    return const_cast<char*>(cortez::str::findLastChar(s, static_cast<char>(c)));
}

char* klibc_strstr(const char* haystack, const char* needle) noexcept {
    if (!haystack || !needle) return nullptr;
    if (!needle[0]) return const_cast<char*>(haystack);
    size_t nlen = cortez::str::length(needle);
    for (const char* p = haystack; *p; ++p) {
        if (*p == needle[0] && cortez::str::compareN(p, needle, nlen) == 0) {
            return const_cast<char*>(p);
        }
    }
    return nullptr;
}

char* klibc_strdup(const char* s) noexcept {
    if (!s) return nullptr;
    size_t len = cortez::str::length(s);
    char* copy = static_cast<char*>(klibc_malloc(len + 1));
    if (!copy) return nullptr;
    memcpy(copy, s, len + 1);
    return copy;
}

unsigned long klibc_strtoul(const char* nptr, char** endptr, int base) noexcept {
    if (!nptr) {
        if (endptr) *endptr = nullptr;
        return 0;
    }
    const char* p = nptr;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;

    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    else if (*p == '+') { ++p; }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if ((base == 0 || base == 2) && p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        base = 2;
        p += 2;
    } else if (base == 0) {
        base = (p[0] == '0') ? 8 : 10;
    }

    unsigned long result = 0;
    const char* start    = p;
    while (*p) {
        int digit = -1;
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (*p >= 'a' && *p <= 'z') digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') digit = *p - 'A' + 10;
        if (digit < 0 || digit >= base) break;
        result = result * static_cast<unsigned long>(base) + static_cast<unsigned long>(digit);
        ++p;
    }

    if (endptr) {
        *endptr = const_cast<char*>((p == start) ? nptr : p);
    }
    return neg ? (0UL - result) : result;
}

long klibc_strtol(const char* nptr, char** endptr, int base) noexcept {
    return static_cast<long>(klibc_strtoul(nptr, endptr, base));
}

int klibc_atoi(const char* nptr) noexcept {
    return static_cast<int>(klibc_strtol(nptr, nullptr, 10));
}

namespace {
struct SnprintfCtx {
    char*  buf;
    size_t cap;
    size_t pos;
};

void emitSnprintfChar(char c, SnprintfCtx* ctx) noexcept {
    if (ctx->buf && ctx->cap > 0 && ctx->pos + 1 < ctx->cap) {
        ctx->buf[ctx->pos] = c;
    }
    ++ctx->pos;
}
} // namespace

int klibc_vsnprintf(char* buf, size_t size, const char* fmt, va_list args) noexcept {
    SnprintfCtx ctx{ buf, size, 0 };
    if (!fmt) {
        if (buf && size > 0) buf[0] = '\0';
        return 0;
    }

    while (*fmt) {
        if (*fmt != '%') {
            emitSnprintfChar(*fmt++, &ctx);
            continue;
        }
        ++fmt;
        if (*fmt == '%') {
            emitSnprintfChar('%', &ctx);
            ++fmt;
            continue;
        }

        bool leftAlign = false;
        if (*fmt == '-') {
            leftAlign = true;
            ++fmt;
        }

        char padChar = ' ';
        if (*fmt == '0' && !leftAlign) {
            padChar = '0';
            ++fmt;
        }

        uint32_t width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + static_cast<uint32_t>(*fmt - '0');
            ++fmt;
        }

        bool isLong = false;
        if (*fmt == 'l') {
            isLong = true;
            ++fmt;
            if (*fmt == 'l') ++fmt;
        } else if (*fmt == 'z') {
            isLong = true;
            ++fmt;
        }

        char numBuf[72];
        const char* piece = numBuf;
        switch (*fmt) {
            case 's': {
                piece = va_arg(args, const char*);
                if (!piece) piece = "(null)";
                break;
            }
            case 'c': {
                numBuf[0] = static_cast<char>(va_arg(args, int));
                numBuf[1] = '\0';
                break;
            }
            case 'd':
            case 'i': {
                int64_t v = isLong ? va_arg(args, int64_t) : va_arg(args, int);
                cortez::str::intToStr(v, numBuf, 10);
                break;
            }
            case 'u': {
                uint64_t v = isLong ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                cortez::str::uintToStr(v, numBuf, 10, (padChar == '0') ? width : 0);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v = isLong ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                cortez::str::uintToStr(v, numBuf, 16, (padChar == '0') ? width : 0);
                if (*fmt == 'x') {
                    for (size_t i = 0; numBuf[i]; ++i) {
                        numBuf[i] = cortez::str::toLower(numBuf[i]);
                    }
                }
                break;
            }
            case 'p': {
                uint64_t v = reinterpret_cast<uint64_t>(va_arg(args, void*));
                numBuf[0] = '0';
                numBuf[1] = 'x';
                cortez::str::uintToStr(v, numBuf + 2, 16, 16);
                for (size_t i = 2; numBuf[i]; ++i) {
                    numBuf[i] = cortez::str::toLower(numBuf[i]);
                }
                break;
            }
            default: {
                numBuf[0] = '%';
                numBuf[1] = *fmt ? *fmt : '\0';
                numBuf[2] = '\0';
                break;
            }
        }

        size_t plen = cortez::str::length(piece);
        if (!leftAlign && width > plen && padChar == ' ') {
            for (size_t p = 0; p < width - plen; ++p) emitSnprintfChar(' ', &ctx);
        }
        for (size_t i = 0; i < plen; ++i) emitSnprintfChar(piece[i], &ctx);
        if (leftAlign && width > plen) {
            for (size_t p = 0; p < width - plen; ++p) emitSnprintfChar(' ', &ctx);
        }

        if (*fmt) ++fmt;
    }

    if (buf && size > 0) {
        size_t termPos = (ctx.pos < size) ? ctx.pos : (size - 1);
        buf[termPos]   = '\0';
    }
    return static_cast<int>(ctx.pos);
}

int klibc_snprintf(char* buf, size_t size, const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    int r = klibc_vsnprintf(buf, size, fmt, args);
    va_end(args);
    return r;
}

int klibc_sprintf(char* buf, const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    int r = klibc_vsnprintf(buf, 65536, fmt, args);
    va_end(args);
    return r;
}

} // extern "C"
