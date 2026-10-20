/**
 * @file input_manager.hpp
 * @brief Unified Console Input Multiplexer coordinating PS/2 and USB Keyboards.
 */

#pragma once

#include "../i_device.hpp"
#include "../../lib/spinlock.hpp"
#include "ring_buffer.hpp"

namespace cortez::drivers::input {

class InputManager final : public ICharDevice {
public:
    static constexpr usize MAX_INPUT_DEVICES = 8;
    static constexpr usize BUFFER_CAPACITY   = 1024;

    static InputManager& instance() noexcept;

    Status registerDevice(ICharDevice* dev) noexcept;
    Status unregisterDevice(ICharDevice* dev) noexcept;

    // ICharDevice overrides
    const char*   getName() const noexcept override { return "cin"; }
    Result<usize> read(void* buffer, usize count) override;
    Result<usize> write(const void*, usize) override { return Status::NotSupported; }
    bool          hasData() const noexcept override;

    // Multiplexer methods
    void          pollHardware() noexcept;
    char          readCharBlocking() noexcept;
    void          pushChar(char c) noexcept;
    void          pushSequence(const char* seq) noexcept;

    usize         getDeviceCount() const noexcept { return m_deviceCount; }
    ICharDevice*  getDeviceAt(usize idx) const noexcept { return (idx < m_deviceCount) ? m_devices[idx] : nullptr; }

    Status        setMainKeyboard(const char* name) noexcept;
    ICharDevice*  getMainKeyboard() const noexcept { return m_mainKeyboard; }
    bool          isMainKeyboard(const ICharDevice* dev) const noexcept { return m_mainKeyboard == dev; }

private:
    InputManager() noexcept = default;

    Spinlock                              m_lock;
    RingBuffer<char, BUFFER_CAPACITY>     m_buffer{};
    ICharDevice*                          m_devices[MAX_INPUT_DEVICES]{};
    usize                                 m_deviceCount  = 0;
    ICharDevice*                          m_mainKeyboard = nullptr;
};

} // namespace cortez::drivers::input
