/**
 * @file ps2_keyboard.hpp
 * @brief Laptop EC-safe i8042 PS/2 Keyboard Driver implementing IInterruptHandler and ICharDevice.
 */

#pragma once

#include "../i_device.hpp"
#include "../../arch/x86_64/idt.hpp"
#include "ring_buffer.hpp"

namespace cortez::drivers::input {

class Ps2KeyboardDriver final : public ICharDevice, public arch::IInterruptHandler {
public:
    static constexpr usize KEY_BUFFER_CAPACITY = 512;

    static Ps2KeyboardDriver& instance() noexcept;

    Status init() noexcept;

    const char*   getName() const noexcept override { return "kbd0"; }
    Result<usize> read(void* buffer, usize count) override;
    Result<usize> write(const void* buffer, usize count) override;
    bool          hasData() const noexcept override;

    void onInterrupt(arch::InterruptFrame* frame) noexcept override;
    void pollHardware() noexcept;
    char readCharBlocking() noexcept;

private:
    Ps2KeyboardDriver() noexcept = default;

    void processScancode(u8 scancode) noexcept;
    void pushSequence(const char* seq) noexcept;
    bool waitInputReady() const noexcept;
    bool waitOutputReady() const noexcept;

    RingBuffer<char, KEY_BUFFER_CAPACITY> m_buffer{};
    bool m_leftShift     = false;
    bool m_rightShift    = false;
    bool m_leftCtrlHeld  = false;
    bool m_rightCtrlHeld = false;
    bool m_ctrlHeld      = false;
    bool m_leftAltHeld   = false;
    bool m_rightAltHeld  = false;
    bool m_altHeld       = false;
    bool m_capsLock      = false;
    bool m_extendedE0    = false;
};

} // namespace cortez::drivers::input
