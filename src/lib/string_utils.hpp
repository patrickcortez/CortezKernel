/**
 * @file string_utils.hpp
 * @brief Freestanding string and buffer utilities for the Cortez Kernel.
 */

#pragma once

#include "types.hpp"

extern "C" {
void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int val, size_t n);
void* memmove(void* dst, const void* src, size_t n);
int   memcmp(const void* s1, const void* s2, size_t n);
}

namespace cortez::str {

usize       length(const char* s) noexcept;
i32         compare(const char* a, const char* b) noexcept;
i32         compareN(const char* a, const char* b, usize maxLen) noexcept;
i32         compareCaseInsensitive(const char* a, const char* b) noexcept;
char*       copy(char* dst, const char* src, usize maxDstSize) noexcept;
char*       concat(char* dst, const char* src, usize maxDstSize) noexcept;
const char* findChar(const char* s, char c) noexcept;
const char* findLastChar(const char* s, char c) noexcept;
void        uintToStr(u64 value, char* buf, u32 base = 10, u32 minDigits = 0) noexcept;
void        intToStr(i64 value, char* buf, u32 base = 10) noexcept;
char        toUpper(char c) noexcept;
char        toLower(char c) noexcept;

} // namespace cortez::str
