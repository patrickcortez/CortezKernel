/**
 * @file ring_buffer.hpp
 * @brief Interrupt-safe circular buffer for character and input event streams.
 */

#pragma once

#include "../../lib/types.hpp"
#include "../../lib/spinlock.hpp"

namespace cortez::drivers::input {

template <typename T, usize Capacity>
class RingBuffer final {
public:
    constexpr RingBuffer() noexcept = default;

    bool push(const T& item) noexcept {
        IrqLockGuard guard(m_lock);
        usize nextHead = (m_head + 1) % Capacity;
        if (nextHead == m_tail) {
            return false; // Full
        }
        m_data[m_head] = item;
        m_head         = nextHead;
        return true;
    }

    bool pop(T* outItem) noexcept {
        IrqLockGuard guard(m_lock);
        if (m_head == m_tail) {
            return false; // Empty
        }
        if (outItem) {
            *outItem = m_data[m_tail];
        }
        m_tail = (m_tail + 1) % Capacity;
        return true;
    }

    bool isEmpty() const noexcept {
        IrqLockGuard guard(m_lock);
        return m_head == m_tail;
    }

    void clear() noexcept {
        IrqLockGuard guard(m_lock);
        m_head = 0;
        m_tail = 0;
    }

private:
    mutable Spinlock m_lock;
    T                m_data[Capacity]{};
    usize            m_head = 0;
    usize            m_tail = 0;
};

} // namespace cortez::drivers::input
