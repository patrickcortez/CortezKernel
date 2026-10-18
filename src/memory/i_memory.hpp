/**
 * @file i_memory.hpp
 * @brief SOLID interfaces for Physical Memory Allocation, Virtual Paging, and Kernel Heap.
 */

#pragma once

#include "../lib/types.hpp"
#include "../lib/result.hpp"

namespace cortez::memory {

enum PageFlags : u64 {
    PAGE_PRESENT       = (1ULL << 0),
    PAGE_WRITABLE      = (1ULL << 1),
    PAGE_USER          = (1ULL << 2),
    PAGE_WRITE_THROUGH = (1ULL << 3),
    PAGE_CACHE_DISABLE = (1ULL << 4),
    PAGE_NO_EXECUTE    = (1ULL << 63),
    PAGE_MMIO_DEFAULT  = PAGE_PRESENT | PAGE_WRITABLE | PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE
};

/**
 * @brief Interface for physical page frame allocation (4 KiB frames).
 */
class IPhysicalAllocator {
public:
    virtual ~IPhysicalAllocator() = default;

    virtual Result<phys_addr_t> allocPages(usize count = 1) noexcept = 0;
    virtual void                freePages(phys_addr_t basePhys, usize count = 1) noexcept = 0;
    virtual u64                 getTotalMemoryBytes() const noexcept = 0;
    virtual u64                 getFreeMemoryBytes() const noexcept = 0;
    virtual u64                 getUsedMemoryBytes() const noexcept = 0;
};

/**
 * @brief Interface for 4-level virtual memory paging and MMIO mapping.
 */
class IVirtualMapper {
public:
    virtual ~IVirtualMapper() = default;

    virtual Status              mapPage(virt_addr_t vaddr, phys_addr_t paddr, u64 flags) noexcept = 0;
    virtual Status              unmapPage(virt_addr_t vaddr) noexcept = 0;
    virtual Result<virt_addr_t> mapMmio(phys_addr_t paddr, usize sizeBytes) noexcept = 0;
    virtual Result<phys_addr_t> translateVirtToPhys(virt_addr_t vaddr) const noexcept = 0;
};

/**
 * @brief Interface for dynamic kernel heap memory allocation (kmalloc/kfree).
 */
class IHeapAllocator {
public:
    virtual ~IHeapAllocator() = default;

    virtual void* allocate(usize size, usize alignment = 16) noexcept = 0;
    virtual void  deallocate(void* ptr) noexcept = 0;
    virtual usize getUsedBytes() const noexcept = 0;
    virtual usize getTotalBytes() const noexcept = 0;
};

} // namespace cortez::memory
