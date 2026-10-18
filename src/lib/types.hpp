/**
 * @file types.hpp
 * @brief Core fixed-width primitive types and compiler attributes for the Cortez Kernel.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace cortez {

using u8      = uint8_t;
using u16     = uint16_t;
using u32     = uint32_t;
using u64     = uint64_t;

using i8      = int8_t;
using i16     = int16_t;
using i32     = int32_t;
using i64     = int64_t;

using usize   = size_t;
using isize   = ptrdiff_t;
using uintptr = uintptr_t;
using phys_addr_t = u64;
using virt_addr_t = u64;

constexpr usize PAGE_SIZE_4K = 4096ULL;
constexpr u64   PAGE_MASK_4K = ~(PAGE_SIZE_4K - 1ULL);

#define CORTEZ_PACKED      __attribute__((packed))
#define CORTEZ_ALIGNED(x)  __attribute__((aligned(x)))
#define CORTEZ_NORETURN    [[noreturn]]
#define CORTEZ_NODISCARD   [[nodiscard]]
#define CORTEZ_ALWAYS_INLINE inline __attribute__((always_inline))

constexpr u64 alignDown(u64 value, u64 alignment) noexcept {
    return value & ~(alignment - 1ULL);
}

constexpr u64 alignUp(u64 value, u64 alignment) noexcept {
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

} // namespace cortez
