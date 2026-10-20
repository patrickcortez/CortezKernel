/**
 * @file input_manager.cpp
 * @brief Implementation of the Unified Console Input Multiplexer.
 */

#include "input_manager.hpp"
#include "ps2_keyboard.hpp"
#include "../usb/xhci_controller.hpp"
#include "../../proc/scheduler.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::input {

InputManager& InputManager::instance() noexcept {
    static InputManager s_mgr;
    return s_mgr;
}

Status InputManager::registerDevice(ICharDevice* dev) noexcept {
    if (!dev) return Status::InvalidArgument;

    IrqLockGuard guard(m_lock);
    for (usize i = 0; i < m_deviceCount; ++i) {
        if (m_devices[i] == dev) {
            return Status::AlreadyExists;
        }
    }

    if (m_deviceCount >= MAX_INPUT_DEVICES) {
        return Status::NoSpace;
    }

    m_devices[m_deviceCount++] = dev;

    // Designate the first detected keyboard as MAIN keyboard.
    // If PS/2 (kbd0) registered first during early boot, the first detected USB keyboard
    // upgrades to MAIN keyboard. Once a USB keyboard is set as MAIN, any subsequent keyboard
    // or other device is treated as secondary and WILL NOT replace the MAIN keyboard.
    bool isUsbKbd = (str::compareN(dev->getName(), "ukbd", 4) == 0);
    if (!m_mainKeyboard) {
        m_mainKeyboard = dev;
        pr_ok("cin", "Designated '%s' as the MAIN keyboard (console stdin active)", dev->getName());
    } else if (isUsbKbd && str::compare(m_mainKeyboard->getName(), "kbd0") == 0) {
        m_mainKeyboard = dev;
        pr_ok("cin", "Detected first USB keyboard '%s'; designated as MAIN keyboard (replacing PS/2 fallback)", dev->getName());
    } else {
        pr_info("cin", "Registered secondary input device '%s' (MAIN keyboard remains '%s')",
                dev->getName(), m_mainKeyboard->getName());
    }

    return Status::Ok;
}

Status InputManager::unregisterDevice(ICharDevice* dev) noexcept {
    if (!dev) return Status::InvalidArgument;

    IrqLockGuard guard(m_lock);
    for (usize i = 0; i < m_deviceCount; ++i) {
        if (m_devices[i] == dev) {
            for (usize j = i; j + 1 < m_deviceCount; ++j) {
                m_devices[j] = m_devices[j + 1];
            }
            m_devices[--m_deviceCount] = nullptr;

            if (m_mainKeyboard == dev) {
                // Main keyboard detached: switch to another active USB keyboard if available,
                // or fall back to PS/2 (kbd0), or the first remaining registered device.
                ICharDevice* fallback = nullptr;
                for (usize k = 0; k < m_deviceCount; ++k) {
                    if (m_devices[k] && str::compareN(m_devices[k]->getName(), "ukbd", 4) == 0) {
                        fallback = m_devices[k];
                        break;
                    }
                }
                if (!fallback && m_deviceCount > 0) {
                    fallback = m_devices[0];
                }
                m_mainKeyboard = fallback;
                if (m_mainKeyboard) {
                    pr_warn("cin", "MAIN keyboard '%s' detached; switched MAIN keyboard to '%s'",
                            dev->getName(), m_mainKeyboard->getName());
                } else {
                    pr_warn("cin", "MAIN keyboard '%s' detached; no input devices remaining", dev->getName());
                }
            } else {
                pr_info("cin", "Unregistered input device '%s' (remaining %zu)", dev->getName(), m_deviceCount);
            }
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

Status InputManager::setMainKeyboard(const char* name) noexcept {
    if (!name || !name[0]) return Status::InvalidArgument;

    IrqLockGuard guard(m_lock);
    for (usize i = 0; i < m_deviceCount; ++i) {
        if (m_devices[i] && str::compare(m_devices[i]->getName(), name) == 0) {
            m_mainKeyboard = m_devices[i];
            pr_ok("cin", "Switched MAIN keyboard to '%s'", m_mainKeyboard->getName());
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

void InputManager::pushChar(char c) noexcept {
    IrqLockGuard guard(m_lock);
    m_buffer.push(c);
}

void InputManager::pushSequence(const char* seq) noexcept {
    if (!seq) return;
    IrqLockGuard guard(m_lock);
    while (*seq) {
        m_buffer.push(*seq++);
    }
}

void InputManager::pollHardware() noexcept {
    // 1. Poll legacy PS/2 hardware if initialized
    Ps2KeyboardDriver::instance().pollHardware();

    // 2. Poll xHCI event ring and ports for hotplug and USB keyboard transfers
    usb::XhciManager::instance().pollEventsAndPorts();

    // 3. Drain input strictly from the designated MAIN keyboard into m_buffer
    ICharDevice* mainDev = nullptr;
    {
        IrqLockGuard guard(m_lock);
        mainDev = m_mainKeyboard;
    }

    if (mainDev) {
        while (mainDev->hasData()) {
            char c = 0;
            auto res = mainDev->read(&c, 1);
            if (res.isOk() && res.value() > 0) {
                IrqLockGuard guard(m_lock);
                m_buffer.push(c);
            } else {
                break;
            }
        }
    }

    // 4. Drain and discard pending data from secondary keyboards to prevent
    //    stale buffer accumulation and prevent duplicate keystrokes (e.g. QEMU dual PS/2 + USB)
    for (usize i = 0; i < m_deviceCount; ++i) {
        ICharDevice* dev = m_devices[i];
        if (!dev || dev == mainDev) continue;

        while (dev->hasData()) {
            char discard = 0;
            auto res = dev->read(&discard, 1);
            if (!res.isOk() || res.value() == 0) break;
        }
    }
}

bool InputManager::hasData() const noexcept {
    if (!m_buffer.isEmpty()) return true;

    if (m_mainKeyboard && m_mainKeyboard->hasData()) {
        return true;
    }
    return false;
}

char InputManager::readCharBlocking() noexcept {
    for (;;) {
        {
            IrqLockGuard guard(m_lock);
            char c = 0;
            if (m_buffer.pop(&c)) {
                return c;
            }
        }

        pollHardware();

        {
            IrqLockGuard guard(m_lock);
            char c = 0;
            if (m_buffer.pop(&c)) {
                return c;
            }
        }

        proc::Scheduler::instance().yield();
        arch::pause();
    }
}

Result<usize> InputManager::read(void* buffer, usize count) {
    if (!buffer || count == 0) return 0;
    auto* out = static_cast<char*>(buffer);

    pollHardware();

    IrqLockGuard guard(m_lock);
    usize bytesRead = 0;
    while (bytesRead < count) {
        char c = 0;
        if (!m_buffer.pop(&c)) {
            break;
        }
        out[bytesRead++] = c;
    }

    return bytesRead;
}

} // namespace cortez::drivers::input
