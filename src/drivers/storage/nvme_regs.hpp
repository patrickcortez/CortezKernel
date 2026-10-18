/**
 * @file nvme_regs.hpp
 * @brief NVM Express (NVMe 1.4) hardware register offsets, SQE/CQE descriptors, and Identify structures.
 */

#pragma once

#include "../../lib/types.hpp"

namespace cortez::drivers::storage {

// NVMe BAR0 MMIO Register Offsets
constexpr u32 NVME_REG_CAP   = 0x0000; // 64-bit Controller Capabilities
constexpr u32 NVME_REG_VS    = 0x0008; // 32-bit Version
constexpr u32 NVME_REG_INTMS = 0x000C; // 32-bit Interrupt Mask Set
constexpr u32 NVME_REG_INTMC = 0x0010; // 32-bit Interrupt Mask Clear
constexpr u32 NVME_REG_CC    = 0x0014; // 32-bit Controller Configuration
constexpr u32 NVME_REG_CSTS  = 0x001C; // 32-bit Controller Status
constexpr u32 NVME_REG_AQA   = 0x0024; // 32-bit Admin Queue Attributes
constexpr u32 NVME_REG_ASQ   = 0x0028; // 64-bit Admin Submission Queue Base Address
constexpr u32 NVME_REG_ACQ   = 0x0030; // 64-bit Admin Completion Queue Base Address
constexpr u32 NVME_REG_DBS   = 0x1000; // Doorbell Base Offset

// Admin Opcodes
constexpr u8 NVME_ADMIN_DELETE_SQ = 0x00;
constexpr u8 NVME_ADMIN_CREATE_SQ = 0x01;
constexpr u8 NVME_ADMIN_DELETE_CQ = 0x04;
constexpr u8 NVME_ADMIN_CREATE_CQ = 0x05;
constexpr u8 NVME_ADMIN_IDENTIFY  = 0x06;
constexpr u8 NVME_ADMIN_SET_FEAT  = 0x09;

// NVM I/O Opcodes
constexpr u8 NVME_IO_FLUSH = 0x00;
constexpr u8 NVME_IO_WRITE = 0x01;
constexpr u8 NVME_IO_READ  = 0x02;

/**
 * @brief 64-byte NVMe Submission Queue Entry (SQE).
 */
struct CORTEZ_PACKED NvmeSqe {
    u8  opcode;
    u8  flags;
    u16 cid;
    u32 nsid;
    u64 reserved0;
    u64 mptr;
    u64 prp1;
    u64 prp2;
    u32 cdw10;
    u32 cdw11;
    u32 cdw12;
    u32 cdw13;
    u32 cdw14;
    u32 cdw15;
};
static_assert(sizeof(NvmeSqe) == 64, "NvmeSqe must be exactly 64 bytes");

/**
 * @brief 16-byte NVMe Completion Queue Entry (CQE).
 */
struct CORTEZ_PACKED NvmeCqe {
    u32 result;
    u32 reserved;
    u16 sqHead;
    u16 sqId;
    u16 cid;
    u16 status; // Bit 0 = Phase Tag (P), Bits 1..15 = Status Code
};
static_assert(sizeof(NvmeCqe) == 16, "NvmeCqe must be exactly 16 bytes");

/**
 * @brief 4096-byte NVMe Identify Controller structure.
 */
struct CORTEZ_PACKED NvmeIdentifyController {
    u16 vid;
    u16 ssvid;
    char sn[20];
    char mn[40];
    char fr[8];
    u8  rab;
    u8  ieee[3];
    u8  cmic;
    u8  mdts;
    u16 cntlid;
    u32 ver;
    u8  pad0[516 - 84];
    u32 nn; // Offset 516: Number of Namespaces
    u8  pad1[4096 - 520];
};
static_assert(sizeof(NvmeIdentifyController) == 4096, "NvmeIdentifyController must be 4096 bytes");

struct CORTEZ_PACKED NvmeLbaf {
    u16 ms;    // Metadata Size
    u8  lbads; // LBA Data Size (2^lbads bytes)
    u8  rp;    // Relative Performance
};

/**
 * @brief 4096-byte NVMe Identify Namespace structure.
 */
struct CORTEZ_PACKED NvmeIdentifyNamespace {
    u64      nsze;  // Namespace Size in logical blocks
    u64      ncap;  // Namespace Capacity
    u64      nuse;  // Namespace Utilization
    u8       nsfeat;
    u8       nlbaf; // Number of LBA Formats (0-based)
    u8       flbas; // Formatted LBA Size (bits 0..3 index into lbaf)
    u8       mc;
    u8       dpc;
    u8       dps;
    u8       nmic;
    u8       rescap;
    u8       fpi;
    u8       dlfeat;
    u16      nawun;
    u16      nawupf;
    u16      nacwu;
    u16      nabsn;
    u16      nabo;
    u16      nabspf;
    u16      noiob;
    u8       nvmcap[16];
    u8       pad0[128 - 64];
    NvmeLbaf lbaf[16];
    u8       pad1[4096 - 192];
};
static_assert(sizeof(NvmeIdentifyNamespace) == 4096, "NvmeIdentifyNamespace must be 4096 bytes");

} // namespace cortez::drivers::storage
