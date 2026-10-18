/**
 * @file boot_info.hpp
 * @brief OOP wrapper around Limine bootloader handoff information.
 */

#pragma once

#include "limine.h"
#include "../lib/types.hpp"
#include "../lib/result.hpp"

namespace cortez::boot {

struct FramebufferInfo {
    void* addr        = nullptr;
    u64   width       = 0;
    u64   height      = 0;
    u64   pitch       = 0;
    u16   bpp         = 0;
    u8    redShift    = 16;
    u8    greenShift  = 8;
    u8    blueShift   = 0;
};

/**
 * @brief Singleton encapsulating UEFI/Limine boot protocol responses.
 */
class BootInfo final {
public:
    static BootInfo& instance() noexcept;

    Status init() noexcept;

    u64                        getHhdmOffset() const noexcept { return m_hhdmOffset; }
    const FramebufferInfo&     getFramebuffer() const noexcept { return m_fb; }
    bool                       hasFramebuffer() const noexcept { return m_fb.addr != nullptr; }
    const limine_memmap_response* getMemmap() const noexcept { return m_memmap; }
    phys_addr_t                getRsdpPhysAddr() const noexcept { return m_rsdpPhys; }
    const char*                getCmdline() const noexcept { return m_cmdline; }
    phys_addr_t                getKernelPhysBase() const noexcept { return m_kernelPhysBase; }
    virt_addr_t                getKernelVirtBase() const noexcept { return m_kernelVirtBase; }

    template <typename T = void*>
    T physToVirt(phys_addr_t phys) const noexcept {
        return reinterpret_cast<T>(phys + m_hhdmOffset);
    }

    phys_addr_t virtToPhys(virt_addr_t virt) const noexcept {
        if (virt >= m_kernelVirtBase && m_kernelVirtBase != 0) {
            return (virt - m_kernelVirtBase) + m_kernelPhysBase;
        }
        return virt - m_hhdmOffset;
    }

private:
    BootInfo() noexcept = default;

    u64                           m_hhdmOffset     = 0;
    FramebufferInfo               m_fb{};
    const limine_memmap_response* m_memmap         = nullptr;
    phys_addr_t                   m_rsdpPhys       = 0;
    const char*                   m_cmdline        = "";
    phys_addr_t                   m_kernelPhysBase = 0;
    virt_addr_t                   m_kernelVirtBase = 0;
};

} // namespace cortez::boot
