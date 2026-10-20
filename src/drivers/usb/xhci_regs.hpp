/**
 * @file xhci_regs.hpp
 * @brief Intel xHCI 1.2 Host Controller MMIO registers, TRBs, Contexts, USB 2.0/3.2 descriptors,
 *        and USB Mass Storage Bulk-Only Transport (BOT) / SCSI structures.
 */

#pragma once

#include "../../lib/types.hpp"

namespace cortez::drivers::usb {

// =============================================================================
// xHCI Capability Register Offsets (from BAR0 base)
// =============================================================================
constexpr u32 XHCI_CAP_CAPLENGTH  = 0x00; // 8-bit Capability Register Length
constexpr u32 XHCI_CAP_HCIVERSION = 0x02; // 16-bit Interface Version Number
constexpr u32 XHCI_CAP_HCSPARAMS1 = 0x04; // 32-bit Structural Parameters 1 (MaxSlots, MaxIntrs, MaxPorts)
constexpr u32 XHCI_CAP_HCSPARAMS2 = 0x08; // 32-bit Structural Parameters 2 (Scratchpad Buffers)
constexpr u32 XHCI_CAP_HCSPARAMS3 = 0x0C; // 32-bit Structural Parameters 3
constexpr u32 XHCI_CAP_HCCPARAMS1 = 0x10; // 32-bit Capability Parameters 1 (AC64, CSZ, xECP)
constexpr u32 XHCI_CAP_DBOFF      = 0x14; // 32-bit Doorbell Offset
constexpr u32 XHCI_CAP_RTSOFF     = 0x18; // 32-bit Runtime Register Space Offset
constexpr u32 XHCI_CAP_HCCPARAMS2 = 0x1C; // 32-bit Capability Parameters 2

// =============================================================================
// xHCI Operational Register Offsets (from BAR0 + CAPLENGTH)
// =============================================================================
constexpr u32 XHCI_OP_USBCMD   = 0x00; // 32-bit USB Command
constexpr u32 XHCI_OP_USBSTS   = 0x04; // 32-bit USB Status
constexpr u32 XHCI_OP_PAGESIZE = 0x08; // 32-bit Page Size
constexpr u32 XHCI_OP_DNCTRL   = 0x14; // 32-bit Device Notification Control
constexpr u32 XHCI_OP_CRCR     = 0x18; // 64-bit Command Ring Control Register
constexpr u32 XHCI_OP_DCBAAP   = 0x30; // 64-bit Device Context Base Address Array Pointer
constexpr u32 XHCI_OP_CONFIG   = 0x38; // 32-bit Configure Register
constexpr u32 XHCI_OP_PORTS    = 0x400; // Port Register Set Base (0x400 + 0x10 * (port - 1))

// USBCMD Bits
constexpr u32 XHCI_CMD_RS     = (1U << 0); // Run/Stop
constexpr u32 XHCI_CMD_HCRST  = (1U << 1); // Host Controller Reset
constexpr u32 XHCI_CMD_INTE   = (1U << 2); // Interrupter Enable
constexpr u32 XHCI_CMD_HSEE   = (1U << 3); // Host System Error Enable

// USBSTS Bits
constexpr u32 XHCI_STS_HCH    = (1U << 0);  // HC Halted
constexpr u32 XHCI_STS_HSE    = (1U << 2);  // Host System Error
constexpr u32 XHCI_STS_EINT   = (1U << 3);  // Event Interrupt
constexpr u32 XHCI_STS_PCD    = (1U << 4);  // Port Change Detect
constexpr u32 XHCI_STS_CNR    = (1U << 11); // Controller Not Ready

// Port Status & Control (PORTSC) Bits
constexpr u32 XHCI_PORTSC_CCS = (1U << 0);  // Current Connect Status (RO)
constexpr u32 XHCI_PORTSC_PED = (1U << 1);  // Port Enabled/Disabled (RW1C)
constexpr u32 XHCI_PORTSC_OCA = (1U << 3);  // Over-current Active (RO)
constexpr u32 XHCI_PORTSC_PR  = (1U << 4);  // Port Reset (RW1S)
constexpr u32 XHCI_PORTSC_PP  = (1U << 9);  // Port Power (RWS)
constexpr u32 XHCI_PORTSC_CSC = (1U << 17); // Connect Status Change (RW1C)
constexpr u32 XHCI_PORTSC_PEC = (1U << 18); // Port Enabled/Disabled Change (RW1C)
constexpr u32 XHCI_PORTSC_WRC = (1U << 19); // Warm Port Reset Change (RW1C)
constexpr u32 XHCI_PORTSC_OCC = (1U << 20); // Over-current Change (RW1C)
constexpr u32 XHCI_PORTSC_PRC = (1U << 21); // Port Reset Change (RW1C)
constexpr u32 XHCI_PORTSC_PLC = (1U << 22); // Port Link State Change (RW1C)
constexpr u32 XHCI_PORTSC_CEC = (1U << 23); // Port Config Error Change (RW1C)
constexpr u32 XHCI_PORTSC_WPR = (1U << 31); // Warm Port Reset (RW1S)

// Mask of all RW1C bits in PORTSC (must be masked off during read-modify-write so PED/changes aren't cleared)
constexpr u32 XHCI_PORTSC_RW1C_MASK = XHCI_PORTSC_PED | XHCI_PORTSC_CSC | XHCI_PORTSC_PEC |
                                      XHCI_PORTSC_WRC | XHCI_PORTSC_OCC | XHCI_PORTSC_PRC |
                                      XHCI_PORTSC_PLC | XHCI_PORTSC_CEC;

constexpr u32 XHCI_PORTSC_CHANGE_BITS = XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC |
                                        XHCI_PORTSC_OCC | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC |
                                        XHCI_PORTSC_CEC;

constexpr u32 XHCI_PORTSC_PLS_MASK    = (0x0FU << 5); // Port Link State (bits 5..8, 0 = U0)

// Port Speed IDs (PORTSC bits 10..13)
constexpr u8 XHCI_SPEED_FULL       = 1; // 12 Mbps (USB 1.1)
constexpr u8 XHCI_SPEED_LOW        = 2; // 1.5 Mbps (USB 1.0)
constexpr u8 XHCI_SPEED_HIGH       = 3; // 480 Mbps (USB 2.0)
constexpr u8 XHCI_SPEED_SUPER      = 4; // 5 Gbps (USB 3.0 Gen 1)
constexpr u8 XHCI_SPEED_SUPER_PLUS = 5; // 10 Gbps (USB 3.1 Gen 2)

// =============================================================================
// xHCI Runtime Interrupter 0 Register Offsets (from BAR0 + RTSOFF + 0x20)
// =============================================================================
constexpr u32 XHCI_IR_BASE   = 0x20;
constexpr u32 XHCI_IR_IMAN   = 0x00; // Interrupter Management (Bit 0 = IP, Bit 1 = IE)
constexpr u32 XHCI_IR_IMOD   = 0x04; // Interrupter Moderation
constexpr u32 XHCI_IR_ERSTSZ = 0x08; // Event Ring Segment Table Size
constexpr u32 XHCI_IR_ERSTBA = 0x10; // Event Ring Segment Table Base Address (64-bit)
constexpr u32 XHCI_IR_ERDP   = 0x18; // Event Ring Dequeue Pointer (64-bit, Bit 3 = EHB)

// Extended Capability IDs
constexpr u8 XHCI_EXT_CAP_LEGACY = 1; // USB Legacy Support (USBLEGSUP)
constexpr u8 XHCI_EXT_CAP_PROTO  = 2; // Supported Protocol Capability

/**
 * @brief Represents a physical root hub protocol port range from an xHCI Supported Protocol capability (§7.2).
 */
struct XhciProtocolRange {
    u8 majorRev   = 0; // 2 for USB 2.0, 3 for USB 3.x
    u8 minorRev   = 0; // e.g., 0x00, 0x10, 0x20
    u8 portOffset = 0; // 1-based index of first compatible port
    u8 portCount  = 0; // Number of consecutive compatible ports
};

// Port Link State values (bits 5..8 of PORTSC)
constexpr u32 XHCI_PLS_U0         = 0;
constexpr u32 XHCI_PLS_U1         = 1;
constexpr u32 XHCI_PLS_U2         = 2;
constexpr u32 XHCI_PLS_U3         = 3; // Suspend
constexpr u32 XHCI_PLS_DISABLED   = 4;
constexpr u32 XHCI_PLS_RXDETECT   = 5;
constexpr u32 XHCI_PLS_INACTIVE   = 6;
constexpr u32 XHCI_PLS_POLLING    = 7;
constexpr u32 XHCI_PLS_RECOVERY   = 8;
constexpr u32 XHCI_PLS_HOT_RESET  = 9;
constexpr u32 XHCI_PLS_COMPLIANCE = 10;
constexpr u32 XHCI_PLS_TEST_MODE  = 11;
constexpr u32 XHCI_PLS_RESUME     = 15;

// =============================================================================
// xHCI Transfer Request Block (TRB) Types & Completion Codes
// =============================================================================
constexpr u8 XHCI_TRB_NORMAL          = 1;
constexpr u8 XHCI_TRB_SETUP_STAGE     = 2;
constexpr u8 XHCI_TRB_DATA_STAGE      = 3;
constexpr u8 XHCI_TRB_STATUS_STAGE    = 4;
constexpr u8 XHCI_TRB_LINK            = 6;
constexpr u8 XHCI_TRB_EVENT_DATA      = 7;
constexpr u8 XHCI_TRB_NOOP_TRANSFER   = 8;

constexpr u8 XHCI_TRB_ENABLE_SLOT     = 9;
constexpr u8 XHCI_TRB_DISABLE_SLOT    = 10;
constexpr u8 XHCI_TRB_ADDRESS_DEVICE  = 11;
constexpr u8 XHCI_TRB_CONFIGURE_EP    = 12;
constexpr u8 XHCI_TRB_EVALUATE_CTX    = 13;
constexpr u8 XHCI_TRB_RESET_EP        = 14;
constexpr u8 XHCI_TRB_STOP_EP         = 15;
constexpr u8 XHCI_TRB_SET_TR_DEQ      = 16;
constexpr u8 XHCI_TRB_NOOP_CMD        = 23;

constexpr u8 XHCI_TRB_TRANSFER_EVENT  = 32;
constexpr u8 XHCI_TRB_CMD_COMPLETION  = 33;
constexpr u8 XHCI_TRB_PORT_STATUS_CHG = 34;

// TRB Completion Codes
constexpr u8 XHCI_CC_SUCCESS          = 1;
constexpr u8 XHCI_CC_DATA_BUF_ERROR   = 2;
constexpr u8 XHCI_CC_BABBLE_DETECTED  = 3;
constexpr u8 XHCI_CC_USB_TX_ERROR     = 4;
constexpr u8 XHCI_CC_TRB_ERROR        = 5;
constexpr u8 XHCI_CC_STALL_ERROR      = 6;
constexpr u8 XHCI_CC_SHORT_PACKET     = 13;

// TRB Control Flags
constexpr u32 XHCI_TRB_CYCLE          = (1U << 0);
constexpr u32 XHCI_TRB_TC             = (1U << 1);  // Toggle Cycle (Link TRB)
constexpr u32 XHCI_TRB_ISP            = (1U << 2);  // Interrupt on Short Packet
constexpr u32 XHCI_TRB_CH             = (1U << 4);  // Chain bit
constexpr u32 XHCI_TRB_IOC            = (1U << 5);  // Interrupt On Completion
constexpr u32 XHCI_TRB_IDT            = (1U << 6);  // Immediate Data (Setup Stage TRB)
constexpr u32 XHCI_TRB_BSR            = (1U << 9);  // Block Set Address Request
constexpr u32 XHCI_TRB_DIR_IN         = (1U << 16); // Data/Status Stage Direction = IN

// Endpoint Types (Endpoint Context dw1 bits 3..5)
constexpr u32 XHCI_EP_TYPE_ISOCH_OUT  = 1;
constexpr u32 XHCI_EP_TYPE_BULK_OUT   = 2;
constexpr u32 XHCI_EP_TYPE_INTR_OUT   = 3;
constexpr u32 XHCI_EP_TYPE_CONTROL    = 4;
constexpr u32 XHCI_EP_TYPE_ISOCH_IN   = 5;
constexpr u32 XHCI_EP_TYPE_BULK_IN    = 6;
constexpr u32 XHCI_EP_TYPE_INTR_IN    = 7;

/**
 * @brief 16-byte generic xHCI Transfer Request Block (TRB).
 */
struct CORTEZ_PACKED XhciTrb {
    u64 param;
    u32 status;
    u32 control;

    constexpr u8 getType() const noexcept {
        return static_cast<u8>((control >> 10) & 0x3FU);
    }

    constexpr u8 getCompletionCode() const noexcept {
        return static_cast<u8>((status >> 24) & 0xFFU);
    }

    constexpr u8 getSlotId() const noexcept {
        return static_cast<u8>((control >> 24) & 0xFFU);
    }

    constexpr u8 getEndpointId() const noexcept {
        return static_cast<u8>((control >> 16) & 0x1FU);
    }
};
static_assert(sizeof(XhciTrb) == 16, "XhciTrb must be 16 bytes");

/**
 * @brief 16-byte Event Ring Segment Table (ERST) entry.
 */
struct CORTEZ_PACKED XhciErstEntry {
    u64 ringSegmentBaseAddr;
    u16 ringSegmentSize;
    u16 reserved0;
    u32 reserved1;
};
static_assert(sizeof(XhciErstEntry) == 16, "XhciErstEntry must be 16 bytes");

/**
 * @brief 32-byte Input Control Context (padded to 64 bytes when HCCPARAMS1.CSZ == 1).
 */
struct CORTEZ_PACKED XhciInputControlContext {
    u32 dropFlags;
    u32 addFlags;
    u32 reserved[5];
    u8  configValue;
    u8  interfaceNum;
    u8  alternateSetting;
    u8  reserved2;
};
static_assert(sizeof(XhciInputControlContext) == 32, "XhciInputControlContext must be 32 bytes");

/**
 * @brief 32-byte Slot Context (padded to 64 bytes when HCCPARAMS1.CSZ == 1).
 */
struct CORTEZ_PACKED XhciSlotContext {
    u32 dw0; // Bits 0..19 RouteString, Bits 20..23 Speed, Bit 26 Hub, Bits 27..31 ContextEntries
    u32 dw1; // Bits 0..15 MaxExitLatency, Bits 16..23 RootHubPortNumber, Bits 24..31 NumberOfPorts
    u32 dw2; // InterrupterTarget, TT fields
    u32 dw3; // Bits 0..7 USB Device Address, Bits 27..31 Slot State
    u32 reserved[4];
};
static_assert(sizeof(XhciSlotContext) == 32, "XhciSlotContext must be 32 bytes");

/**
 * @brief 32-byte Endpoint Context (padded to 64 bytes when HCCPARAMS1.CSZ == 1).
 */
struct CORTEZ_PACKED XhciEndpointContext {
    u32 dw0;        // EP State, Mult, MaxPStreams, LSA, Interval, MaxESITPayloadHi
    u32 dw1;        // Bits 1..2 CErr, Bits 3..5 EPType, Bit 7 HID, Bits 8..15 MaxBurstSize, Bits 16..31 MaxPacketSize
    u64 trDequeue;  // Bits 4..63 TR Dequeue Pointer, Bit 0 DCS (Dequeue Cycle State)
    u32 dw4;        // Bits 0..15 Average TRB Length, Bits 16..31 MaxESITPayloadLo
    u32 reserved[3];
};
static_assert(sizeof(XhciEndpointContext) == 32, "XhciEndpointContext must be 32 bytes");

// =============================================================================
// Standard USB 2.0 / 3.2 Setup Packet & Descriptors
// =============================================================================
constexpr u8 USB_REQ_GET_STATUS        = 0x00;
constexpr u8 USB_REQ_CLEAR_FEATURE     = 0x01;
constexpr u8 USB_REQ_SET_ADDRESS       = 0x05;
constexpr u8 USB_REQ_GET_DESCRIPTOR    = 0x06;
constexpr u8 USB_REQ_SET_CONFIGURATION = 0x09;
constexpr u8 USB_REQ_SET_INTERFACE     = 0x0B;

// USB Mass Storage Class-Specific Requests (BOT §3)
constexpr u8 USB_REQ_MSC_GET_MAX_LUN   = 0xFE;
constexpr u8 USB_REQ_MSC_RESET         = 0xFF; // Bulk-Only Mass Storage Reset

constexpr u8 USB_DESC_DEVICE           = 0x01;
constexpr u8 USB_DESC_CONFIGURATION    = 0x02;
constexpr u8 USB_DESC_STRING           = 0x03;
constexpr u8 USB_DESC_INTERFACE        = 0x04;
constexpr u8 USB_DESC_ENDPOINT         = 0x05;
constexpr u8 USB_DESC_SS_EP_COMPANION  = 0x30;

constexpr u8 USB_CLASS_MASS_STORAGE    = 0x08;
constexpr u8 USB_MSC_SUBCLASS_SCSI     = 0x06;
constexpr u8 USB_MSC_SUBCLASS_UFI      = 0x04;
constexpr u8 USB_MSC_SUBCLASS_RBC      = 0x01;
constexpr u8 USB_MSC_PROTO_BOT         = 0x50; // Bulk-Only Transport

struct CORTEZ_PACKED UsbSetupPacket {
    u8  bmRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
};
static_assert(sizeof(UsbSetupPacket) == 8, "UsbSetupPacket must be 8 bytes");

struct CORTEZ_PACKED UsbDeviceDescriptor {
    u8  bLength;
    u8  bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass;
    u8  bDeviceSubClass;
    u8  bDeviceProtocol;
    u8  bMaxPacketSize0;
    u16 idVendor;
    u16 idProduct;
    u16 bcdDevice;
    u8  iManufacturer;
    u8  iProduct;
    u8  iSerialNumber;
    u8  bNumConfigurations;
};
static_assert(sizeof(UsbDeviceDescriptor) == 18, "UsbDeviceDescriptor must be 18 bytes");

struct CORTEZ_PACKED UsbConfigDescriptor {
    u8  bLength;
    u8  bDescriptorType;
    u16 wTotalLength;
    u8  bNumInterfaces;
    u8  bConfigurationValue;
    u8  iConfiguration;
    u8  bmAttributes;
    u8  bMaxPower;
};
static_assert(sizeof(UsbConfigDescriptor) == 9, "UsbConfigDescriptor must be 9 bytes");

struct CORTEZ_PACKED UsbInterfaceDescriptor {
    u8 bLength;
    u8 bDescriptorType;
    u8 bInterfaceNumber;
    u8 bAlternateSetting;
    u8 bNumEndpoints;
    u8 bInterfaceClass;
    u8 bInterfaceSubClass;
    u8 bInterfaceProtocol;
    u8 iInterface;
};
static_assert(sizeof(UsbInterfaceDescriptor) == 9, "UsbInterfaceDescriptor must be 9 bytes");

struct CORTEZ_PACKED UsbEndpointDescriptor {
    u8  bLength;
    u8  bDescriptorType;
    u8  bEndpointAddress; // Bit 7: 1=IN, 0=OUT; Bits 0..3: Endpoint Number
    u8  bmAttributes;     // Bits 0..1: 0=Control, 1=Isoch, 2=Bulk, 3=Interrupt
    u16 wMaxPacketSize;
    u8  bInterval;
};
static_assert(sizeof(UsbEndpointDescriptor) == 7, "UsbEndpointDescriptor must be 7 bytes");

struct CORTEZ_PACKED UsbSsEpCompanionDescriptor {
    u8  bLength;
    u8  bDescriptorType; // 0x30
    u8  bMaxBurst;
    u8  bmAttributes;
    u16 wBytesPerInterval;
};
static_assert(sizeof(UsbSsEpCompanionDescriptor) == 6, "UsbSsEpCompanionDescriptor must be 6 bytes");

// =============================================================================
// USB Mass Storage Class — Bulk-Only Transport (BOT) & SCSI Definitions
// =============================================================================
constexpr u32 USB_MSC_CBW_SIGNATURE = 0x43425355U; // "USBC"
constexpr u32 USB_MSC_CSW_SIGNATURE = 0x53425355U; // "USBS"

constexpr u8  USB_MSC_DIR_OUT       = 0x00;
constexpr u8  USB_MSC_DIR_IN        = 0x80;

// SCSI Command Opcodes
constexpr u8  SCSI_CMD_TEST_UNIT_READY = 0x00;
constexpr u8  SCSI_CMD_REQUEST_SENSE   = 0x03;
constexpr u8  SCSI_CMD_INQUIRY         = 0x12;
constexpr u8  SCSI_CMD_READ_CAPACITY10 = 0x25;
constexpr u8  SCSI_CMD_READ10          = 0x28;
constexpr u8  SCSI_CMD_WRITE10         = 0x2A;
constexpr u8  SCSI_CMD_SYNC_CACHE10    = 0x35;
constexpr u8  SCSI_CMD_READ16          = 0x88;
constexpr u8  SCSI_CMD_WRITE16         = 0x8A;
constexpr u8  SCSI_CMD_SERVICE_ACT_IN  = 0x9E; // Subcode 0x10 = READ CAPACITY (16)

// Standard SCSI Sense Keys (Fixed Sense byte 2 bits 0..3)
constexpr u8  SCSI_SENSE_NO_SENSE         = 0x00;
constexpr u8  SCSI_SENSE_RECOVERED_ERROR  = 0x01;
constexpr u8  SCSI_SENSE_NOT_READY        = 0x02;
constexpr u8  SCSI_SENSE_MEDIUM_ERROR     = 0x03;
constexpr u8  SCSI_SENSE_HARDWARE_ERROR   = 0x04;
constexpr u8  SCSI_SENSE_ILLEGAL_REQUEST  = 0x05;
constexpr u8  SCSI_SENSE_UNIT_ATTENTION   = 0x06;

// Common SCSI Additional Sense Codes (ASC)
constexpr u8  SCSI_ASC_NOT_READY_TO_READY = 0x28; // Not Ready to Ready transition
constexpr u8  SCSI_ASC_MEDIUM_NOT_PRESENT = 0x3A; // Medium Not Present

/**
 * @brief 31-byte USB Mass Storage Command Block Wrapper (CBW).
 */
struct CORTEZ_PACKED UsbMscCbw {
    u32 signature;          // 0x43425355 ("USBC")
    u32 tag;                // Host-assigned transaction tag
    u32 dataTransferLength; // Number of bytes to transfer in data phase
    u8  flags;              // Bit 7: 0 = Data-Out, 1 = Data-In
    u8  lun;                // Bits 0..3: Logical Unit Number
    u8  cbLength;           // Length of CDB in bytes (1..16)
    u8  cb[16];             // SCSI Command Descriptor Block (CDB)
};
static_assert(sizeof(UsbMscCbw) == 31, "UsbMscCbw must be exactly 31 bytes");

/**
 * @brief 13-byte USB Mass Storage Command Status Wrapper (CSW).
 */
struct CORTEZ_PACKED UsbMscCsw {
    u32 signature;   // 0x53425355 ("USBS")
    u32 tag;         // Echoed dCBWTag
    u32 dataResidue; // Difference between expected and actual bytes transferred
    u8  status;      // 0x00 = Command Passed, 0x01 = Command Failed, 0x02 = Phase Error
};
static_assert(sizeof(UsbMscCsw) == 13, "UsbMscCsw must be exactly 13 bytes");

/**
 * @brief 36-byte Standard SCSI INQUIRY response data.
 */
struct CORTEZ_PACKED ScsiInquiryData {
    u8   peripheral;
    u8   removable;
    u8   version;
    u8   responseDataFormat;
    u8   additionalLength;
    u8   flags[3];
    char vendorId[8];
    char productId[16];
    char productRev[4];
};
static_assert(sizeof(ScsiInquiryData) == 36, "ScsiInquiryData must be 36 bytes");

// Big-endian <-> Little-endian conversion helpers for SCSI CDBs and responses
constexpr u16 bswap16(u16 val) noexcept {
    return static_cast<u16>((val >> 8) | (val << 8));
}

constexpr u32 bswap32(u32 val) noexcept {
    return ((val >> 24) & 0x000000FFU) |
           ((val >> 8)  & 0x0000FF00U) |
           ((val << 8)  & 0x00FF0000U) |
           ((val << 24) & 0xFF000000U);
}

constexpr u64 bswap64(u64 val) noexcept {
    return (static_cast<u64>(bswap32(static_cast<u32>(val & 0xFFFFFFFFULL))) << 32) |
           static_cast<u64>(bswap32(static_cast<u32>(val >> 32)));
}

} // namespace cortez::drivers::usb
