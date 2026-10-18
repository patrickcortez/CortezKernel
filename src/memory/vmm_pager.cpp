/**
 * @file vmm_pager.cpp
 * @brief Implementation of 4-level x86_64 paging and uncacheable MMIO mapping.
 */

#include "vmm_pager.hpp"
#include "../arch/x86_64/cpu.hpp"
#include "../lib/printk.hpp"

namespace cortez::memory {

namespace {
constexpr u64 PTE_ADDR_MASK = 0x000FFFFFFFFFF000ULL;
constexpr u64 PTE_HUGE_PAGE = (1ULL << 7);
}

PageTableVmm& PageTableVmm::instance() noexcept {
    static PageTableVmm s_vmm;
    return s_vmm;
}

Status PageTableVmm::init(IPhysicalAllocator* pmm, u64 hhdmOffset) noexcept {
    if (!pmm) return Status::InvalidArgument;

    m_pmm        = pmm;
    m_hhdmOffset = hhdmOffset;

    phys_addr_t cr3Phys = arch::readCr3() & PTE_ADDR_MASK;
    m_pml4Virt          = reinterpret_cast<u64*>(cr3Phys + m_hhdmOffset);
    m_nextMmioVirt      = MMIO_VIRT_BASE;

    pr_ok("vmm", "4-Level Page Table VMM initialized (CR3 PML4=0x%llx, HHDM=0x%llx)",
          cr3Phys, m_hhdmOffset);
    return Status::Ok;
}

u64* PageTableVmm::getOrCreateNextTable(u64* tableVirt, usize index, u64 flags) noexcept {
    u64 entry = tableVirt[index];
    if (entry & PAGE_PRESENT) {
        if (entry & PTE_HUGE_PAGE) {
            return nullptr; // Cannot traverse into a huge page
        }
        phys_addr_t nextPhys = entry & PTE_ADDR_MASK;
        return reinterpret_cast<u64*>(nextPhys + m_hhdmOffset);
    }

    auto allocRes = m_pmm->allocPages(1);
    if (allocRes.isError()) {
        return nullptr;
    }

    phys_addr_t newPhys = allocRes.value();
    tableVirt[index]    = (newPhys & PTE_ADDR_MASK) | flags;
    return reinterpret_cast<u64*>(newPhys + m_hhdmOffset);
}

Status PageTableVmm::mapPage(virt_addr_t vaddr, phys_addr_t paddr, u64 flags) noexcept {
    if (!m_pml4Virt) return Status::NotSupported;

    IrqLockGuard guard(m_lock);

    usize pml4Idx = (vaddr >> 39) & 0x1FFULL;
    usize pdptIdx = (vaddr >> 30) & 0x1FFULL;
    usize pdIdx   = (vaddr >> 21) & 0x1FFULL;
    usize ptIdx   = (vaddr >> 12) & 0x1FFULL;

    u64 parentFlags = PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER);

    u64* pdpt = getOrCreateNextTable(m_pml4Virt, pml4Idx, parentFlags);
    if (!pdpt) return Status::OutOfMemory;

    u64* pd = getOrCreateNextTable(pdpt, pdptIdx, parentFlags);
    if (!pd) return Status::OutOfMemory;

    u64* pt = getOrCreateNextTable(pd, pdIdx, parentFlags);
    if (!pt) return Status::OutOfMemory;

    pt[ptIdx] = (paddr & PTE_ADDR_MASK) | flags | PAGE_PRESENT;
    arch::invlpg(vaddr);

    return Status::Ok;
}

Status PageTableVmm::unmapPage(virt_addr_t vaddr) noexcept {
    if (!m_pml4Virt) return Status::NotSupported;

    IrqLockGuard guard(m_lock);

    usize pml4Idx = (vaddr >> 39) & 0x1FFULL;
    usize pdptIdx = (vaddr >> 30) & 0x1FFULL;
    usize pdIdx   = (vaddr >> 21) & 0x1FFULL;
    usize ptIdx   = (vaddr >> 12) & 0x1FFULL;

    if (!(m_pml4Virt[pml4Idx] & PAGE_PRESENT)) return Status::NotFound;
    u64* pdpt = reinterpret_cast<u64*>((m_pml4Virt[pml4Idx] & PTE_ADDR_MASK) + m_hhdmOffset);

    if (!(pdpt[pdptIdx] & PAGE_PRESENT) || (pdpt[pdptIdx] & PTE_HUGE_PAGE)) return Status::NotFound;
    u64* pd = reinterpret_cast<u64*>((pdpt[pdptIdx] & PTE_ADDR_MASK) + m_hhdmOffset);

    if (!(pd[pdIdx] & PAGE_PRESENT) || (pd[pdIdx] & PTE_HUGE_PAGE)) return Status::NotFound;
    u64* pt = reinterpret_cast<u64*>((pd[pdIdx] & PTE_ADDR_MASK) + m_hhdmOffset);

    pt[ptIdx] = 0;
    arch::invlpg(vaddr);
    return Status::Ok;
}

Result<virt_addr_t> PageTableVmm::mapMmio(phys_addr_t paddr, usize sizeBytes) noexcept {
    if (sizeBytes == 0) return Status::InvalidArgument;

    phys_addr_t alignedPhys = alignDown(paddr, PAGE_SIZE_4K);
    usize       offsetInPage = static_cast<usize>(paddr - alignedPhys);
    usize       totalAligned = static_cast<usize>(alignUp(sizeBytes + offsetInPage, PAGE_SIZE_4K));
    usize       pageCount    = totalAligned / PAGE_SIZE_4K;

    virt_addr_t baseVirt = 0;
    {
        IrqLockGuard guard(m_lock);
        baseVirt       = m_nextMmioVirt;
        m_nextMmioVirt += totalAligned + PAGE_SIZE_4K; // Guard page between MMIO regions
    }

    for (usize i = 0; i < pageCount; ++i) {
        Status st = mapPage(baseVirt + i * PAGE_SIZE_4K,
                            alignedPhys + i * PAGE_SIZE_4K,
                            PAGE_MMIO_DEFAULT);
        if (st != Status::Ok) {
            return st;
        }
    }

    return baseVirt + offsetInPage;
}

Result<phys_addr_t> PageTableVmm::translateVirtToPhys(virt_addr_t vaddr) const noexcept {
    if (!m_pml4Virt) return Status::NotSupported;

    IrqLockGuard guard(m_lock);

    usize pml4Idx = (vaddr >> 39) & 0x1FFULL;
    usize pdptIdx = (vaddr >> 30) & 0x1FFULL;
    usize pdIdx   = (vaddr >> 21) & 0x1FFULL;
    usize ptIdx   = (vaddr >> 12) & 0x1FFULL;

    if (!(m_pml4Virt[pml4Idx] & PAGE_PRESENT)) return Status::NotFound;
    u64* pdpt = reinterpret_cast<u64*>((m_pml4Virt[pml4Idx] & PTE_ADDR_MASK) + m_hhdmOffset);

    u64 pdptEntry = pdpt[pdptIdx];
    if (!(pdptEntry & PAGE_PRESENT)) return Status::NotFound;
    if (pdptEntry & PTE_HUGE_PAGE) {
        return (pdptEntry & 0x000FFFFFC0000000ULL) | (vaddr & 0x3FFFFFFFULL);
    }

    u64* pd = reinterpret_cast<u64*>((pdptEntry & PTE_ADDR_MASK) + m_hhdmOffset);
    u64 pdEntry = pd[pdIdx];
    if (!(pdEntry & PAGE_PRESENT)) return Status::NotFound;
    if (pdEntry & PTE_HUGE_PAGE) {
        return (pdEntry & 0x000FFFFFFFE00000ULL) | (vaddr & 0x1FFFFFULL);
    }

    u64* pt = reinterpret_cast<u64*>((pdEntry & PTE_ADDR_MASK) + m_hhdmOffset);
    u64 ptEntry = pt[ptIdx];
    if (!(ptEntry & PAGE_PRESENT)) return Status::NotFound;

    return (ptEntry & PTE_ADDR_MASK) | (vaddr & 0xFFFULL);
}

} // namespace cortez::memory
