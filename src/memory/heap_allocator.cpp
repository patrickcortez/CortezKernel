/**
 * @file heap_allocator.cpp
 * @brief Implementation of the Segregated-Bin Coalescing Kernel Heap Allocator.
 */

#include "heap_allocator.hpp"
#include "../lib/string_utils.hpp"
#include "../lib/printk.hpp"

namespace cortez::memory {

namespace {
constexpr usize MIN_BLOCK_PAYLOAD = 32;
} // namespace

KernelHeap& KernelHeap::instance() noexcept {
    static KernelHeap s_heap;
    return s_heap;
}

usize KernelHeap::binIndexForSize(usize size) noexcept {
    if (size <= 32)   return 0;
    if (size <= 64)   return 1;
    if (size <= 128)  return 2;
    if (size <= 256)  return 3;
    if (size <= 512)  return 4;
    if (size <= 1024) return 5;
    if (size <= 2048) return 6;
    return 7;
}

KernelHeap::FreeListLinks* KernelHeap::getFreeLinks(BlockHeader* hdr) noexcept {
    return reinterpret_cast<FreeListLinks*>(reinterpret_cast<u8*>(hdr) + sizeof(BlockHeader));
}

void KernelHeap::insertFreeBlock(BlockHeader* hdr) noexcept {
    if (!hdr) return;
    hdr->isFree = 1;
    usize bin   = binIndexForSize(hdr->size);
    auto* links = getFreeLinks(hdr);

    links->prevFree = nullptr;
    links->nextFree = m_freeBins[bin];
    if (m_freeBins[bin]) {
        getFreeLinks(m_freeBins[bin])->prevFree = hdr;
    }
    m_freeBins[bin] = hdr;
}

void KernelHeap::removeFreeBlock(BlockHeader* hdr) noexcept {
    if (!hdr) return;
    usize bin   = binIndexForSize(hdr->size);
    auto* links = getFreeLinks(hdr);

    if (links->prevFree) {
        getFreeLinks(links->prevFree)->nextFree = links->nextFree;
    } else if (m_freeBins[bin] == hdr) {
        m_freeBins[bin] = links->nextFree;
    }

    if (links->nextFree) {
        getFreeLinks(links->nextFree)->prevFree = links->prevFree;
    }

    links->nextFree = nullptr;
    links->prevFree = nullptr;
}

Status KernelHeap::init(IPhysicalAllocator* pmm, u64 hhdmOffset) noexcept {
    if (!pmm) return Status::InvalidArgument;

    m_pmm        = pmm;
    m_hhdmOffset = hhdmOffset;
    m_head       = nullptr;
    m_usedBytes  = 0;
    m_totalBytes = 0;
    for (usize i = 0; i < NUM_FREE_BINS; ++i) {
        m_freeBins[i] = nullptr;
    }

    if (!expandHeap(INITIAL_HEAP_PAGES * PAGE_SIZE_4K)) {
        return Status::OutOfMemory;
    }

    pr_ok("heap", "Initialized Segregated-Bin Kernel Heap (%zu KiB initial arena)", m_totalBytes / 1024);
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
    insertFreeBlock(newBlock);
    return true;
}

void* KernelHeap::allocate(usize size, usize alignment) noexcept {
    return allocateRaw(size, alignment, true);
}

void* KernelHeap::allocateRaw(usize size, usize alignment, bool zeroMemory) noexcept {
    if (size == 0 || !m_pmm) return nullptr;
    if (alignment < 16) alignment = 16;

    usize alignedSize = static_cast<usize>(alignUp(size, 16));
    if (alignedSize < MIN_BLOCK_PAYLOAD) {
        alignedSize = MIN_BLOCK_PAYLOAD;
    }

    IrqLockGuard guard(m_lock);

    for (int attempt = 0; attempt < 2; ++attempt) {
        usize startBin = binIndexForSize(alignedSize);
        for (usize bin = startBin; bin < NUM_FREE_BINS; ++bin) {
            BlockHeader* curr = m_freeBins[bin];
            while (curr) {
                BlockHeader* nextCandidate = getFreeLinks(curr)->nextFree;
                if (curr->isFree && curr->size >= alignedSize) {
                    u64 rawPayloadAddr = reinterpret_cast<u64>(curr) + sizeof(BlockHeader);
                    u64 alignedPayload = alignUp(rawPayloadAddr, alignment);
                    usize leadPad      = static_cast<usize>(alignedPayload - rawPayloadAddr);

                    // If misaligned and leadPad is too small to hold a free block header + payload, advance one alignment unit
                    if (leadPad > 0 && leadPad < sizeof(BlockHeader) + MIN_BLOCK_PAYLOAD) {
                        alignedPayload = alignUp(rawPayloadAddr + sizeof(BlockHeader) + MIN_BLOCK_PAYLOAD, alignment);
                        leadPad        = static_cast<usize>(alignedPayload - rawPayloadAddr);
                    }

                    if (curr->size >= leadPad + alignedSize) {
                        removeFreeBlock(curr);

                        // Split leading alignment padding block if required
                        if (leadPad >= sizeof(BlockHeader) + MIN_BLOCK_PAYLOAD) {
                            usize origSize   = curr->size;
                            BlockHeader* nxt = curr->next;

                            curr->size = leadPad - sizeof(BlockHeader);

                            auto* alignedHdr   = reinterpret_cast<BlockHeader*>(alignedPayload - sizeof(BlockHeader));
                            alignedHdr->magic  = HEAP_MAGIC;
                            alignedHdr->isFree = 0;
                            alignedHdr->size   = origSize - leadPad;
                            alignedHdr->prev   = curr;
                            alignedHdr->next   = nxt;
                            if (nxt) nxt->prev = alignedHdr;
                            curr->next = alignedHdr;

                            insertFreeBlock(curr);
                            curr = alignedHdr;
                        }

                        // Split trailing remainder if large enough
                        if (curr->size >= alignedSize + sizeof(BlockHeader) + MIN_BLOCK_PAYLOAD) {
                            u8* splitPtr     = reinterpret_cast<u8*>(curr) + sizeof(BlockHeader) + alignedSize;
                            auto* tailBlock  = reinterpret_cast<BlockHeader*>(splitPtr);
                            tailBlock->magic  = HEAP_MAGIC;
                            tailBlock->isFree = 1;
                            tailBlock->size   = curr->size - alignedSize - sizeof(BlockHeader);
                            tailBlock->next   = curr->next;
                            tailBlock->prev   = curr;

                            if (curr->next) {
                                curr->next->prev = tailBlock;
                            }
                            curr->next = tailBlock;
                            curr->size = alignedSize;

                            insertFreeBlock(tailBlock);
                        }

                        curr->isFree = 0;
                        m_usedBytes += curr->size;

                        void* payload = reinterpret_cast<u8*>(curr) + sizeof(BlockHeader);
                        if (zeroMemory) {
                            memset(payload, 0, curr->size);
                        }
                        return payload;
                    }
                }
                curr = nextCandidate;
            }
        }

        if (!expandHeap(alignedSize + alignment + sizeof(BlockHeader) + MIN_BLOCK_PAYLOAD)) {
            break;
        }
    }

    return nullptr;
}

void KernelHeap::coalesceBlock(BlockHeader* header) noexcept {
    // Coalesce with physically adjacent next block
    if (header->next && header->next->isFree) {
        u8* expectedNext = reinterpret_cast<u8*>(header) + sizeof(BlockHeader) + header->size;
        if (reinterpret_cast<u8*>(header->next) == expectedNext) {
            BlockHeader* next = header->next;
            removeFreeBlock(next);
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
            removeFreeBlock(prev);
            prev->size += sizeof(BlockHeader) + header->size;
            prev->next  = header->next;
            if (header->next) {
                header->next->prev = prev;
            }
            header = prev;
        }
    }

    insertFreeBlock(header);
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

    coalesceBlock(header);
}

usize KernelHeap::getBlockSize(const void* ptr) const noexcept {
    if (!ptr) return 0;
    const auto* header = reinterpret_cast<const BlockHeader*>(static_cast<const u8*>(ptr) - sizeof(BlockHeader));
    if (header->magic != HEAP_MAGIC) return 0;
    return header->size;
}

void* KernelHeap::reallocate(void* ptr, usize newSize, usize alignment) noexcept {
    if (!ptr) {
        return allocateRaw(newSize, alignment, false);
    }
    if (newSize == 0) {
        deallocate(ptr);
        return nullptr;
    }

    usize alignedNew = static_cast<usize>(alignUp(newSize, 16));
    if (alignedNew < MIN_BLOCK_PAYLOAD) {
        alignedNew = MIN_BLOCK_PAYLOAD;
    }

    usize oldSize = 0;
    {
        IrqLockGuard guard(m_lock);
        auto* header = reinterpret_cast<BlockHeader*>(static_cast<u8*>(ptr) - sizeof(BlockHeader));
        if (header->magic != HEAP_MAGIC || header->isFree) {
            return nullptr;
        }
        oldSize = header->size;

        // Case 1: Current block is already large enough (shrink or no-op)
        if (header->size >= alignedNew) {
            if (header->size >= alignedNew + sizeof(BlockHeader) + MIN_BLOCK_PAYLOAD) {
                usize diff        = header->size - alignedNew;
                u8* splitPtr      = reinterpret_cast<u8*>(header) + sizeof(BlockHeader) + alignedNew;
                auto* tailBlock   = reinterpret_cast<BlockHeader*>(splitPtr);
                tailBlock->magic  = HEAP_MAGIC;
                tailBlock->isFree = 1;
                tailBlock->size   = diff - sizeof(BlockHeader);
                tailBlock->next   = header->next;
                tailBlock->prev   = header;
                if (header->next) {
                    header->next->prev = tailBlock;
                }
                header->next = tailBlock;
                header->size = alignedNew;
                if (m_usedBytes >= diff) m_usedBytes -= diff;

                coalesceBlock(tailBlock);
            }
            return ptr;
        }

        // Case 2: Physically adjacent next block is free and combined size fits
        if (header->next && header->next->isFree) {
            u8* expectedNext = reinterpret_cast<u8*>(header) + sizeof(BlockHeader) + header->size;
            if (reinterpret_cast<u8*>(header->next) == expectedNext) {
                usize combinedSize = header->size + sizeof(BlockHeader) + header->next->size;
                if (combinedSize >= alignedNew) {
                    BlockHeader* next = header->next;
                    removeFreeBlock(next);
                    m_usedBytes += sizeof(BlockHeader) + next->size;
                    header->size = combinedSize;
                    header->next = next->next;
                    if (next->next) {
                        next->next->prev = header;
                    }

                    // Split leftover if large enough
                    if (header->size >= alignedNew + sizeof(BlockHeader) + MIN_BLOCK_PAYLOAD) {
                        usize diff        = header->size - alignedNew;
                        u8* splitPtr      = reinterpret_cast<u8*>(header) + sizeof(BlockHeader) + alignedNew;
                        auto* tailBlock   = reinterpret_cast<BlockHeader*>(splitPtr);
                        tailBlock->magic  = HEAP_MAGIC;
                        tailBlock->isFree = 1;
                        tailBlock->size   = diff - sizeof(BlockHeader);
                        tailBlock->next   = header->next;
                        tailBlock->prev   = header;
                        if (header->next) {
                            header->next->prev = tailBlock;
                        }
                        header->next = tailBlock;
                        header->size = alignedNew;
                        if (m_usedBytes >= diff) m_usedBytes -= diff;

                        insertFreeBlock(tailBlock);
                    }
                    return ptr;
                }
            }
        }
    }

    // Case 3: Allocate new block, copy old payload, and free old block
    void* newPtr = allocateRaw(newSize, alignment, false);
    if (!newPtr) return nullptr;
    memcpy(newPtr, ptr, (oldSize < newSize) ? oldSize : newSize);
    deallocate(ptr);
    return newPtr;
}

} // namespace cortez::memory
