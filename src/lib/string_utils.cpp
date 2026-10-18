/**
 * @file string_utils.cpp
 * @brief Implementation of freestanding string manipulation utilities.
 */

#include "string_utils.hpp"

namespace cortez::str {

usize length(const char* s) noexcept {
    if (!s) return 0;
    usize len = 0;
    while (s[len] != '\0') {
        ++len;
    }
    return len;
}

i32 compare(const char* a, const char* b) noexcept {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && (*a == *b)) {
        ++a;
        ++b;
    }
    return static_cast<u8>(*a) - static_cast<u8>(*b);
}

i32 compareN(const char* a, const char* b, usize maxLen) noexcept {
    if (maxLen == 0 || a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    for (usize i = 0; i < maxLen; ++i) {
        if (a[i] != b[i] || a[i] == '\0') {
            return static_cast<u8>(a[i]) - static_cast<u8>(b[i]);
        }
    }
    return 0;
}

char toUpper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
}

char toLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

i32 compareCaseInsensitive(const char* a, const char* b) noexcept {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    while (*a && (toLower(*a) == toLower(*b))) {
        ++a;
        ++b;
    }
    return static_cast<u8>(toLower(*a)) - static_cast<u8>(toLower(*b));
}

char* copy(char* dst, const char* src, usize maxDstSize) noexcept {
    if (!dst || maxDstSize == 0) return dst;
    if (!src) {
        dst[0] = '\0';
        return dst;
    }
    usize i = 0;
    for (; i + 1 < maxDstSize && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return dst;
}

char* concat(char* dst, const char* src, usize maxDstSize) noexcept {
    if (!dst || maxDstSize == 0 || !src) return dst;
    usize dstLen = length(dst);
    if (dstLen + 1 >= maxDstSize) return dst;
    copy(dst + dstLen, src, maxDstSize - dstLen);
    return dst;
}

const char* findChar(const char* s, char c) noexcept {
    if (!s) return nullptr;
    while (*s != '\0') {
        if (*s == c) return s;
        ++s;
    }
    return (c == '\0') ? s : nullptr;
}

const char* findLastChar(const char* s, char c) noexcept {
    if (!s) return nullptr;
    const char* last = nullptr;
    while (*s != '\0') {
        if (*s == c) last = s;
        ++s;
    }
    return (c == '\0') ? s : last;
}

void uintToStr(u64 value, char* buf, u32 base, u32 minDigits) noexcept {
    if (!buf || base < 2 || base > 16) return;
    static constexpr char digits[] = "0123456789ABCDEF";
    char tmp[68];
    u32 pos = 0;

    if (value == 0) {
        tmp[pos++] = '0';
    } else {
        while (value > 0) {
            tmp[pos++] = digits[value % base];
            value /= base;
        }
    }

    while (pos < minDigits && pos < 64) {
        tmp[pos++] = '0';
    }

    for (u32 i = 0; i < pos; ++i) {
        buf[i] = tmp[pos - 1 - i];
    }
    buf[pos] = '\0';
}

void intToStr(i64 value, char* buf, u32 base) noexcept {
    if (!buf) return;
    if (value < 0 && base == 10) {
        *buf++ = '-';
        uintToStr(static_cast<u64>(-(value + 1)) + 1ULL, buf, base, 0);
    } else {
        uintToStr(static_cast<u64>(value), buf, base, 0);
    }
}

} // namespace cortez::str
