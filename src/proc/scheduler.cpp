/**
 * @file scheduler.cpp
 * @brief Implementation of Task stack frame setup, dynamic spawning, reaping, and Round-Robin Scheduler.
 */

#include "scheduler.hpp"
#include "../arch/x86_64/cpu.hpp"
#include "../arch/x86_64/gdt.hpp"
#include "../arch/x86_64/apic.hpp"
#include "../lib/printk.hpp"

extern "C" void cortez_switch_context(uint64_t* prevRspOut,
                                      uint64_t nextRsp,
                                      void* prevFx,
                                      const void* nextFx);

namespace cortez::proc {

namespace {
void kernelTaskTrampoline() {
    arch::sti();
    TaskArgEntryFunc fn  = nullptr;
    void*            arg = nullptr;
    u64              isArgTask = 0;
    asm volatile("mov %%r12, %0\n\t"
                 "mov %%r13, %1\n\t"
                 "mov %%r14, %2"
                 : "=r"(fn), "=r"(arg), "=r"(isArgTask));
    if (fn) {
        if (isArgTask) {
            fn(arg);
        } else {
            reinterpret_cast<TaskEntryFunc>(fn)();
        }
    }
    Task* curr = Scheduler::instance().getCurrentTask();
    if (curr) {
        curr->setState(TaskState::Zombie);
    }
    for (;;) {
        Scheduler::instance().yield();
        arch::hlt();
    }
}
} // namespace

void Task::cleanupStack() noexcept {
    if (m_stackBase) {
        delete[] m_stackBase;
        m_stackBase = nullptr;
    }
    m_stackSize     = 0;
    m_rsp           = 0;
    m_ownerModuleId = 0;
    m_wakeTickMs    = 0;
    m_state         = TaskState::Stopped;
}

bool Task::initKernelTask(u32 pid, const char* name, TaskEntryFunc entry, usize stackSize) noexcept {
    return initKernelArgTask(pid,
                             name,
                             reinterpret_cast<TaskArgEntryFunc>(entry),
                             nullptr,
                             0,
                             stackSize);
}

bool Task::initKernelArgTask(u32 pid,
                             const char* name,
                             TaskArgEntryFunc entry,
                             void* arg,
                             u32 ownerModuleId,
                             usize stackSize) noexcept {
    cleanupStack();

    if (stackSize < 16384) stackSize = 16384;
    stackSize = static_cast<usize>(alignUp(stackSize, 16));

    m_stackBase = new u8[stackSize];
    if (!m_stackBase) return false;

    memset(m_stackBase, 0, stackSize);

    m_pid           = pid;
    m_ownerModuleId = ownerModuleId;
    m_state         = TaskState::Ready;
    m_stackSize     = stackSize;
    m_ticks         = 0;
    m_wakeTickMs    = 0;
    str::copy(m_name, name ? name : "ktask", sizeof(m_name));
    str::copy(m_cwd, "/", sizeof(m_cwd));
    arch::fxsave(m_fxState);

    u64* sp = reinterpret_cast<u64*>(m_stackBase + m_stackSize);
    // Align stack and push initial return address + callee-saved registers (rbp, rbx, r12..r15)
    *(--sp) = 0; // Stack alignment sentinel
    *(--sp) = reinterpret_cast<u64>(&kernelTaskTrampoline); // rip popped by ret
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = reinterpret_cast<u64>(entry); // r12 -> entry function pointer
    *(--sp) = reinterpret_cast<u64>(arg);   // r13 -> void* argument
    *(--sp) = (ownerModuleId != 0 || arg != nullptr) ? 1ULL : 0ULL; // r14 -> isArgTask flag
    *(--sp) = 0; // r15

    m_rsp = reinterpret_cast<u64>(sp);
    return true;
}

Scheduler& Scheduler::instance() noexcept {
    static Scheduler s_sched;
    return s_sched;
}

Status Scheduler::init(TaskEntryFunc initEntry, TaskEntryFunc kworkerEntry) noexcept {
    m_taskCount  = 0;
    m_currentIdx = 0;
    m_nextPid    = 1;

    // PID 0: swapper/0 (current bootstrap stack)
    m_tasks[0].initBootTask(0, "swapper/0");
    m_taskCount = 1;

    // PID 1: init (system initialization, Cortez Shell & C Compiler -> 512 KiB stack)
    if (initEntry) {
        if (!m_tasks[m_taskCount].initKernelTask(m_nextPid++, "init", initEntry, Task::SHELL_STACK_SIZE)) {
            return Status::OutOfMemory;
        }
        ++m_taskCount;
    }

    // PID 2: kworker/0 (background disk sync & housekeeping task)
    if (kworkerEntry) {
        if (!m_tasks[m_taskCount].initKernelTask(m_nextPid++, "kworker/0", kworkerEntry, Task::KERNEL_STACK_SIZE)) {
            return Status::OutOfMemory;
        }
        ++m_taskCount;
    }

    pr_ok("sched", "Initialized Round-Robin Scheduler (%zu boot tasks, PID 1 stack=%zu KiB)",
          m_taskCount, Task::SHELL_STACK_SIZE / 1024);
    return Status::Ok;
}

Result<u32> Scheduler::spawnTask(const char* name,
                                 TaskArgEntryFunc fn,
                                 void* arg,
                                 u32 ownerModuleId,
                                 usize stackSize) noexcept {
    if (!fn) return Status::InvalidArgument;

    u64 flags = arch::disableInterruptsSave();
    reapZombiesAndWakeSleepers();

    usize freeSlot = MAX_TASKS;
    for (usize i = 1; i < MAX_TASKS; ++i) {
        if (m_tasks[i].getState() == TaskState::Stopped) {
            freeSlot = i;
            break;
        }
    }

    if (freeSlot >= MAX_TASKS) {
        arch::restoreInterrupts(flags);
        return Status::NoSpace;
    }

    u32 newPid = m_nextPid++;
    bool ok = m_tasks[freeSlot].initKernelArgTask(newPid, name, fn, arg, ownerModuleId, stackSize);
    if (ok && freeSlot >= m_taskCount) {
        m_taskCount = freeSlot + 1;
    }
    arch::restoreInterrupts(flags);

    if (!ok) return Status::OutOfMemory;
    return newPid;
}

Status Scheduler::killTask(u32 pid) noexcept {
    if (pid <= 2) return Status::InvalidArgument; // Protect swapper, init, kworker

    u64 flags = arch::disableInterruptsSave();
    for (usize i = 0; i < MAX_TASKS; ++i) {
        if (m_tasks[i].getState() != TaskState::Stopped && m_tasks[i].getPid() == pid) {
            if (i == m_currentIdx) {
                m_tasks[i].setState(TaskState::Zombie);
                arch::restoreInterrupts(flags);
                yield();
                return Status::Ok;
            }
            m_tasks[i].cleanupStack();
            arch::restoreInterrupts(flags);
            return Status::Ok;
        }
    }
    arch::restoreInterrupts(flags);
    return Status::NotFound;
}

usize Scheduler::killModuleTasks(u32 ownerModuleId) noexcept {
    if (ownerModuleId == 0) return 0;

    u64 flags   = arch::disableInterruptsSave();
    usize count = 0;
    for (usize i = 0; i < MAX_TASKS; ++i) {
        if (m_tasks[i].getState() != TaskState::Stopped &&
            m_tasks[i].getOwnerModuleId() == ownerModuleId &&
            i != m_currentIdx) {
            m_tasks[i].cleanupStack();
            ++count;
        }
    }
    arch::restoreInterrupts(flags);
    return count;
}

void Scheduler::sleepMs(u64 milliseconds) noexcept {
    if (milliseconds == 0) {
        yield();
        return;
    }
    u64 nowMs = arch::ApicController::instance().getUptimeMs();
    Task* curr = getCurrentTask();
    if (!curr || m_currentIdx == 0) {
        arch::ApicController::instance().sleepMs(milliseconds);
        return;
    }
    curr->setWakeTickMs(nowMs + milliseconds);
    curr->setState(TaskState::Interruptible);
    while (curr->getState() == TaskState::Interruptible) {
        yield();
        if (curr->getState() == TaskState::Interruptible) {
            if (arch::interruptsEnabled()) {
                arch::hlt();
            } else {
                arch::pause();
            }
        }
    }
}

void Scheduler::reapZombiesAndWakeSleepers() noexcept {
    u64 nowMs = arch::ApicController::instance().getUptimeMs();
    for (usize i = 0; i < MAX_TASKS; ++i) {
        if (i == m_currentIdx) continue;
        TaskState st = m_tasks[i].getState();
        if (st == TaskState::Zombie) {
            m_tasks[i].cleanupStack();
        } else if (st == TaskState::Interruptible) {
            u64 wake = m_tasks[i].getWakeTickMs();
            if (wake > 0 && nowMs >= wake) {
                m_tasks[i].setWakeTickMs(0);
                m_tasks[i].setState(TaskState::Ready);
            }
        }
    }
}

void Scheduler::yield() noexcept {
    reapZombiesAndWakeSleepers();

    usize prevIdx = m_currentIdx;
    usize nextIdx = prevIdx;

    for (usize step = 1; step <= MAX_TASKS; ++step) {
        usize candidate = (prevIdx + step) % MAX_TASKS;
        TaskState st    = m_tasks[candidate].getState();
        if (st == TaskState::Ready || st == TaskState::Running) {
            nextIdx = candidate;
            break;
        }
    }

    m_tasks[prevIdx].incrementTicks();
    if (nextIdx == prevIdx) return;

    if (m_tasks[prevIdx].getState() == TaskState::Running) {
        m_tasks[prevIdx].setState(TaskState::Ready);
    }
    m_tasks[nextIdx].setState(TaskState::Running);
    m_currentIdx = nextIdx;

    if (u8* top = m_tasks[nextIdx].getStackTop()) {
        arch::GdtManager::instance().setKernelStack(reinterpret_cast<virt_addr_t>(top));
    }

    cortez_switch_context(m_tasks[prevIdx].getRspPtr(),
                          m_tasks[nextIdx].getRsp(),
                          m_tasks[prevIdx].getFxState(),
                          m_tasks[nextIdx].getFxState());

    reapZombiesAndWakeSleepers();
}

} // namespace cortez::proc
