/**
 * @file scheduler.cpp
 * @brief Implementation of Task stack frame setup and Round-Robin Scheduler.
 */

#include "scheduler.hpp"
#include "../arch/x86_64/cpu.hpp"
#include "../arch/x86_64/gdt.hpp"
#include "../lib/printk.hpp"

extern "C" void cortez_switch_context(uint64_t* prevRspOut, uint64_t nextRsp);

namespace cortez::proc {

namespace {
void kernelTaskTrampoline() {
    arch::sti();
    TaskEntryFunc fn = nullptr;
    asm volatile("mov %%r12, %0" : "=r"(fn));
    if (fn) {
        fn();
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

bool Task::initKernelTask(u32 pid, const char* name, TaskEntryFunc entry) noexcept {
    m_pid   = pid;
    m_state = TaskState::Ready;
    m_ticks = 0;
    str::copy(m_name, name ? name : "ktask", sizeof(m_name));
    str::copy(m_cwd, "/", sizeof(m_cwd));

    m_stackBase = new u8[KERNEL_STACK_SIZE];
    if (!m_stackBase) return false;

    memset(m_stackBase, 0, KERNEL_STACK_SIZE);

    u64* sp = reinterpret_cast<u64*>(m_stackBase + KERNEL_STACK_SIZE);
    // Align stack and push initial return address + callee-saved registers (rbp, rbx, r12..r15)
    *(--sp) = 0; // Stack alignment sentinel
    *(--sp) = reinterpret_cast<u64>(&kernelTaskTrampoline); // rip popped by ret
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    *(--sp) = reinterpret_cast<u64>(entry); // r12 -> entry function pointer
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
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

    // PID 0: swapper/0 (current bootstrap stack)
    m_tasks[0].initBootTask(0, "swapper/0");
    m_taskCount = 1;

    // PID 1: init (system initialization & Cortez Shell)
    if (initEntry) {
        if (!m_tasks[m_taskCount].initKernelTask(1, "init", initEntry)) {
            return Status::OutOfMemory;
        }
        ++m_taskCount;
    }

    // PID 2: kworker/0 (background disk sync & housekeeping task)
    if (kworkerEntry) {
        if (!m_tasks[m_taskCount].initKernelTask(2, "kworker/0", kworkerEntry)) {
            return Status::OutOfMemory;
        }
        ++m_taskCount;
    }

    pr_ok("sched", "Initialized Round-Robin Scheduler (%zu kernel tasks: PID 0..%zu)",
          m_taskCount, m_taskCount - 1);
    return Status::Ok;
}

void Scheduler::yield() noexcept {
    if (m_taskCount <= 1) return;

    usize prevIdx = m_currentIdx;
    usize nextIdx = prevIdx;

    for (usize step = 1; step <= m_taskCount; ++step) {
        usize candidate = (prevIdx + step) % m_taskCount;
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

    cortez_switch_context(m_tasks[prevIdx].getRspPtr(), m_tasks[nextIdx].getRsp());
}

} // namespace cortez::proc
