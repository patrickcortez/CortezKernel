/**
 * @file pmm_bitmap.cpp
 * @brief Implementation of BitmapPmm physical page frame allocator.
 */

#include "pmm_bitmap.hpp"
#include "../lib/string_utils.hpp"
#include "../lib/printk.hpp"

namespace cortez::memory {

BitmapPmm& BitmapPmm::instance() noexcept {
    static BitmapPmm s_pmm;
    return s_pmm;
}

Status BitmapPmm::init(const limine_memmap_response* memmap, u64 hhdmOffset) noexcept {
    if (!memmap || memmap->entry_count == 0) {
        return Status::InvalidArgument;
    }

    m_hhdmOffset = hhdmOffset;

    phys_addr_t highestPhysAddr = 0;

    for (u64 i = 0; i < memmap->entry_count; ++i) {
        const limine_memmap_entry* entry = memmap->entries[i];
        if (!entry) continue;
        if (entry->type == LIMINE_MEMMAP_USABLE ||
            entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            phys_addr_t top = entry->base + entry->length;
            if (top > highestPhysAddr) {
                highestPhysAddr = top;
            }
        }
    }

    m_maxPages    = static_cast<usize>(alignUp(highestPhysAddr, PAGE_SIZE_4K) / PAGE_SIZE_4K);
    m_bitmapBytes = alignUp((m_maxPages + 7) / 8, PAGE_SIZE_4K);

    // Locate a usable physical region large enough to host the bitmap
    phys_addr_t bitmapPhysBase = 0;
    for (u64 i = 0; i < memmap->entry_count; ++i) {
        const limine_memmap_entry* entry = memmap->entries[i];
        if (entry && entry->type == LIMINE_MEMMAP_USABLE &&
            entry->base >= 0x100000ULL && entry->length >= m_bitmapBytes) {
            bitmapPhysBase = alignUp(entry->base, PAGE_SIZE_4K);
            if (bitmapPhysBase + m_bitmapBytes <= entry->base + entry->length) {
                break;
            }
            bitmapPhysBase = 0;
        }
    }

    if (bitmapPhysBase == 0) {
        return Status::OutOfMemory;
    }

    m_bitmap = reinterpret_cast<u8*>(bitmapPhysBase + m_hhdmOffset);
    // Mark all pages as reserved (1) initially
    memset(m_bitmap, 0xFF, m_bitmapBytes);

    m_totalPages = 0;
    m_freePages  = 0;

    // Free all LIMINE_MEMMAP_USABLE pages above 1 MiB
    for (u64 i = 0; i < memmap->entry_count; ++i) {
        const limine_memmap_entry* entry = memmap->entries[i];
        if (!entry || entry->type != LIMINE_MEMMAP_USABLE) continue;

        phys_addr_t start = alignUp(entry->base, PAGE_SIZE_4K);
        phys_addr_t end   = alignDown(entry->base + entry->length, PAGE_SIZE_4K);
        if (end <= start) continue;

        for (phys_addr_t addr = start; addr < end; addr += PAGE_SIZE_4K) {
            usize pageIdx = static_cast<usize>(addr / PAGE_SIZE_4K);
            if (pageIdx == 0) continue; // Never allocate physical page 0
            clearBit(pageIdx);
            ++m_totalPages;
            ++m_freePages;
        }
    }

    // Reserve the pages occupied by the PMM bitmap itself
    usize bitmapStartPage = static_cast<usize>(bitmapPhysBase / PAGE_SIZE_4K);
    usize bitmapPageCount = m_bitmapBytes / PAGE_SIZE_4K;
    for (usize i = 0; i < bitmapPageCount; ++i) {
        usize idx = bitmapStartPage + i;
        if (!testBit(idx)) {
            setBit(idx);
            if (m_freePages > 0) --m_freePages;
        }
    }

    m_lastHintIdx = bitmapStartPage + bitmapPageCount;

    pr_ok("pmm", "Initialized Bitmap PMM: %llu MiB usable (%zu free pages, bitmap at 0x%llx)",
          static_cast<u64>(m_freePages * PAGE_SIZE_4K) / (1024ULL * 1024ULL),
          m_freePages,
          bitmapPhysBase);

    return Status::Ok;
}

Result<phys_addr_t> BitmapPmm::allocPages(usize count) noexcept {
    if (count == 0) return Status::InvalidArgument;

    IrqLockGuard guard(m_lock);
    if (m_freePages < count) {
        return Status::OutOfMemory;
    }

    auto searchRange = [&](usize startIdx, usize endIdx) -> usize {
        usize consecutive = 0;
        usize candidateStart = 0;
        for (usize idx = startIdx; idx < endIdx; ++idx) {
            if (!testBit(idx)) {
                if (consecutive == 0) candidateStart = idx;
                if (++consecutive == count) {
                    return candidateStart;
                }
            } else {
                consecutive = 0;
            }
        }
        return 0;
    };

    usize foundIdx = searchRange(m_lastHintIdx, m_maxPages);
    if (foundIdx == 0 && m_lastHintIdx > 1) {
        foundIdx = searchRange(1, m_lastHintIdx);
    }

    if (foundIdx == 0) {
        return Status::OutOfMemory;
    }

    for (usize i = 0; i < count; ++i) {
        setBit(foundIdx + i);
    }
    m_freePages  -= count;
    m_lastHintIdx = foundIdx + count;

    phys_addr_t physBase = static_cast<phys_addr_t>(foundIdx) * PAGE_SIZE_4K;
    void* virtBase = reinterpret_cast<void*>(physBase + m_hhdmOffset);
    memset(virtBase, 0, count * PAGE_SIZE_4K);

    return physBase;
}

void BitmapPmm::freePages(phys_addr_t basePhys, usize count) noexcept {
    if (basePhys == 0 || count == 0) return;

    IrqLockGuard guard(m_lock);
    usize startPage = static_cast<usize>(basePhys / PAGE_SIZE_4K);
    for (usize i = 0; i < count; ++i) {
        usize idx = startPage + i;
        if (idx > 0 && idx < m_maxPages && testBit(idx)) {
            clearBit(idx);
            ++m_freePages;
        }
    }
    if (startPage < m_lastHintIdx) {
        m_lastHintIdx = startPage;
    }
}

} // namespace cortez::memory
