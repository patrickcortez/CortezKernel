/**
 * @file printk.cpp
 * @brief Implementation of the Linux-style printk logger with safe UART detection.
 */

#include "printk.hpp"
#include "string_utils.hpp"
#include "spinlock.hpp"
#include "../arch/x86_64/cpu.hpp"
#include "../arch/x86_64/io_ports.hpp"
#include <stdarg.h>

namespace cortez::klog {

namespace {
constexpr u16 COM1_PORT = 0x3F8;
Spinlock      s_logLock;

void formatArgs(void (*emit)(char, void*), void* ctx, const char* fmt, va_list args) noexcept {
    while (*fmt) {
        if (*fmt != '%') {
            emit(*fmt++, ctx);
            continue;
        }
        ++fmt; // skip '%'
        if (*fmt == '%') {
            emit('%', ctx);
            ++fmt;
            continue;
        }

        // Parse zero-padding and width
        u32 minDigits = 0;
        if (*fmt == '0') {
            ++fmt;
            while (*fmt >= '0' && *fmt <= '9') {
                minDigits = minDigits * 10 + static_cast<u32>(*fmt - '0');
                ++fmt;
            }
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                minDigits = minDigits * 10 + static_cast<u32>(*fmt - '0');
                ++fmt;
            }
        }

        // Parse length modifier ('l' or 'll' or 'z')
        bool isLongLong = false;
        if (*fmt == 'l') {
            ++fmt;
            isLongLong = true;
            if (*fmt == 'l') {
                ++fmt;
            }
        } else if (*fmt == 'z') {
            ++fmt;
            isLongLong = true;
        }

        char numBuf[72];
        switch (*fmt) {
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                while (*s) emit(*s++, ctx);
                break;
            }
            case 'c': {
                char c = static_cast<char>(va_arg(args, int));
                emit(c, ctx);
                break;
            }
            case 'd':
            case 'i': {
                i64 val = isLongLong ? va_arg(args, i64) : va_arg(args, int);
                str::intToStr(val, numBuf, 10);
                for (usize i = 0; numBuf[i]; ++i) emit(numBuf[i], ctx);
                break;
            }
            case 'u': {
                u64 val = isLongLong ? va_arg(args, u64) : va_arg(args, unsigned int);
                str::uintToStr(val, numBuf, 10, minDigits);
                for (usize i = 0; numBuf[i]; ++i) emit(numBuf[i], ctx);
                break;
            }
            case 'x':
            case 'X': {
                u64 val = isLongLong ? va_arg(args, u64) : va_arg(args, unsigned int);
                str::uintToStr(val, numBuf, 16, minDigits);
                for (usize i = 0; numBuf[i]; ++i) {
                    char ch = (*fmt == 'x') ? str::toLower(numBuf[i]) : numBuf[i];
                    emit(ch, ctx);
                }
                break;
            }
            case 'p': {
                u64 val = reinterpret_cast<u64>(va_arg(args, void*));
                emit('0', ctx);
                emit('x', ctx);
                str::uintToStr(val, numBuf, 16, 16);
                for (usize i = 0; numBuf[i]; ++i) emit(str::toLower(numBuf[i]), ctx);
                break;
            }
            default:
                emit('%', ctx);
                if (*fmt) emit(*fmt, ctx);
                break;
        }
        if (*fmt) ++fmt;
    }
}
} // namespace

Logger& Logger::instance() noexcept {
    static Logger s_logger;
    return s_logger;
}

void Logger::initSerial() noexcept {
    m_bootTsc = arch::rdtsc();

    // Probe 16550 UART scratch register to verify COM1 presence on real laptops
    arch::outb(COM1_PORT + 7, 0x5A);
    if (arch::inb(COM1_PORT + 7) != 0x5A) {
        m_serialReady = false;
        return;
    }

    arch::outb(COM1_PORT + 1, 0x00); // Disable interrupts
    arch::outb(COM1_PORT + 3, 0x80); // Enable DLAB
    arch::outb(COM1_PORT + 0, 0x01); // 115200 baud (divisor 1)
    arch::outb(COM1_PORT + 1, 0x00);
    arch::outb(COM1_PORT + 3, 0x03); // 8 bits, no parity, one stop bit
    arch::outb(COM1_PORT + 2, 0xC7); // Enable FIFO
    arch::outb(COM1_PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
    m_serialReady = true;
}

void Logger::setConsoleDevice(drivers::ICharDevice* console) noexcept {
    m_console = console;
}

void Logger::appendCharToBuffer(char c) noexcept {
    if (m_writePos + 1 < LOG_BUFFER_SIZE) {
        m_buffer[m_writePos++] = c;
        m_buffer[m_writePos]   = '\0';
    }
}

void Logger::putSerialChar(char c) noexcept {
    if (!m_serialReady) return;
    for (u32 spin = 0; spin < 4096; ++spin) {
        if (arch::inb(COM1_PORT + 5) & 0x20) {
            arch::outb(COM1_PORT, static_cast<u8>(c));
            break;
        }
    }
}

void Logger::putChar(char c) noexcept {
    if (c == '\n') {
        putSerialChar('\r');
    }
    putSerialChar(c);
    if (m_console) {
        (void)m_console->write(&c, 1);
    }
}

void Logger::writeRaw(const char* str, usize len) noexcept {
    for (usize i = 0; i < len; ++i) {
        putChar(str[i]);
    }
}

void Logger::log(Level level, const char* subsystem, const char* fmt, ...) noexcept {
    IrqLockGuard guard(s_logLock);

    // Approximate microsecond timestamp from TSC (~2.5 GHz nominal baseline)
    u64 deltaTsc = arch::rdtsc() - m_bootTsc;
    u64 totalUs  = deltaTsc / 2500ULL;
    u64 secs     = totalUs / 1000000ULL;
    u64 usecs    = totalUs % 1000000ULL;

    auto emitBoth = [](char c, void* selfPtr) {
        auto* self = static_cast<Logger*>(selfPtr);
        self->appendCharToBuffer(c);
        self->putChar(c);
    };

    auto emitConsoleOnly = [](char c, void* selfPtr) {
        auto* self = static_cast<Logger*>(selfPtr);
        self->putChar(c);
    };

    auto writeStr = [&](const char* s, bool recordInBuffer) {
        while (*s) {
            if (recordInBuffer) appendCharToBuffer(*s);
            putChar(*s++);
        }
    };

    // ANSI color prefix for terminal, plain text for dmesg buffer
    writeStr("\033[90m[", false);
    appendCharToBuffer('[');

    char timeBuf[32];
    str::uintToStr(secs, timeBuf, 10, 5);
    writeStr(timeBuf, true);
    writeStr(".", true);
    str::uintToStr(usecs, timeBuf, 10, 6);
    writeStr(timeBuf, true);
    writeStr("]\033[0m ", false);
    appendCharToBuffer(']');
    appendCharToBuffer(' ');

    switch (level) {
        case Level::Info:  writeStr("\033[36m", false); break;
        case Level::Ok:    writeStr("\033[32m", false); break;
        case Level::Warn:  writeStr("\033[33m", false); break;
        case Level::Error: writeStr("\033[31m", false); break;
    }

    if (subsystem && subsystem[0]) {
        writeStr(subsystem, true);
        writeStr(": ", true);
    }
    writeStr("\033[0m", false);

    va_list args;
    va_start(args, fmt);
    formatArgs(emitBoth, this, fmt, args);
    va_end(args);

    emitBoth('\n', this);
    (void)emitConsoleOnly;
}

void Logger::printf(const char* fmt, ...) noexcept {
    IrqLockGuard guard(s_logLock);
    auto emitConsole = [](char c, void* selfPtr) {
        static_cast<Logger*>(selfPtr)->putChar(c);
    };
    va_list args;
    va_start(args, fmt);
    formatArgs(emitConsole, this, fmt, args);
    va_end(args);
}

} // namespace cortez::klog
