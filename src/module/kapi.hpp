/**
 * @file kapi.hpp
 * @brief Stable C ABI Kernel Symbol Table (cortez.h) exposed to runtime-compiled C modules and programs.
 */

#pragma once

#include "../lib/types.hpp"

struct TCCState;

extern "C" {

struct cortez_pci_dev {
    uint16_t segment;
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision_id;
    uint8_t  irq_line;
    uint64_t bar0_phys;
    uint64_t bar0_size;
};

struct cortez_chardev_ops {
    int64_t (*read)(void* ctx, void* buf, size_t count);
    int64_t (*write)(void* ctx, const void* buf, size_t count);
    bool    (*has_data)(void* ctx);
};

typedef int  (*cortez_cmd_fn)(int argc, const char** argv);
typedef void (*cortez_irq_fn)(uint8_t vec, void* ctx);
typedef void (*cortez_task_fn)(void* arg);

/* 1. Console & Logging */
void k_printf(const char* fmt, ...);
void k_log(int level, const char* subsys, const char* fmt, ...);
int  k_getchar(void);
bool k_kbhit(void);

/* 2. Memory Management (Heap, Physical Pages, HHDM, MMIO) */
void*    k_malloc(size_t size);
void*    k_calloc(size_t nmemb, size_t size);
void*    k_realloc(void* ptr, size_t size);
void     k_free(void* ptr);
uint64_t k_alloc_pages(size_t count);
void     k_free_pages(uint64_t phys_addr, size_t count);
void*    k_phys_to_virt(uint64_t phys_addr);
uint64_t k_virt_to_phys(const void* virt_addr);
void*    k_map_mmio(uint64_t phys_addr, size_t size);

/* 3. x86_64 Port I/O */
uint8_t  k_inb(uint16_t port);
void     k_outb(uint16_t port, uint8_t val);
uint16_t k_inw(uint16_t port);
void     k_outw(uint16_t port, uint16_t val);
uint32_t k_inl(uint16_t port);
void     k_outl(uint16_t port, uint32_t val);
void     k_io_wait(void);

/* 4. Interrupts & APIC */
int  k_irq_alloc_vector(void);
int  k_irq_register(uint8_t vector, cortez_irq_fn handler, void* ctx);
void k_irq_route_isa(uint8_t isa_irq, uint8_t vector);
void k_irq_unregister(uint8_t vector);

/* 5. PCI / PCIe Bus Access */
size_t   k_pci_count(void);
int      k_pci_get(size_t index, cortez_pci_dev* out_dev);
uint32_t k_pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
void     k_pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset, uint32_t val);
void     k_pci_enable_busmaster(const cortez_pci_dev* dev);
bool     k_pci_enable_msi(const cortez_pci_dev* dev, uint8_t vector);

/* 6. Dynamic Shell Command Registration */
int k_register_command(const char* name, const char* desc, const char* usage, cortez_cmd_fn fn);
int k_unregister_command(const char* name);

/* 7. Dynamic Character Device Registration (/dev/<name>) */
int k_register_chardev(const char* name, const cortez_chardev_ops* ops, void* ctx);
int k_unregister_chardev(const char* name);

/* 8. VFS File Operations */
int     k_open(const char* path, uint32_t flags);
int64_t k_read(int fd, void* buf, size_t count);
int64_t k_write(int fd, const void* buf, size_t count);
int64_t k_lseek(int fd, int64_t offset, int whence);
int     k_close(int fd);

/* 9. Scheduler, Tasks & Timing */
int      k_task_spawn(const char* name, cortez_task_fn entry, void* arg);
int      k_task_kill(uint32_t pid);
void     k_yield(void);
void     k_sleep_ms(uint64_t ms);
uint64_t k_ticks(void);
void     k_delay_us(uint64_t us);

} // extern "C"

namespace cortez::module {

/**
 * @brief Registers all k_* kernel APIs and standard C library functions into a TCC compilation context.
 */
void registerKernelSymbols(TCCState* s) noexcept;

} // namespace cortez::module
