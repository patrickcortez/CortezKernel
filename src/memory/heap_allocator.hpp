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
    static constexpr usize NUM_FREE_BINS      = 8;

    static KernelHeap& instance() noexcept;

    Status init(IPhysicalAllocator* pmm, u64 hhdmOffset) noexcept;

    void* allocate(usize size, usize alignment = 16) noexcept override;
    void* allocateRaw(usize size, usize alignment = 16, bool zeroMemory = false) noexcept;
    void* reallocate(void* ptr, usize newSize, usize alignment = 16) noexcept override;
    void  deallocate(void* ptr) noexcept override;
    usize getBlockSize(const void* ptr) const noexcept;

    usize getUsedBytes() const noexcept override { return m_usedBytes; }
    usize getTotalBytes() const noexcept override { return m_totalBytes; }

private:
    struct BlockHeader {
        u32          magic;
        u32          isFree;
        usize        size; // Payload size in bytes (always >= 32, multiple of 16)
        BlockHeader* next; // Next block in physical arena order
        BlockHeader* prev; // Prev block in physical arena order
    };

    struct FreeListLinks {
        BlockHeader* nextFree;
        BlockHeader* prevFree;
    };

    KernelHeap() noexcept = default;

    static usize binIndexForSize(usize size) noexcept;
    static FreeListLinks* getFreeLinks(BlockHeader* hdr) noexcept;
    void insertFreeBlock(BlockHeader* hdr) noexcept;
    void removeFreeBlock(BlockHeader* hdr) noexcept;
    void coalesceBlock(BlockHeader* hdr) noexcept;
    bool expandHeap(usize minBytesNeeded) noexcept;

    mutable Spinlock    m_lock;
    IPhysicalAllocator* m_pmm                      = nullptr;
    u64                 m_hhdmOffset               = 0;
    BlockHeader*        m_head                     = nullptr;
    BlockHeader*        m_freeBins[NUM_FREE_BINS]{};
    usize               m_usedBytes                = 0;
    usize               m_totalBytes               = 0;
};

} // namespace cortez::memory
