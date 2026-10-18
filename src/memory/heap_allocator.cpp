/**
 * @file heap_allocator.cpp
 * @brief Implementation of the Coalescing Free-List Kernel Heap Allocator.
 */

#include "heap_allocator.hpp"
#include "../lib/string_utils.hpp"
#include "../lib/printk.hpp"

namespace cortez::memory {

KernelHeap& KernelHeap::instance() noexcept {
    static KernelHeap s_heap;
    return s_heap;
}

Status KernelHeap::init(IPhysicalAllocator* pmm, u64 hhdmOffset) noexcept {
    if (!pmm) return Status::InvalidArgument;

    m_pmm        = pmm;
    m_hhdmOffset = hhdmOffset;
    m_head       = nullptr;
    m_usedBytes  = 0;
    m_totalBytes = 0;

    if (!expandHeap(INITIAL_HEAP_PAGES * PAGE_SIZE_4K)) {
        return Status::OutOfMemory;
    }

    pr_ok("heap", "Initialized Kernel Heap (%zu KiB initial arena)", m_totalBytes / 1024);
    return Status::Ok;
}

bool KernelHeap::expandHeap(usize minBytesNeeded) noexcept {
    usize totalBytes = alignUp(minBytesNeeded + sizeof(BlockHeader) + 64, PAGE_SIZE_4K);
    usize pages      = totalBytes / PAGE_SIZE_4K;
    if (pages < 64) pages = 64; // Minimum 256 KiB chunk expansion

    auto res = m_pmm->allocPages(pages);
    if (res.isError()) {
        return false;
    }

    usize chunkBytes = pages * PAGE_SIZE_4K;
    auto* newBlock   = reinterpret_cast<BlockHeader*>(res.value() + m_hhdmOffset);
    newBlock->magic  = HEAP_MAGIC;
    newBlock->isFree = 1;
    newBlock->size   = chunkBytes - sizeof(BlockHeader);
    newBlock->next   = nullptr;
    newBlock->prev   = nullptr;

    if (!m_head) {
        m_head = newBlock;
    } else {
        BlockHeader* tail = m_head;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next     = newBlock;
        newBlock->prev = tail;
    }

    m_totalBytes += newBlock->size;
    return true;
}

void* KernelHeap::allocate(usize size, usize alignment) noexcept {
    if (size == 0 || !m_pmm) return nullptr;
    if (alignment < 16) alignment = 16;

    usize alignedSize = static_cast<usize>(alignUp(size, alignment));

    IrqLockGuard guard(m_lock);

    for (int attempt = 0; attempt < 2; ++attempt) {
        BlockHeader* curr = m_head;
        while (curr) {
            if (curr->isFree && curr->size >= alignedSize) {
                // Split block if remaining space can hold another header + 32B payload
                if (curr->size >= alignedSize + sizeof(BlockHeader) + 32) {
                    u8* splitPtr = reinterpret_cast<u8*>(curr) + sizeof(BlockHeader) + alignedSize;
                    auto* nextBlock   = reinterpret_cast<BlockHeader*>(splitPtr);
                    nextBlock->magic  = HEAP_MAGIC;
                    nextBlock->isFree = 1;
                    nextBlock->size   = curr->size - alignedSize - sizeof(BlockHeader);
                    nextBlock->next   = curr->next;
                    nextBlock->prev   = curr;

                    if (curr->next) {
                        curr->next->prev = nextBlock;
                    }
                    curr->next = nextBlock;
                    curr->size = alignedSize;
                }

                curr->isFree = 0;
                m_usedBytes += curr->size;

                void* payload = reinterpret_cast<u8*>(curr) + sizeof(BlockHeader);
                memset(payload, 0, curr->size);
                return payload;
            }
            curr = curr->next;
        }

        if (!expandHeap(alignedSize)) {
            break;
        }
    }

    return nullptr;
}

void KernelHeap::deallocate(void* ptr) noexcept {
    if (!ptr) return;

    IrqLockGuard guard(m_lock);

    auto* header = reinterpret_cast<BlockHeader*>(static_cast<u8*>(ptr) - sizeof(BlockHeader));
    if (header->magic != HEAP_MAGIC || header->isFree) {
        return;
    }

    header->isFree = 1;
    if (m_usedBytes >= header->size) {
        m_usedBytes -= header->size;
    }

    // Coalesce with physically adjacent next block
    if (header->next && header->next->isFree) {
        u8* expectedNext = reinterpret_cast<u8*>(header) + sizeof(BlockHeader) + header->size;
        if (reinterpret_cast<u8*>(header->next) == expectedNext) {
            BlockHeader* next = header->next;
            header->size += sizeof(BlockHeader) + next->size;
            header->next  = next->next;
            if (next->next) {
                next->next->prev = header;
            }
        }
    }

    // Coalesce with physically adjacent previous block
    if (header->prev && header->prev->isFree) {
        u8* expectedCurr = reinterpret_cast<u8*>(header->prev) + sizeof(BlockHeader) + header->prev->size;
        if (reinterpret_cast<u8*>(header) == expectedCurr) {
            BlockHeader* prev = header->prev;
            prev->size += sizeof(BlockHeader) + header->size;
            prev->next  = header->next;
            if (header->next) {
                header->next->prev = prev;
            }
        }
    }
}

} // namespace cortez::memory
