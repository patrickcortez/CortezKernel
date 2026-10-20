/**
 * @file printk.hpp
 * @brief Linux-style kernel logger (printk / klog) with timestamps and disk flush support.
 */

#pragma once

#include "types.hpp"
#include "../drivers/i_device.hpp"

namespace cortez::klog {

enum class Level : u8 {
    Debug = 0,
    Info  = 1,
    Ok    = 2,
    Warn  = 3,
    Error = 4
};

/**
 * @brief Singleton kernel logger managing early serial, GOP console, and dmesg buffer.
 */
class Logger final {
public:
    static constexpr usize LOG_BUFFER_SIZE = 32768;

    static Logger& instance() noexcept;

    void initSerial() noexcept;
    void setConsoleDevice(drivers::ICharDevice* console) noexcept;

    void log(Level level, const char* subsystem, const char* fmt, ...) noexcept;
    void printf(const char* fmt, ...) noexcept;
    void putChar(char c) noexcept;
    void writeRaw(const char* str, usize len) noexcept;

    const char* getBuffer() const noexcept { return m_buffer; }
    usize       getBufferLength() const noexcept { return m_writePos; }

private:
    Logger() noexcept = default;

    void appendCharToBuffer(char c) noexcept;
    void putSerialChar(char c) noexcept;

    drivers::ICharDevice* m_console       = nullptr;
    bool                  m_serialReady   = false;
    u64                   m_bootTsc       = 0;
    usize                 m_writePos      = 0;
    char                  m_buffer[LOG_BUFFER_SIZE]{};
};

} // namespace cortez::klog

#define pr_dbg(subsys, fmt, ...) \
    ::cortez::klog::Logger::instance().log(::cortez::klog::Level::Debug, subsys, fmt, ##__VA_ARGS__)

#define pr_info(subsys, fmt, ...) \
    ::cortez::klog::Logger::instance().log(::cortez::klog::Level::Info, subsys, fmt, ##__VA_ARGS__)

#define pr_ok(subsys, fmt, ...) \
    ::cortez::klog::Logger::instance().log(::cortez::klog::Level::Ok, subsys, fmt, ##__VA_ARGS__)

#define pr_warn(subsys, fmt, ...) \
    ::cortez::klog::Logger::instance().log(::cortez::klog::Level::Warn, subsys, fmt, ##__VA_ARGS__)

#define pr_err(subsys, fmt, ...) \
    ::cortez::klog::Logger::instance().log(::cortez::klog::Level::Error, subsys, fmt, ##__VA_ARGS__)

#define kprintf(fmt, ...) \
    ::cortez::klog::Logger::instance().printf(fmt, ##__VA_ARGS__)
