/**
 * @file icxxabi.cpp
 * @brief Itanium C++ ABI runtime support and freestanding compiler intrinsics.
 */

#include "../lib/types.hpp"
#include "../arch/x86_64/cpu.hpp"

extern "C" {

using ctor_func_t = void (*)();
extern ctor_func_t __init_array_start[];
extern ctor_func_t __init_array_end[];

void* __dso_handle = nullptr;

void __cortez_call_global_constructors() {
    for (ctor_func_t* fn = __init_array_start; fn < __init_array_end; ++fn) {
        if (*fn) {
            (*fn)();
        }
    }
}

int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}

void __cxa_pure_virtual() {
    cortez::arch::haltForever();
}

int __cxa_guard_acquire(uint64_t* guard) {
    uint8_t* initialized = reinterpret_cast<uint8_t*>(guard);
    if (__atomic_load_n(initialized, __ATOMIC_ACQUIRE) != 0) {
        return 0;
    }
    return 1;
}

void __cxa_guard_release(uint64_t* guard) {
    uint8_t* initialized = reinterpret_cast<uint8_t*>(guard);
    __atomic_store_n(initialized, 1, __ATOMIC_RELEASE);
}

void __cxa_guard_abort(uint64_t*) {}

void* memcpy(void* dst, const void* src, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    while (n >= 8 && ((reinterpret_cast<uintptr_t>(d) & 7) == 0) &&
           ((reinterpret_cast<uintptr_t>(s) & 7) == 0)) {
        *reinterpret_cast<uint64_t*>(d) = *reinterpret_cast<const uint64_t*>(s);
        d += 8;
        s += 8;
        n -= 8;
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void* memset(void* dst, int val, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    uint8_t byteVal = static_cast<uint8_t>(val);
    uint64_t wordVal = static_cast<uint64_t>(byteVal) * 0x0101010101010101ULL;
    while (n >= 8 && ((reinterpret_cast<uintptr_t>(d) & 7) == 0)) {
        *reinterpret_cast<uint64_t*>(d) = wordVal;
        d += 8;
        n -= 8;
    }
    while (n--) {
        *d++ = byteVal;
    }
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        return memcpy(dst, src, n);
    }
    if (d > s) {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const auto* a = static_cast<const uint8_t*>(s1);
    const auto* b = static_cast<const uint8_t*>(s2);
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return static_cast<int>(a[i]) - static_cast<int>(b[i]);
        }
    }
    return 0;
}

} // extern "C"
