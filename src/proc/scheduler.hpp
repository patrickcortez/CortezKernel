/**
 * @file scheduler.hpp
 * @brief Round-Robin Kernel Task Scheduler (PID 0 swapper, PID 1 init, PID 2 kworker).
 */

#pragma once

#include "task.hpp"
#include "../lib/result.hpp"

namespace cortez::proc {

class IScheduler {
public:
    virtual ~IScheduler() = default;
    virtual void  yield() noexcept = 0;
    virtual Task* getCurrentTask() noexcept = 0;
};

class Scheduler final : public IScheduler {
public:
    static constexpr usize MAX_TASKS = 8;

    static Scheduler& instance() noexcept;

    Status init(TaskEntryFunc initEntry, TaskEntryFunc kworkerEntry) noexcept;

    void  yield() noexcept override;
    Task* getCurrentTask() noexcept override { return &m_tasks[m_currentIdx]; }

    usize       getTaskCount() const noexcept { return m_taskCount; }
    const Task* getTaskAt(usize idx) const noexcept {
        return (idx < m_taskCount) ? &m_tasks[idx] : nullptr;
    }

private:
    Scheduler() noexcept = default;

    Task  m_tasks[MAX_TASKS]{};
    usize m_taskCount  = 0;
    usize m_currentIdx = 0;
};

} // namespace cortez::proc
