/**
 * @file klibc.hpp
 * @brief Freestanding C standard library shim (setjmp, string, stdlib, stdio) for the compiler and C modules.
 */

#pragma once

#include "../lib/types.hpp"
#include <stdarg.h>

extern "C" {

using cortez_jmp_buf = uint64_t[8];

int  cortez_setjmp(cortez_jmp_buf env) noexcept;
void cortez_longjmp(cortez_jmp_buf env, int val) noexcept;

void* klibc_malloc(size_t size) noexcept;
void* klibc_calloc(size_t nmemb, size_t size) noexcept;
void* klibc_realloc(void* ptr, size_t size) noexcept;
void  klibc_free(void* ptr) noexcept;

size_t klibc_strlen(const char* s) noexcept;
char*  klibc_strcpy(char* dst, const char* src) noexcept;
char*  klibc_strncpy(char* dst, const char* src, size_t n) noexcept;
char*  klibc_strcat(char* dst, const char* src) noexcept;
int    klibc_strcmp(const char* a, const char* b) noexcept;
int    klibc_strncmp(const char* a, const char* b, size_t n) noexcept;
int    klibc_strcasecmp(const char* a, const char* b) noexcept;
char*  klibc_strchr(const char* s, int c) noexcept;
char*  klibc_strrchr(const char* s, int c) noexcept;
char*  klibc_strstr(const char* haystack, const char* needle) noexcept;
char*  klibc_strdup(const char* s) noexcept;

long          klibc_strtol(const char* nptr, char** endptr, int base) noexcept;
unsigned long klibc_strtoul(const char* nptr, char** endptr, int base) noexcept;
int           klibc_atoi(const char* nptr) noexcept;

int klibc_vsnprintf(char* buf, size_t size, const char* fmt, va_list args) noexcept;
int klibc_snprintf(char* buf, size_t size, const char* fmt, ...) noexcept;
int klibc_sprintf(char* buf, const char* fmt, ...) noexcept;

} // extern "C"
