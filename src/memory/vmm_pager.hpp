/**
 * @file vmm_pager.hpp
 * @brief 4-Level x86_64 Virtual Memory Manager (PML4/PDPT/PD/PT) with dedicated uncached MMIO mapping.
 */

#pragma once

#include "i_memory.hpp"
#include "../lib/spinlock.hpp"

namespace cortez::memory {

class PageTableVmm final : public IVirtualMapper {
public:
    static constexpr virt_addr_t MMIO_VIRT_BASE = 0xFFFFFE8000000000ULL; // PML4[509]

    static PageTableVmm& instance() noexcept;

    Status init(IPhysicalAllocator* pmm, u64 hhdmOffset) noexcept;

    Status              mapPage(virt_addr_t vaddr, phys_addr_t paddr, u64 flags) noexcept override;
    Status              unmapPage(virt_addr_t vaddr) noexcept override;
    Result<virt_addr_t> mapMmio(phys_addr_t paddr, usize sizeBytes) noexcept override;
    Result<phys_addr_t> translateVirtToPhys(virt_addr_t vaddr) const noexcept override;

private:
    PageTableVmm() noexcept = default;

    u64* getOrCreateNextTable(u64* tableVirt, usize index, u64 flags) noexcept;

    mutable Spinlock    m_lock;
    IPhysicalAllocator* m_pmm          = nullptr;
    u64                 m_hhdmOffset   = 0;
    u64*                m_pml4Virt     = nullptr;
    virt_addr_t         m_nextMmioVirt = MMIO_VIRT_BASE;
};

} // namespace cortez::memory
