/**
 * @file task.hpp
 * @brief Process Control Block (Task / Linux task_struct equivalent) for kernel tasks.
 */

#pragma once

#include "../lib/types.hpp"
#include "../lib/string_utils.hpp"

namespace cortez::proc {

enum class TaskState : u8 {
    Running        = 0, // TASK_RUNNING (active on CPU)
    Ready          = 1, // TASK_RUNNING (runnable in queue)
    Interruptible  = 2, // TASK_INTERRUPTIBLE (waiting for I/O or timer)
    Stopped        = 3, // TASK_STOPPED
    Zombie         = 4  // EXIT_ZOMBIE
};

constexpr const char* taskStateToString(TaskState state) noexcept {
    switch (state) {
        case TaskState::Running:       return "R (running)";
        case TaskState::Ready:         return "R (runnable)";
        case TaskState::Interruptible: return "S (sleeping)";
        case TaskState::Stopped:       return "T (stopped)";
        case TaskState::Zombie:        return "Z (zombie)";
        default:                       return "? (unknown)";
    }
}

using TaskEntryFunc = void (*)();

/**
 * @brief Process Control Block (PCB) representing a kernel task/process.
 */
class Task final {
public:
    static constexpr usize KERNEL_STACK_SIZE = 32768; // 32 KiB per task stack

    Task() noexcept = default;

    void initBootTask(u32 pid, const char* name) noexcept {
        m_pid       = pid;
        m_state     = TaskState::Running;
        m_stackBase = nullptr;
        m_rsp       = 0;
        m_ticks     = 0;
        str::copy(m_name, name, sizeof(m_name));
        str::copy(m_cwd, "/", sizeof(m_cwd));
    }

    bool initKernelTask(u32 pid, const char* name, TaskEntryFunc entry) noexcept;

    u32         getPid() const noexcept { return m_pid; }
    const char* getName() const noexcept { return m_name; }
    TaskState   getState() const noexcept { return m_state; }
    void        setState(TaskState st) noexcept { m_state = st; }

    u64*        getRspPtr() noexcept { return &m_rsp; }
    u64         getRsp() const noexcept { return m_rsp; }
    u8*         getStackTop() const noexcept {
        return m_stackBase ? (m_stackBase + KERNEL_STACK_SIZE) : nullptr;
    }

    u64         getTicks() const noexcept { return m_ticks; }
    void        incrementTicks() noexcept { ++m_ticks; }

    const char* getCwd() const noexcept { return m_cwd; }
    void        setCwd(const char* newCwd) noexcept {
        str::copy(m_cwd, newCwd, sizeof(m_cwd));
    }

private:
    u32       m_pid   = 0;
    char      m_name[32]{};
    TaskState m_state = TaskState::Stopped;
    u8*       m_stackBase = nullptr;
    u64       m_rsp   = 0;
    u64       m_ticks = 0;
    char      m_cwd[256] = "/";
};

} // namespace cortez::proc
