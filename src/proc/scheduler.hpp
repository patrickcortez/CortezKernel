/**
 * @file scheduler.hpp
 * @brief Round-Robin Kernel Task Scheduler supporting dynamic runtime tasks, sleeping, and reaping.
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
    static constexpr usize MAX_TASKS = 32;

    static Scheduler& instance() noexcept;

    Status init(TaskEntryFunc initEntry, TaskEntryFunc kworkerEntry) noexcept;

    void  yield() noexcept override;
    Task* getCurrentTask() noexcept override { return &m_tasks[m_currentIdx]; }

    Result<u32> spawnTask(const char* name,
                          TaskArgEntryFunc fn,
                          void* arg = nullptr,
                          u32 ownerModuleId = 0,
                          usize stackSize = Task::KERNEL_STACK_SIZE) noexcept;
    Status      killTask(u32 pid) noexcept;
    usize       killModuleTasks(u32 ownerModuleId) noexcept;
    void        sleepMs(u64 milliseconds) noexcept;

    usize       getTaskCount() const noexcept { return MAX_TASKS; }
    const Task* getTaskAt(usize idx) const noexcept {
        if (idx >= MAX_TASKS) return nullptr;
        return (m_tasks[idx].getState() != TaskState::Stopped) ? &m_tasks[idx] : nullptr;
    }

private:
    Scheduler() noexcept = default;

    void reapZombiesAndWakeSleepers() noexcept;

    CORTEZ_ALIGNED(16) Task m_tasks[MAX_TASKS]{};
    usize m_taskCount  = 0;
    usize m_currentIdx = 0;
    u32   m_nextPid    = 1;
};

} // namespace cortez::proc
