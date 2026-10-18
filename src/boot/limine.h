/**
 * @file limine.h
 * @brief Limine Boot Protocol specification headers for x86_64 UEFI/BIOS boot.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL

#define LIMINE_BASE_REVISION(N) \
    uint64_t limine_base_revision[3] = { 0xf9562b2d5c95a6c8ULL, 0x6a7b384944536bdcULL, (N) };

#define LIMINE_REQUESTS_START_MARKER \
    uint64_t limine_requests_start_marker[4] = { \
        0xf6b8f4b39de7d1aeULL, 0xfab91a6940fcb9cfULL, \
        0x785c6ed015d3e316ULL, 0x181e920a7852b9d9ULL };

#define LIMINE_REQUESTS_END_MARKER \
    uint64_t limine_requests_end_marker[2] = { \
        0xadc0e0531bb10d03ULL, 0x9572709f31764c62ULL };

/* Framebuffer Request */
#define LIMINE_FRAMEBUFFER_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75ULL, 0xa3148604f6fab11bULL }

struct limine_video_mode {
    uint64_t pitch;
    uint64_t width;
    uint64_t height;
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
};

struct limine_framebuffer {
    void*                     address;
    uint64_t                  width;
    uint64_t                  height;
    uint64_t                  pitch;
    uint16_t                  bpp;
    uint8_t                   memory_model;
    uint8_t                   red_mask_size;
    uint8_t                   red_mask_shift;
    uint8_t                   green_mask_size;
    uint8_t                   green_mask_shift;
    uint8_t                   blue_mask_size;
    uint8_t                   blue_mask_shift;
    uint8_t                   unused[7];
    uint64_t                  edid_size;
    void*                     edid;
    uint64_t                  mode_count;
    struct limine_video_mode** modes;
};

struct limine_framebuffer_response {
    uint64_t                    revision;
    uint64_t                    framebuffer_count;
    struct limine_framebuffer** framebuffers;
};

struct limine_framebuffer_request {
    uint64_t                            id[4];
    uint64_t                            revision;
    struct limine_framebuffer_response* response;
};

/* Higher-Half Direct Map (HHDM) Request */
#define LIMINE_HHDM_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852ULL, 0x63984e959a98244bULL }

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

struct limine_hhdm_request {
    uint64_t                     id[4];
    uint64_t                     revision;
    struct limine_hhdm_response* response;
};

/* Memory Map Request */
#define LIMINE_MEMMAP_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806fULL, 0xe304acdfc50c3c62ULL }

#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES     6
#define LIMINE_MEMMAP_FRAMEBUFFER            7

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t                     revision;
    uint64_t                     entry_count;
    struct limine_memmap_entry** entries;
};

struct limine_memmap_request {
    uint64_t                       id[4];
    uint64_t                       revision;
    struct limine_memmap_response* response;
};

/* RSDP (ACPI) Request */
#define LIMINE_RSDP_REQUEST \
    { LIMINE_COMMON_MAGIC, 0xc5e77b6b397e7b43ULL, 0x27637845accdcf3cULL }

struct limine_rsdp_response {
    uint64_t revision;
    void*    address;
};

struct limine_rsdp_request {
    uint64_t                     id[4];
    uint64_t                     revision;
    struct limine_rsdp_response* response;
};

/* Kernel Address Request */
#define LIMINE_KERNEL_ADDRESS_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x71ba76863cc55f63ULL, 0xb2644a48c516a487ULL }

struct limine_kernel_address_response {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
};

struct limine_kernel_address_request {
    uint64_t                               id[4];
    uint64_t                               revision;
    struct limine_kernel_address_response* response;
};

/* Kernel File Request (provides cmdline and kernel ELF info) */
#define LIMINE_KERNEL_FILE_REQUEST \
    { LIMINE_COMMON_MAGIC, 0xad97e90e83f1ed67ULL, 0x31eb5d1c5ff23b69ULL }

struct limine_uuid {
    uint32_t a;
    uint16_t b;
    uint16_t c;
    uint8_t  d[8];
};

struct limine_file {
    uint64_t           revision;
    void*              address;
    uint64_t           size;
    char*              path;
    char*              cmdline;
    uint32_t           media_type;
    uint32_t           unused;
    uint32_t           tftp_ip;
    uint32_t           tftp_port;
    uint32_t           partition_index;
    uint32_t           mbr_disk_id;
    struct limine_uuid gpt_disk_uuid;
    struct limine_uuid gpt_part_uuid;
    struct limine_uuid part_uuid;
};

struct limine_kernel_file_response {
    uint64_t            revision;
    struct limine_file* kernel_file;
};

struct limine_kernel_file_request {
    uint64_t                            id[4];
    uint64_t                            revision;
    struct limine_kernel_file_response* response;
};

#ifdef __cplusplus
}
#endif
