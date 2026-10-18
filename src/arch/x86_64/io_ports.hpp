/**
 * @file io_ports.hpp
 * @brief x86_64 legacy I/O port accessors (inb/outb, inw/outw, inl/outl).
 */

#pragma once

#include "../../lib/types.hpp"

namespace cortez::arch {

CORTEZ_ALWAYS_INLINE void outb(u16 port, u8 value) noexcept {
    asm volatile("outb %0, %1" :: "a"(value), "Nd"(port));
}

CORTEZ_ALWAYS_INLINE u8 inb(u16 port) noexcept {
    u8 ret = 0;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

CORTEZ_ALWAYS_INLINE void outw(u16 port, u16 value) noexcept {
    asm volatile("outw %0, %1" :: "a"(value), "Nd"(port));
}

CORTEZ_ALWAYS_INLINE u16 inw(u16 port) noexcept {
    u16 ret = 0;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

CORTEZ_ALWAYS_INLINE void outl(u16 port, u32 value) noexcept {
    asm volatile("outl %0, %1" :: "a"(value), "Nd"(port));
}

CORTEZ_ALWAYS_INLINE u32 inl(u16 port) noexcept {
    u32 ret = 0;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

CORTEZ_ALWAYS_INLINE void ioWait() noexcept {
    outb(0x80, 0);
}

} // namespace cortez::arch
