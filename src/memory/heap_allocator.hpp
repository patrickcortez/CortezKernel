/**
 * @file heap_allocator.hpp
 * @brief Coalescing Free-List Kernel Heap Allocator (kmalloc / kfree / operator new).
 */

#pragma once

#include "i_memory.hpp"
#include "../lib/spinlock.hpp"

namespace cortez::memory {

class KernelHeap final : public IHeapAllocator {
public:
    static constexpr u32   HEAP_MAGIC         = 0xC047E2A1U;
    static constexpr usize INITIAL_HEAP_PAGES = 1024; // 4 MiB initial heap

    static KernelHeap& instance() noexcept;

    Status init(IPhysicalAllocator* pmm, u64 hhdmOffset) noexcept;

    void* allocate(usize size, usize alignment = 16) noexcept override;
    void  deallocate(void* ptr) noexcept override;

    usize getUsedBytes() const noexcept override { return m_usedBytes; }
    usize getTotalBytes() const noexcept override { return m_totalBytes; }

private:
    struct BlockHeader {
        u32          magic;
        u32          isFree;
        usize        size; // Payload size in bytes
        BlockHeader* next;
        BlockHeader* prev;
    };

    KernelHeap() noexcept = default;

    bool expandHeap(usize minBytesNeeded) noexcept;

    mutable Spinlock    m_lock;
    IPhysicalAllocator* m_pmm        = nullptr;
    u64                 m_hhdmOffset = 0;
    BlockHeader*        m_head       = nullptr;
    usize               m_usedBytes  = 0;
    usize               m_totalBytes = 0;
};

} // namespace cortez::memory
