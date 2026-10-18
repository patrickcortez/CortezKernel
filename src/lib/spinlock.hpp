/**
 * @file spinlock.hpp
 * @brief Atomic spinlock and RAII interrupt-saving lock guard (spin_lock_irqsave).
 */

#pragma once

#include "types.hpp"
#include "../arch/x86_64/cpu.hpp"

namespace cortez {

/**
 * @brief Atomic test-and-test-and-set Spinlock for SMP and interrupt synchronization.
 */
class Spinlock final {
public:
    constexpr Spinlock() noexcept : m_locked(0) {}

    Spinlock(const Spinlock&) = delete;
    Spinlock& operator=(const Spinlock&) = delete;

    void lock() noexcept {
        while (__atomic_test_and_set(&m_locked, __ATOMIC_ACQUIRE)) {
            while (__atomic_load_n(&m_locked, __ATOMIC_RELAXED)) {
                arch::pause();
            }
        }
    }

    void unlock() noexcept {
        __atomic_clear(&m_locked, __ATOMIC_RELEASE);
    }

    u64 lockIrqSave() noexcept {
        u64 flags = arch::disableInterruptsSave();
        lock();
        return flags;
    }

    void unlockIrqRestore(u64 flags) noexcept {
        unlock();
        arch::restoreInterrupts(flags);
    }

private:
    volatile u8 m_locked;
};

/**
 * @brief RAII guard that disables local CPU interrupts and acquires a Spinlock.
 */
class IrqLockGuard final {
public:
    explicit IrqLockGuard(Spinlock& lock) noexcept
        : m_lock(lock), m_flags(m_lock.lockIrqSave()) {}

    ~IrqLockGuard() noexcept {
        m_lock.unlockIrqRestore(m_flags);
    }

    IrqLockGuard(const IrqLockGuard&) = delete;
    IrqLockGuard& operator=(const IrqLockGuard&) = delete;

private:
    Spinlock& m_lock;
    u64       m_flags;
};

} // namespace cortez
