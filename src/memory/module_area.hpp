/**
 * @file module_area.hpp
 * @brief Executable Higher-Half Virtual Memory Region (0xFFFFFFFFC0000000+) for Runtime-Compiled C Modules.
 */

#pragma once

#include "i_memory.hpp"
#include "../lib/spinlock.hpp"

namespace cortez::memory {

class ModuleArea final {
public:
    static constexpr virt_addr_t MODULE_VIRT_BASE = 0xFFFFFFFFC0000000ULL;
    static constexpr virt_addr_t MODULE_VIRT_END  = 0xFFFFFFFFFF000000ULL;
    static constexpr usize       MAX_ALLOCATIONS  = 64;

    static ModuleArea& instance() noexcept;

    Status init(IPhysicalAllocator* pmm, IVirtualMapper* vmm) noexcept;

    void*  allocate(usize sizeBytes) noexcept;
    Status free(void* basePtr, usize sizeBytes = 0) noexcept;

    bool   containsAddress(virt_addr_t vaddr) const noexcept {
        return vaddr >= MODULE_VIRT_BASE && vaddr < MODULE_VIRT_END;
    }

    usize  getUsedBytes() const noexcept { return m_usedBytes; }

private:
    struct Allocation {
        bool        inUse;
        virt_addr_t virtBase;
        usize       sizeBytes; // Page-aligned size
    };

    ModuleArea() noexcept = default;

    bool isRangeOverlapping(virt_addr_t base, usize bytes) const noexcept;

    mutable Spinlock    m_lock;
    IPhysicalAllocator* m_pmm                          = nullptr;
    IVirtualMapper*     m_vmm                          = nullptr;
    Allocation          m_allocations[MAX_ALLOCATIONS]{};
    usize               m_usedBytes                    = 0;
};

} // namespace cortez::memory
