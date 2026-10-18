/**
 * @file boot_info.cpp
 * @brief Limine boot protocol requests and BootInfo initialization.
 */

#include "boot_info.hpp"

namespace {

__attribute__((used, section(".limine_requests_start")))
volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".limine_requests")))
volatile limine_framebuffer_request g_fbRequest = {
    .id       = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".limine_requests")))
volatile limine_hhdm_request g_hhdmRequest = {
    .id       = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".limine_requests")))
volatile limine_memmap_request g_memmapRequest = {
    .id       = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".limine_requests")))
volatile limine_rsdp_request g_rsdpRequest = {
    .id       = LIMINE_RSDP_REQUEST,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".limine_requests")))
volatile limine_kernel_address_request g_kernelAddrRequest = {
    .id       = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".limine_requests")))
volatile limine_kernel_file_request g_kernelFileRequest = {
    .id       = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".limine_requests_end")))
volatile LIMINE_REQUESTS_END_MARKER;

} // namespace

namespace cortez::boot {

BootInfo& BootInfo::instance() noexcept {
    static BootInfo s_instance;
    return s_instance;
}

Status BootInfo::init() noexcept {
    if (!g_hhdmRequest.response || !g_memmapRequest.response) {
        return Status::DeviceNotFound;
    }

    m_hhdmOffset = g_hhdmRequest.response->offset;
    m_memmap     = g_memmapRequest.response;

    if (g_fbRequest.response && g_fbRequest.response->framebuffer_count > 0) {
        limine_framebuffer* fb = g_fbRequest.response->framebuffers[0];
        if (fb) {
            m_fb.addr       = fb->address;
            m_fb.width      = fb->width;
            m_fb.height     = fb->height;
            m_fb.pitch      = fb->pitch;
            m_fb.bpp        = fb->bpp;
            m_fb.redShift   = fb->red_mask_shift;
            m_fb.greenShift = fb->green_mask_shift;
            m_fb.blueShift  = fb->blue_mask_shift;
        }
    }

    if (g_rsdpRequest.response && g_rsdpRequest.response->address) {
        u64 addr = reinterpret_cast<u64>(g_rsdpRequest.response->address);
        m_rsdpPhys = (addr >= m_hhdmOffset) ? (addr - m_hhdmOffset) : addr;
    }

    if (g_kernelAddrRequest.response) {
        m_kernelPhysBase = g_kernelAddrRequest.response->physical_base;
        m_kernelVirtBase = g_kernelAddrRequest.response->virtual_base;
    }

    if (g_kernelFileRequest.response && g_kernelFileRequest.response->kernel_file) {
        if (g_kernelFileRequest.response->kernel_file->cmdline) {
            m_cmdline = g_kernelFileRequest.response->kernel_file->cmdline;
        }
    }

    return Status::Ok;
}

} // namespace cortez::boot
