/**
 * @file module_area.cpp
 * @brief Implementation of the Higher-Half Executable Module Memory Allocator.
 */

#include "module_area.hpp"
#include "../lib/string_utils.hpp"
#include "../lib/printk.hpp"

extern "C" const char __kernel_end[];

namespace cortez::memory {

ModuleArea& ModuleArea::instance() noexcept {
    static ModuleArea s_area;
    return s_area;
}

Status ModuleArea::init(IPhysicalAllocator* pmm, IVirtualMapper* vmm) noexcept {
    if (!pmm || !vmm) return Status::InvalidArgument;

    virt_addr_t kernelEnd = reinterpret_cast<virt_addr_t>(&__kernel_end[0]);
    if (kernelEnd >= MODULE_VIRT_BASE) {
        pr_err("modmem", "Kernel image end (0x%llx) overlaps MODULE_VIRT_BASE (0x%llx)",
               kernelEnd, MODULE_VIRT_BASE);
        return Status::NoSpace;
    }

    m_pmm       = pmm;
    m_vmm       = vmm;
    m_usedBytes = 0;
    for (usize i = 0; i < MAX_ALLOCATIONS; ++i) {
        m_allocations[i] = Allocation{ false, 0, 0 };
    }

    pr_ok("modmem", "Initialized Executable Module Region [0x%llx .. 0x%llx] (kernel_end=0x%llx)",
          MODULE_VIRT_BASE, MODULE_VIRT_END, kernelEnd);
    return Status::Ok;
}

bool ModuleArea::isRangeOverlapping(virt_addr_t base, usize bytes) const noexcept {
    virt_addr_t end = base + bytes;
    for (usize i = 0; i < MAX_ALLOCATIONS; ++i) {
        if (!m_allocations[i].inUse) continue;
        virt_addr_t aStart = m_allocations[i].virtBase;
        virt_addr_t aEnd   = aStart + m_allocations[i].sizeBytes + PAGE_SIZE_4K; // Include guard page
        if (base < aEnd && end > aStart) {
            return true;
        }
    }
    return false;
}

void* ModuleArea::allocate(usize sizeBytes) noexcept {
    if (sizeBytes == 0 || !m_pmm || !m_vmm) return nullptr;

    usize alignedBytes = static_cast<usize>(alignUp(sizeBytes, PAGE_SIZE_4K));
    usize pageCount    = alignedBytes / PAGE_SIZE_4K;

    IrqLockGuard guard(m_lock);

    usize slot = MAX_ALLOCATIONS;
    for (usize i = 0; i < MAX_ALLOCATIONS; ++i) {
        if (!m_allocations[i].inUse) {
            slot = i;
            break;
        }
    }
    if (slot >= MAX_ALLOCATIONS) return nullptr;

    // First-fit scan in [MODULE_VIRT_BASE, MODULE_VIRT_END)
    virt_addr_t candidate = MODULE_VIRT_BASE;
    bool found            = false;
    while (candidate + alignedBytes + PAGE_SIZE_4K <= MODULE_VIRT_END) {
        if (!isRangeOverlapping(candidate, alignedBytes + PAGE_SIZE_4K)) {
            found = true;
            break;
        }
        candidate += PAGE_SIZE_4K;
    }
    if (!found) return nullptr;

    for (usize p = 0; p < pageCount; ++p) {
        auto physRes = m_pmm->allocPages(1);
        if (physRes.isError()) {
            // Rollback pages mapped so far
            for (usize r = 0; r < p; ++r) {
                virt_addr_t rv = candidate + r * PAGE_SIZE_4K;
                auto tp        = m_vmm->translateVirtToPhys(rv);
                (void)m_vmm->unmapPage(rv);
                if (tp.isOk()) {
                    m_pmm->freePages(tp.value(), 1);
                }
            }
            return nullptr;
        }

        // Map as kernel RWX (PAGE_PRESENT | PAGE_WRITABLE, without PAGE_NO_EXECUTE)
        Status mst = m_vmm->mapPage(candidate + p * PAGE_SIZE_4K,
                                    physRes.value(),
                                    PAGE_PRESENT | PAGE_WRITABLE);
        if (mst != Status::Ok) {
            m_pmm->freePages(physRes.value(), 1);
            for (usize r = 0; r < p; ++r) {
                virt_addr_t rv = candidate + r * PAGE_SIZE_4K;
                auto tp        = m_vmm->translateVirtToPhys(rv);
                (void)m_vmm->unmapPage(rv);
                if (tp.isOk()) {
                    m_pmm->freePages(tp.value(), 1);
                }
            }
            return nullptr;
        }
    }

    void* ptr = reinterpret_cast<void*>(candidate);
    memset(ptr, 0, alignedBytes);

    m_allocations[slot] = Allocation{ true, candidate, alignedBytes };
    m_usedBytes        += alignedBytes;
    return ptr;
}

Status ModuleArea::free(void* basePtr, usize) noexcept {
    if (!basePtr || !m_pmm || !m_vmm) return Status::InvalidArgument;

    virt_addr_t vbase = reinterpret_cast<virt_addr_t>(basePtr);

    IrqLockGuard guard(m_lock);

    for (usize i = 0; i < MAX_ALLOCATIONS; ++i) {
        if (m_allocations[i].inUse && m_allocations[i].virtBase == vbase) {
            usize bytes     = m_allocations[i].sizeBytes;
            usize pageCount = bytes / PAGE_SIZE_4K;
            for (usize p = 0; p < pageCount; ++p) {
                virt_addr_t pv = vbase + p * PAGE_SIZE_4K;
                auto tp        = m_vmm->translateVirtToPhys(pv);
                (void)m_vmm->unmapPage(pv);
                if (tp.isOk()) {
                    m_pmm->freePages(tp.value(), 1);
                }
            }
            if (m_usedBytes >= bytes) {
                m_usedBytes -= bytes;
            }
            m_allocations[i] = Allocation{ false, 0, 0 };
            return Status::Ok;
        }
    }

    return Status::NotFound;
}

} // namespace cortez::memory
