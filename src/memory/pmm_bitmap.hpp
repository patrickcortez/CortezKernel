/**
 * @file pmm_bitmap.hpp
 * @brief Bitmap-based Physical Memory Manager (PMM) supporting contiguous DMA allocation.
 */

#pragma once

#include "i_memory.hpp"
#include "../boot/limine.h"
#include "../lib/spinlock.hpp"

namespace cortez::memory {

class BitmapPmm final : public IPhysicalAllocator {
public:
    static BitmapPmm& instance() noexcept;

    Status init(const limine_memmap_response* memmap, u64 hhdmOffset) noexcept;

    Result<phys_addr_t> allocPages(usize count = 1) noexcept override;
    void                freePages(phys_addr_t basePhys, usize count = 1) noexcept override;

    u64 getTotalMemoryBytes() const noexcept override { return m_totalPages * PAGE_SIZE_4K; }
    u64 getFreeMemoryBytes() const noexcept override { return m_freePages * PAGE_SIZE_4K; }
    u64 getUsedMemoryBytes() const noexcept override {
        return (m_totalPages - m_freePages) * PAGE_SIZE_4K;
    }

private:
    BitmapPmm() noexcept = default;

    bool testBit(usize pageIdx) const noexcept {
        return (m_bitmap[pageIdx / 8] & (1U << (pageIdx % 8))) != 0;
    }

    void setBit(usize pageIdx) noexcept {
        m_bitmap[pageIdx / 8] |= static_cast<u8>(1U << (pageIdx % 8));
    }

    void clearBit(usize pageIdx) noexcept {
        m_bitmap[pageIdx / 8] &= static_cast<u8>(~(1U << (pageIdx % 8)));
    }

    mutable Spinlock m_lock;
    u8*              m_bitmap      = nullptr;
    usize            m_bitmapBytes = 0;
    usize            m_maxPages    = 0;
    usize            m_totalPages  = 0;
    usize            m_freePages   = 0;
    usize            m_lastHintIdx = 1;
    u64              m_hhdmOffset  = 0;
};

} // namespace cortez::memory
