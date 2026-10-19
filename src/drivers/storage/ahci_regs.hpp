/**
 * @file ahci_regs.hpp
 * @brief Serial ATA Advanced Host Controller Interface (AHCI 1.0–1.3.1) Register & FIS Definitions.
 */

#pragma once

#include "../../lib/types.hpp"

namespace cortez::drivers::storage {

// =============================================================================
// Generic Host Control (GHC) Register Offsets (relative to ABAR / BAR5 MMIO)
// =============================================================================
constexpr u32 AHCI_REG_CAP       = 0x00U; // Host Capabilities (32-bit)
constexpr u32 AHCI_REG_GHC       = 0x04U; // Global Host Control (32-bit)
constexpr u32 AHCI_REG_IS        = 0x08U; // Interrupt Status (32-bit)
constexpr u32 AHCI_REG_PI        = 0x0CU; // Ports Implemented (32-bit bitmask)
constexpr u32 AHCI_REG_VS        = 0x10U; // AHCI Version (32-bit: major.minor)
constexpr u32 AHCI_REG_CCC_CTL   = 0x14U; // Command Completion Coalescing Control
constexpr u32 AHCI_REG_CCC_PORTS = 0x18U; // Command Completion Coalescing Ports
constexpr u32 AHCI_REG_EM_LOC    = 0x1CU; // Enclosure Management Location
constexpr u32 AHCI_REG_EM_CTL    = 0x20U; // Enclosure Management Control
constexpr u32 AHCI_REG_CAP2      = 0x24U; // Host Capabilities Extended (AHCI 1.2+)
constexpr u32 AHCI_REG_BOHC      = 0x28U; // BIOS/OS Handoff Control and Status (AHCI 1.2+)

// GHC Flags
constexpr u32 AHCI_GHC_HR   = 1U << 0;  // HBA Reset
constexpr u32 AHCI_GHC_IE   = 1U << 1;  // Interrupt Enable
constexpr u32 AHCI_GHC_MRSM = 1U << 2;  // MSI Revert to Single Message
constexpr u32 AHCI_GHC_AE   = 1U << 31; // AHCI Enable

// CAP Flags
constexpr u32 AHCI_CAP_S64A = 1U << 31; // Supports 64-bit Addressing
constexpr u32 AHCI_CAP_SNCQ = 1U << 30; // Supports Native Command Queuing
constexpr u32 AHCI_CAP_SSNTF= 1U << 29; // Supports SNotification Register
constexpr u32 AHCI_CAP_SMPS = 1U << 28; // Supports Interlock Switch / Mech Presence
constexpr u32 AHCI_CAP_SSS  = 1U << 27; // Supports Staggered Spin-Up
constexpr u32 AHCI_CAP_SALP = 1U << 26; // Supports Aggressive Link Power Management
constexpr u32 AHCI_CAP_SPM  = 1U << 17; // Supports Port Multiplier
constexpr u32 AHCI_CAP_FBSS = 1U << 16; // FIS-Based Switching Supported
constexpr u32 AHCI_CAP_PMD  = 1U << 15; // PIO Multiple DRQ Block
constexpr u32 AHCI_CAP_SAM  = 1U << 18; // Supports AHCI Mode Only

// CAP2 & BOHC Flags
constexpr u32 AHCI_CAP2_BOH  = 1U << 0; // BIOS/OS Handoff Supported
constexpr u32 AHCI_BOHC_BOS  = 1U << 0; // BIOS Owned Semaphore
constexpr u32 AHCI_BOHC_OOS  = 1U << 1; // OS Owned Semaphore
constexpr u32 AHCI_BOHC_SOOE = 1U << 2; // SMI on OS Ownership Enable
constexpr u32 AHCI_BOHC_OOC  = 1U << 3; // OS Ownership Change
constexpr u32 AHCI_BOHC_BB   = 1U << 4; // BIOS Busy

// =============================================================================
// Port Register Offsets (relative to ABAR + 0x100 + portIndex * 0x80)
// =============================================================================
constexpr u32 AHCI_PORT_OFFSET_BASE   = 0x100U;
constexpr u32 AHCI_PORT_OFFSET_STRIDE = 0x80U;

constexpr u32 AHCI_PORT_CLB  = 0x00U; // Command List Base (Low 32-bit, 1 KiB aligned)
constexpr u32 AHCI_PORT_CLBU = 0x04U; // Command List Base (Upper 32-bit)
constexpr u32 AHCI_PORT_FB   = 0x08U; // FIS Base (Low 32-bit, 256-byte aligned)
constexpr u32 AHCI_PORT_FBU  = 0x0CU; // FIS Base (Upper 32-bit)
constexpr u32 AHCI_PORT_IS   = 0x10U; // Port Interrupt Status
constexpr u32 AHCI_PORT_IE   = 0x14U; // Port Interrupt Enable
constexpr u32 AHCI_PORT_CMD  = 0x18U; // Port Command and Status
constexpr u32 AHCI_PORT_TFD  = 0x20U; // Task File Data (Status & Error)
constexpr u32 AHCI_PORT_SIG  = 0x24U; // Signature
constexpr u32 AHCI_PORT_SSTS = 0x28U; // Serial ATA Status (SCR0: SStatus)
constexpr u32 AHCI_PORT_SCTL = 0x2CU; // Serial ATA Control (SCR2: SControl)
constexpr u32 AHCI_PORT_SERR = 0x30U; // Serial ATA Error (SCR1: SError)
constexpr u32 AHCI_PORT_SACT = 0x34U; // Serial ATA Active (SCR3: SActive)
constexpr u32 AHCI_PORT_CI   = 0x38U; // Command Issue (bitmask)
constexpr u32 AHCI_PORT_SNTF = 0x3CU; // Serial ATA Notification (SCR4: SNotification)
constexpr u32 AHCI_PORT_FBS  = 0x40U; // FIS-Based Switching Control

// Port CMD Flags
constexpr u32 AHCI_PORT_CMD_ST    = 1U << 0;  // Start DMA Engine
constexpr u32 AHCI_PORT_CMD_SUD   = 1U << 1;  // Spin-Up Device
constexpr u32 AHCI_PORT_CMD_POD   = 1U << 2;  // Power On Device
constexpr u32 AHCI_PORT_CMD_CLO   = 1U << 3;  // Command List Override
constexpr u32 AHCI_PORT_CMD_FRE   = 1U << 4;  // FIS Receive Enable
constexpr u32 AHCI_PORT_CMD_FR    = 1U << 14; // FIS Receive Running
constexpr u32 AHCI_PORT_CMD_CR    = 1U << 15; // Command List Running
constexpr u32 AHCI_PORT_CMD_ATAPI = 1U << 24; // Device is ATAPI
constexpr u32 AHCI_PORT_CMD_ICC_ACTIVE = 1U << 28; // Interface Power State: Active

// Port SSTS (SStatus) Fields
constexpr u32 AHCI_SSTS_DET_MASK    = 0x0FU;
constexpr u32 AHCI_SSTS_DET_NONE    = 0x00U; // No device detected
constexpr u32 AHCI_SSTS_DET_NOPHY   = 0x01U; // Device detected, no PHY communication established
constexpr u32 AHCI_SSTS_DET_PRESENT = 0x03U; // Device present and PHY communication established
constexpr u32 AHCI_SSTS_DET_OFFLINE = 0x04U; // PHY offline

constexpr u32 AHCI_SSTS_SPD_MASK    = 0x0FU << 4;
constexpr u32 AHCI_SSTS_SPD_GEN1    = 0x01U << 4; // SATA-I  (1.5 Gbps)
constexpr u32 AHCI_SSTS_SPD_GEN2    = 0x02U << 4; // SATA-II (3.0 Gbps)
constexpr u32 AHCI_SSTS_SPD_GEN3    = 0x03U << 4; // SATA-III (6.0 Gbps)

constexpr u32 AHCI_SSTS_IPM_MASK    = 0x0FU << 8;
constexpr u32 AHCI_SSTS_IPM_ACTIVE  = 0x01U << 8;

// Port SCTL (SControl) Fields
constexpr u32 AHCI_SCTL_DET_NOOP    = 0x00U;
constexpr u32 AHCI_SCTL_DET_RESET   = 0x01U; // Perform interface reset (COMRESET)
constexpr u32 AHCI_SCTL_DET_DISABLE = 0x04U; // Disable PHY
constexpr u32 AHCI_SCTL_IPM_NODIS   = 0x00U; // No power management restrictions

// Port TFD (Task File Data) Status & Error Bits
constexpr u32 AHCI_TFD_STS_ERR = 1U << 0; // Error bit
constexpr u32 AHCI_TFD_STS_DRQ = 1U << 3; // Data Request bit
constexpr u32 AHCI_TFD_STS_BSY = 1U << 7; // Busy bit

// Port Interrupt Status Bits
constexpr u32 AHCI_PORT_IS_DHRS = 1U << 0;  // Device to Host Register FIS
constexpr u32 AHCI_PORT_IS_PSS  = 1U << 1;  // PIO Setup FIS
constexpr u32 AHCI_PORT_IS_DSS  = 1U << 2;  // DMA Setup FIS
constexpr u32 AHCI_PORT_IS_SDBS = 1U << 3;  // Set Device Bits
constexpr u32 AHCI_PORT_IS_DPS  = 1U << 5;  // Descriptor Processed
constexpr u32 AHCI_PORT_IS_TFES = 1U << 30; // Task File Error
constexpr u32 AHCI_PORT_IS_HBFS = 1U << 29; // Host Bus Fatal Error
constexpr u32 AHCI_PORT_IS_HBDS = 1U << 28; // Host Bus Data Error
constexpr u32 AHCI_PORT_IS_IFS  = 1U << 27; // Interface Fatal Error

constexpr u32 AHCI_PORT_IS_FATAL_ERRORS =
    AHCI_PORT_IS_TFES | AHCI_PORT_IS_HBFS | AHCI_PORT_IS_HBDS | AHCI_PORT_IS_IFS;

// =============================================================================
// ATA Signatures (PxSIG values)
// =============================================================================
constexpr u32 SATA_SIG_ATA   = 0x00000101U; // SATA Hard Disk / Solid State Drive
constexpr u32 SATA_SIG_ATAPI = 0xEB140101U; // SATAPI CD/DVD Optical Drive
constexpr u32 SATA_SIG_SEMB  = 0xC33C0101U; // Enclosure Management Bridge
constexpr u32 SATA_SIG_PM    = 0x96690101U; // Port Multiplier

// =============================================================================
// ATA Commands
// =============================================================================
constexpr u8 ATA_CMD_READ_DMA_EXT     = 0x25U; // Read sectors with 48-bit LBA
constexpr u8 ATA_CMD_WRITE_DMA_EXT    = 0x35U; // Write sectors with 48-bit LBA
constexpr u8 ATA_CMD_READ_DMA         = 0xC8U; // Read sectors with 28-bit LBA
constexpr u8 ATA_CMD_WRITE_DMA        = 0xCAU; // Write sectors with 28-bit LBA
constexpr u8 ATA_CMD_FLUSH_EXT        = 0xEAU; // Flush cache (48-bit)
constexpr u8 ATA_CMD_FLUSH            = 0xE7U; // Flush cache (28-bit)
constexpr u8 ATA_CMD_IDENTIFY         = 0xECU; // Identify ATA Device
constexpr u8 ATA_CMD_IDENTIFY_PACKET  = 0xA1U; // Identify ATAPI Packet Device
constexpr u8 ATA_CMD_PACKET           = 0xA0U; // ATAPI Packet Command

// =============================================================================
// FIS (Frame Information Structure) Types & Layouts
// =============================================================================
constexpr u8 FIS_TYPE_REG_H2D   = 0x27U; // Register FIS — Host to Device
constexpr u8 FIS_TYPE_REG_D2H   = 0x34U; // Register FIS — Device to Host
constexpr u8 FIS_TYPE_DMA_ACT   = 0x39U; // DMA Activate FIS — Device to Host
constexpr u8 FIS_TYPE_DMA_SETUP = 0x41U; // DMA Setup FIS — Bidirectional
constexpr u8 FIS_TYPE_DATA      = 0x46U; // Data FIS — Bidirectional
constexpr u8 FIS_TYPE_BIST      = 0x58U; // BIST Activate FIS — Bidirectional
constexpr u8 FIS_TYPE_PIO_SETUP = 0x5FU; // PIO Setup FIS — Device to Host
constexpr u8 FIS_TYPE_DEV_BITS  = 0xA1U; // Set Device Bits FIS — Device to Host

/**
 * @brief Host to Device Register FIS (20 bytes).
 */
struct CORTEZ_PACKED FisRegH2D {
    u8  fisType;         // FIS_TYPE_REG_H2D (0x27)
    u8  pmPortAndC;      // bit 7: C (1=Command, 0=Control), bits 3:0: Port Multiplier port
    u8  command;         // ATA Command Register
    u8  featureLow;      // Feature Register (7:0)
    u8  lba0;            // LBA Low (7:0)
    u8  lba1;            // LBA Mid (15:8)
    u8  lba2;            // LBA High (23:16)
    u8  device;          // Device Register (bit 6 = 1 for LBA mode)
    u8  lba3;            // LBA (31:24)
    u8  lba4;            // LBA (39:32)
    u8  lba5;            // LBA (47:40)
    u8  featureHigh;     // Feature Register (15:8)
    u8  countLow;        // Sector Count (7:0)
    u8  countHigh;       // Sector Count (15:8)
    u8  icc;             // Isochronous Command Completion
    u8  control;         // Control Register
    u8  reserved[4];
};
static_assert(sizeof(FisRegH2D) == 20, "FisRegH2D must be 20 bytes");

/**
 * @brief Device to Host Register FIS (20 bytes).
 */
struct CORTEZ_PACKED FisRegD2H {
    u8  fisType;         // FIS_TYPE_REG_D2H (0x34)
    u8  pmPortAndI;      // bit 6: Interrupt bit, bits 3:0: PM Port
    u8  status;          // Status Register
    u8  error;           // Error Register
    u8  lba0;
    u8  lba1;
    u8  lba2;
    u8  device;
    u8  lba3;
    u8  lba4;
    u8  lba5;
    u8  reserved1;
    u8  countLow;
    u8  countHigh;
    u8  reserved2[6];
};
static_assert(sizeof(FisRegD2H) == 20, "FisRegD2H must be 20 bytes");

// =============================================================================
// AHCI DMA Memory Structures
// =============================================================================

/**
 * @brief Physical Region Descriptor Table (PRDT) Entry (16 bytes).
 */
struct CORTEZ_PACKED AhciPrdtEntry {
    u32 dba;             // Data Base Address (Low 32-bit, word aligned)
    u32 dbau;            // Data Base Address (Upper 32-bit)
    u32 reserved;
    u32 dbcAndInterrupt; // Data Byte Count (bits 21:0, 0-based byteCount - 1), bit 31 = Interrupt on completion
};
static_assert(sizeof(AhciPrdtEntry) == 16, "AhciPrdtEntry must be 16 bytes");

/**
 * @brief Command Header in Command List (32 bytes per slot; 32 slots = 1024 bytes).
 */
struct CORTEZ_PACKED AhciCommandHeader {
    u16 flagsAndCfl;     // CFL (bits 4:0), A (bit 5), W (bit 6), P (bit 7), R (bit 8), B (bit 9), C (bit 10), PMP (bits 15:12)
    u16 prdtl;           // Physical Region Descriptor Table Length (number of PRDT entries)
    u32 prdbc;           // PRD Byte Count transferred
    u32 ctba;            // Command Table Base Address Low (128-byte aligned)
    u32 ctbau;           // Command Table Base Address Upper 32-bit
    u32 reserved[4];
};
static_assert(sizeof(AhciCommandHeader) == 32, "AhciCommandHeader must be 32 bytes");

/**
 * @brief Command Table containing CFIS, ATAPI command buffer, and PRDT array.
 */
struct CORTEZ_PACKED AhciCommandTable {
    u8            cfis[64];    // Command FIS (typically FisRegH2D)
    u8            acmd[16];    // ATAPI Command (12 or 16 bytes)
    u8            reserved[48];
    AhciPrdtEntry prdt[16];    // 16 PRDT entries (covers up to 64 MiB transfers with 4 MiB chunks)
};
static_assert(sizeof(AhciCommandTable) == 384, "AhciCommandTable layout check");

/**
 * @brief Received FIS Structure (256 bytes per port, 256-byte aligned).
 */
struct CORTEZ_PACKED AhciReceivedFis {
    u8 dsfis[28];        // DMA Setup FIS
    u8 reserved1[4];
    u8 psfis[20];        // PIO Setup FIS
    u8 reserved2[12];
    u8 rfis[20];         // D2H Register FIS
    u8 reserved3[4];
    u8 sdbfis[8];        // Set Device Bits FIS
    u8 ufis[64];         // Unknown FIS
    u8 reserved4[96];
};
static_assert(sizeof(AhciReceivedFis) == 256, "AhciReceivedFis must be 256 bytes");

} // namespace cortez::drivers::storage
