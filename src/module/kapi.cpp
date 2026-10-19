/**
 * @file kapi.cpp
 * @brief Implementation of the Stable C ABI Kernel Functions (cortez.h) and TCC Symbol Table Registration.
 */

#include "kapi.hpp"
#include "module_manager.hpp"
#include "../klibc/klibc.hpp"
#include "../lib/printk.hpp"
#include "../lib/string_utils.hpp"
#include "../drivers/input/ps2_keyboard.hpp"
#include "../drivers/i_device.hpp"
#include "../memory/heap_allocator.hpp"
#include "../memory/pmm_bitmap.hpp"
#include "../memory/vmm_pager.hpp"
#include "../boot/boot_info.hpp"
#include "../arch/x86_64/io_ports.hpp"
#include "../arch/x86_64/idt.hpp"
#include "../arch/x86_64/apic.hpp"
#include "../arch/x86_64/pcie.hpp"
#include "../arch/x86_64/cpu.hpp"
#include "../fs/vfs_manager.hpp"
#include "../proc/scheduler.hpp"
#include "../../third_party/tinycc/libtcc.h"
#include <stdarg.h>

extern "C" {

/* ============================================================================
 * 1. Console & Logging
 * ========================================================================== */

void k_printf(const char* fmt, ...) {
    if (!fmt) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = klibc_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0) {
        cortez::klog::Logger::instance().writeRaw(buf, static_cast<cortez::usize>(len));
    }
}

void k_log(int level, const char* subsys, const char* fmt, ...) {
    if (!fmt) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    klibc_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    cortez::klog::Level lvl = cortez::klog::Level::Info;
    if (level == 1)      lvl = cortez::klog::Level::Ok;
    else if (level == 2) lvl = cortez::klog::Level::Warn;
    else if (level >= 3) lvl = cortez::klog::Level::Error;

    cortez::klog::Logger::instance().log(lvl, subsys ? subsys : "mod", "%s", buf);
}

int k_getchar(void) {
    char c = 0;
    auto& kbd = cortez::drivers::input::Ps2KeyboardDriver::instance();
    auto res = kbd.read(&c, 1);
    if (res.isError() || res.value() == 0) {
        return -1;
    }
    return static_cast<unsigned char>(c);
}

bool k_kbhit(void) {
    return cortez::drivers::input::Ps2KeyboardDriver::instance().hasData();
}

/* ============================================================================
 * 2. Memory Management
 * ========================================================================== */

void* k_malloc(size_t size) {
    return klibc_malloc(size);
}

void* k_calloc(size_t nmemb, size_t size) {
    return klibc_calloc(nmemb, size);
}

void* k_realloc(void* ptr, size_t size) {
    return klibc_realloc(ptr, size);
}

void k_free(void* ptr) {
    klibc_free(ptr);
}

uint64_t k_alloc_pages(size_t count) {
    return cortez::module::ModuleManager::instance().allocPagesForCurrent(count);
}

void k_free_pages(uint64_t phys_addr, size_t count) {
    cortez::module::ModuleManager::instance().freePagesForCurrent(phys_addr, count);
}

void* k_phys_to_virt(uint64_t phys_addr) {
    if (phys_addr == 0) return nullptr;
    return cortez::boot::BootInfo::instance().physToVirt<void*>(phys_addr);
}

uint64_t k_virt_to_phys(const void* virt_addr) {
    if (!virt_addr) return 0;
    auto res = cortez::memory::PageTableVmm::instance().translateVirtToPhys(
        reinterpret_cast<cortez::virt_addr_t>(virt_addr));
    if (res.isOk()) {
        return res.value();
    }
    return cortez::boot::BootInfo::instance().virtToPhys(
        reinterpret_cast<cortez::virt_addr_t>(virt_addr));
}

void* k_map_mmio(uint64_t phys_addr, size_t size) {
    auto res = cortez::memory::PageTableVmm::instance().mapMmio(phys_addr, size);
    if (res.isError()) {
        return nullptr;
    }
    return reinterpret_cast<void*>(res.value());
}

/* ============================================================================
 * 3. x86_64 Port I/O
 * ========================================================================== */

uint8_t k_inb(uint16_t port) {
    return cortez::arch::inb(port);
}

void k_outb(uint16_t port, uint8_t val) {
    cortez::arch::outb(port, val);
}

uint16_t k_inw(uint16_t port) {
    return cortez::arch::inw(port);
}

void k_outw(uint16_t port, uint16_t val) {
    cortez::arch::outw(port, val);
}

uint32_t k_inl(uint16_t port) {
    return cortez::arch::inl(port);
}

void k_outl(uint16_t port, uint32_t val) {
    cortez::arch::outl(port, val);
}

void k_io_wait(void) {
    cortez::arch::ioWait();
}

/* ============================================================================
 * 4. Interrupts & APIC
 * ========================================================================== */

int k_irq_alloc_vector(void) {
    return cortez::arch::IdtManager::instance().allocateVector();
}

int k_irq_register(uint8_t vector, cortez_irq_fn handler, void* ctx) {
    return cortez::module::ModuleManager::instance().registerIrqForCurrent(vector, handler, ctx);
}

void k_irq_route_isa(uint8_t isa_irq, uint8_t vector) {
    cortez::arch::ApicController::instance().routeIsaIrq(isa_irq, vector, false);
    cortez::module::ModuleManager::instance().recordIsaIrqRoute(isa_irq, vector);
}

void k_irq_unregister(uint8_t vector) {
    cortez::module::ModuleManager::instance().unregisterIrq(vector);
}

/* ============================================================================
 * 5. PCI / PCIe Bus Access
 * ========================================================================== */

size_t k_pci_count(void) {
    return cortez::arch::PcieManager::instance().getDeviceCount();
}

int k_pci_get(size_t index, cortez_pci_dev* out_dev) {
    auto& pcie = cortez::arch::PcieManager::instance();
    if (!out_dev || index >= pcie.getDeviceCount()) {
        return -1;
    }
    const auto& d = pcie.getDevice(index);
    out_dev->segment     = d.segment;
    out_dev->bus         = d.bus;
    out_dev->device      = d.device;
    out_dev->function    = d.function;
    out_dev->vendor_id   = d.vendorId;
    out_dev->device_id   = d.deviceId;
    out_dev->class_code  = d.classCode;
    out_dev->subclass    = d.subclass;
    out_dev->prog_if     = d.progIf;
    out_dev->revision_id = d.revisionId;
    out_dev->irq_line    = d.irqLine;
    out_dev->bar0_phys   = d.bar0Phys;
    out_dev->bar0_size   = d.bar0Size;
    return 0;
}

uint32_t k_pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset) {
    return cortez::arch::PcieManager::instance().readConfig32(bus, dev, func, offset);
}

void k_pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t val) {
    cortez::arch::PcieManager::instance().writeConfig32(bus, dev, func, offset, val);
}

void k_pci_enable_busmaster(const cortez_pci_dev* dev) {
    if (!dev) return;
    cortez::arch::PciDeviceInfo info{};
    info.bus      = dev->bus;
    info.device   = dev->device;
    info.function = dev->function;
    cortez::arch::PcieManager::instance().enableBusMastering(info);
}

bool k_pci_enable_msi(const cortez_pci_dev* dev, uint8_t vector) {
    if (!dev) return false;
    cortez::arch::PciDeviceInfo info{};
    info.bus      = dev->bus;
    info.device   = dev->device;
    info.function = dev->function;
    cortez::u32 apicId = cortez::arch::ApicController::instance().getApicId();
    return cortez::arch::PcieManager::instance().enableMsiOrMsix(info, vector, apicId);
}

/* ============================================================================
 * 6. Dynamic Shell Command & Char Device Registration
 * ========================================================================== */

int k_register_command(const char* name, const char* desc, const char* usage, cortez_cmd_fn fn) {
    return cortez::module::ModuleManager::instance().registerCommandForCurrent(name, desc, usage, fn);
}

int k_unregister_command(const char* name) {
    return cortez::module::ModuleManager::instance().unregisterCommandByName(name);
}

int k_register_chardev(const char* name, const cortez_chardev_ops* ops, void* ctx) {
    return cortez::module::ModuleManager::instance().registerCharDevForCurrent(name, ops, ctx);
}

int k_unregister_chardev(const char* name) {
    return cortez::module::ModuleManager::instance().unregisterCharDevByName(name);
}

/* ============================================================================
 * 8. VFS File Operations
 * ========================================================================== */

int k_open(const char* path, uint32_t flags) {
    if (!path) return -1;
    const char* cwd = "/";
    if (auto* task = cortez::proc::Scheduler::instance().getCurrentTask()) {
        cwd = task->getCwd();
    }
    auto res = cortez::fs::VfsManager::instance().open(cwd, path, flags);
    if (res.isError()) return -1;
    return res.value();
}

int64_t k_read(int fd, void* buf, size_t count) {
    auto res = cortez::fs::VfsManager::instance().read(fd, buf, count);
    if (res.isError()) return -1;
    return static_cast<int64_t>(res.value());
}

int64_t k_write(int fd, const void* buf, size_t count) {
    auto res = cortez::fs::VfsManager::instance().write(fd, buf, count);
    if (res.isError()) return -1;
    return static_cast<int64_t>(res.value());
}

int64_t k_lseek(int fd, int64_t offset, int whence) {
    auto res = cortez::fs::VfsManager::instance().seek(fd, offset, whence);
    if (res.isError()) return -1;
    return static_cast<int64_t>(res.value());
}

int k_close(int fd) {
    return (cortez::fs::VfsManager::instance().close(fd) == cortez::Status::Ok) ? 0 : -1;
}

/* ============================================================================
 * 9. Scheduler, Tasks & Timing
 * ========================================================================== */

int k_task_spawn(const char* name, cortez_task_fn entry, void* arg) {
    cortez::u32 ownerMod = cortez::module::ModuleManager::instance().getActiveModuleId();
    auto res = cortez::proc::Scheduler::instance().spawnTask(name, entry, arg, ownerMod);
    if (res.isError()) return -1;
    return static_cast<int>(res.value());
}

int k_task_kill(uint32_t pid) {
    return (cortez::proc::Scheduler::instance().killTask(pid) == cortez::Status::Ok) ? 0 : -1;
}

void k_yield(void) {
    cortez::proc::Scheduler::instance().yield();
}

void k_sleep_ms(uint64_t ms) {
    cortez::proc::Scheduler::instance().sleepMs(ms);
}

uint64_t k_ticks(void) {
    return cortez::arch::ApicController::instance().getUptimeMs();
}

void k_delay_us(uint64_t us) {
    // Each port 0x80 I/O wait takes ~1 microsecond on standard x86 chipsets
    for (uint64_t i = 0; i < us; ++i) {
        cortez::arch::ioWait();
    }
}

} // extern "C"

namespace cortez::module {

void registerKernelSymbols(TCCState* s) noexcept {
    if (!s) return;

    // 1. Console & Logging
    tcc_add_symbol(s, "k_printf",  reinterpret_cast<const void*>(&k_printf));
    tcc_add_symbol(s, "k_log",     reinterpret_cast<const void*>(&k_log));
    tcc_add_symbol(s, "k_getchar", reinterpret_cast<const void*>(&k_getchar));
    tcc_add_symbol(s, "k_kbhit",   reinterpret_cast<const void*>(&k_kbhit));

    // 2. Memory Management
    tcc_add_symbol(s, "k_malloc",       reinterpret_cast<const void*>(&k_malloc));
    tcc_add_symbol(s, "k_calloc",       reinterpret_cast<const void*>(&k_calloc));
    tcc_add_symbol(s, "k_realloc",      reinterpret_cast<const void*>(&k_realloc));
    tcc_add_symbol(s, "k_free",         reinterpret_cast<const void*>(&k_free));
    tcc_add_symbol(s, "k_alloc_pages",  reinterpret_cast<const void*>(&k_alloc_pages));
    tcc_add_symbol(s, "k_free_pages",   reinterpret_cast<const void*>(&k_free_pages));
    tcc_add_symbol(s, "k_phys_to_virt", reinterpret_cast<const void*>(&k_phys_to_virt));
    tcc_add_symbol(s, "k_virt_to_phys", reinterpret_cast<const void*>(&k_virt_to_phys));
    tcc_add_symbol(s, "k_map_mmio",     reinterpret_cast<const void*>(&k_map_mmio));

    // 3. Port I/O
    tcc_add_symbol(s, "k_inb",     reinterpret_cast<const void*>(&k_inb));
    tcc_add_symbol(s, "k_outb",    reinterpret_cast<const void*>(&k_outb));
    tcc_add_symbol(s, "k_inw",     reinterpret_cast<const void*>(&k_inw));
    tcc_add_symbol(s, "k_outw",    reinterpret_cast<const void*>(&k_outw));
    tcc_add_symbol(s, "k_inl",     reinterpret_cast<const void*>(&k_inl));
    tcc_add_symbol(s, "k_outl",    reinterpret_cast<const void*>(&k_outl));
    tcc_add_symbol(s, "k_io_wait", reinterpret_cast<const void*>(&k_io_wait));

    // 4. Interrupts & APIC
    tcc_add_symbol(s, "k_irq_alloc_vector", reinterpret_cast<const void*>(&k_irq_alloc_vector));
    tcc_add_symbol(s, "k_irq_register",     reinterpret_cast<const void*>(&k_irq_register));
    tcc_add_symbol(s, "k_irq_route_isa",    reinterpret_cast<const void*>(&k_irq_route_isa));
    tcc_add_symbol(s, "k_irq_unregister",   reinterpret_cast<const void*>(&k_irq_unregister));

    // 5. PCI / PCIe
    tcc_add_symbol(s, "k_pci_count",            reinterpret_cast<const void*>(&k_pci_count));
    tcc_add_symbol(s, "k_pci_get",              reinterpret_cast<const void*>(&k_pci_get));
    tcc_add_symbol(s, "k_pci_read32",           reinterpret_cast<const void*>(&k_pci_read32));
    tcc_add_symbol(s, "k_pci_write32",          reinterpret_cast<const void*>(&k_pci_write32));
    tcc_add_symbol(s, "k_pci_enable_busmaster", reinterpret_cast<const void*>(&k_pci_enable_busmaster));
    tcc_add_symbol(s, "k_pci_enable_msi",       reinterpret_cast<const void*>(&k_pci_enable_msi));

    // 6. Commands & Char Devices
    tcc_add_symbol(s, "k_register_command",   reinterpret_cast<const void*>(&k_register_command));
    tcc_add_symbol(s, "k_unregister_command", reinterpret_cast<const void*>(&k_unregister_command));
    tcc_add_symbol(s, "k_register_chardev",   reinterpret_cast<const void*>(&k_register_chardev));
    tcc_add_symbol(s, "k_unregister_chardev", reinterpret_cast<const void*>(&k_unregister_chardev));

    // 7. VFS
    tcc_add_symbol(s, "k_open",  reinterpret_cast<const void*>(&k_open));
    tcc_add_symbol(s, "k_read",  reinterpret_cast<const void*>(&k_read));
    tcc_add_symbol(s, "k_write", reinterpret_cast<const void*>(&k_write));
    tcc_add_symbol(s, "k_lseek", reinterpret_cast<const void*>(&k_lseek));
    tcc_add_symbol(s, "k_close", reinterpret_cast<const void*>(&k_close));

    // 8. Scheduler & Timing
    tcc_add_symbol(s, "k_task_spawn", reinterpret_cast<const void*>(&k_task_spawn));
    tcc_add_symbol(s, "k_task_kill",  reinterpret_cast<const void*>(&k_task_kill));
    tcc_add_symbol(s, "k_yield",      reinterpret_cast<const void*>(&k_yield));
    tcc_add_symbol(s, "k_sleep_ms",   reinterpret_cast<const void*>(&k_sleep_ms));
    tcc_add_symbol(s, "k_ticks",      reinterpret_cast<const void*>(&k_ticks));
    tcc_add_symbol(s, "k_delay_us",   reinterpret_cast<const void*>(&k_delay_us));

    // 9. Standard C library aliases (stdio.h, stdlib.h, string.h)
    tcc_add_symbol(s, "printf",     reinterpret_cast<const void*>(&k_printf));
    tcc_add_symbol(s, "snprintf",   reinterpret_cast<const void*>(&klibc_snprintf));
    tcc_add_symbol(s, "sprintf",    reinterpret_cast<const void*>(&klibc_sprintf));
    tcc_add_symbol(s, "malloc",     reinterpret_cast<const void*>(&klibc_malloc));
    tcc_add_symbol(s, "calloc",     reinterpret_cast<const void*>(&klibc_calloc));
    tcc_add_symbol(s, "realloc",    reinterpret_cast<const void*>(&klibc_realloc));
    tcc_add_symbol(s, "free",       reinterpret_cast<const void*>(&klibc_free));
    tcc_add_symbol(s, "atoi",       reinterpret_cast<const void*>(&klibc_atoi));
    tcc_add_symbol(s, "strtol",     reinterpret_cast<const void*>(&klibc_strtol));
    tcc_add_symbol(s, "strtoul",    reinterpret_cast<const void*>(&klibc_strtoul));
    tcc_add_symbol(s, "strlen",     reinterpret_cast<const void*>(&klibc_strlen));
    tcc_add_symbol(s, "strcpy",     reinterpret_cast<const void*>(&klibc_strcpy));
    tcc_add_symbol(s, "strncpy",    reinterpret_cast<const void*>(&klibc_strncpy));
    tcc_add_symbol(s, "strcat",     reinterpret_cast<const void*>(&klibc_strcat));
    tcc_add_symbol(s, "strcmp",     reinterpret_cast<const void*>(&klibc_strcmp));
    tcc_add_symbol(s, "strncmp",    reinterpret_cast<const void*>(&klibc_strncmp));
    tcc_add_symbol(s, "strcasecmp", reinterpret_cast<const void*>(&klibc_strcasecmp));
    tcc_add_symbol(s, "strchr",     reinterpret_cast<const void*>(&klibc_strchr));
    tcc_add_symbol(s, "strrchr",    reinterpret_cast<const void*>(&klibc_strrchr));
    tcc_add_symbol(s, "strstr",     reinterpret_cast<const void*>(&klibc_strstr));
    tcc_add_symbol(s, "strdup",     reinterpret_cast<const void*>(&klibc_strdup));
    tcc_add_symbol(s, "memcpy",     reinterpret_cast<const void*>(&memcpy));
    tcc_add_symbol(s, "memset",     reinterpret_cast<const void*>(&memset));
    tcc_add_symbol(s, "memmove",    reinterpret_cast<const void*>(&memmove));
    tcc_add_symbol(s, "memcmp",     reinterpret_cast<const void*>(&memcmp));
}

} // namespace cortez::module
