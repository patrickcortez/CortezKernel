/**
 * @file xhci_controller.cpp
 * @brief Implementation of the PCIe xHCI Host Controller, BIOS Handoff, USB Port Enumeration,
 *        and Multi-Controller XhciManager.
 */

#include "xhci_controller.hpp"
#include "../storage/gpt_partition.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::usb {

// =============================================================================
// XhciController MMIO Helpers
// =============================================================================

u32 XhciController::readCap32(u32 offset) const noexcept {
    return *reinterpret_cast<volatile const u32*>(m_capBaseVirt + offset);
}

u32 XhciController::readOp32(u32 offset) const noexcept {
    return *reinterpret_cast<volatile const u32*>(m_opBaseVirt + offset);
}

void XhciController::writeOp32(u32 offset, u32 value) noexcept {
    *reinterpret_cast<volatile u32*>(m_opBaseVirt + offset) = value;
}

u64 XhciController::readOp64(u32 offset) const noexcept {
    u32 low  = readOp32(offset);
    u32 high = readOp32(offset + 4);
    return (static_cast<u64>(high) << 32) | low;
}

void XhciController::writeOp64(u32 offset, u64 value) noexcept {
    writeOp32(offset, static_cast<u32>(value & 0xFFFFFFFFULL));
    writeOp32(offset + 4, static_cast<u32>(value >> 32));
}

u32 XhciController::readPortsc(u8 portNum) const noexcept {
    u32 offset = XHCI_OP_PORTS + static_cast<u32>(portNum - 1U) * 0x10U;
    return readOp32(offset);
}

void XhciController::writePortsc(u8 portNum, u32 value) noexcept {
    u32 offset = XHCI_OP_PORTS + static_cast<u32>(portNum - 1U) * 0x10U;
    writeOp32(offset, value);
}

void XhciController::ringDoorbell(u8 slotId, u32 target) noexcept {
    arch::memoryBarrier();
    m_dbBaseVirt[slotId] = target;
}

u8* XhciController::getInputControlCtx(XhciSlotState& slot) const noexcept {
    return slot.inputCtxVirt;
}

u8* XhciController::getInputSlotCtx(XhciSlotState& slot) const noexcept {
    return slot.inputCtxVirt + m_contextSize;
}

u8* XhciController::getInputEpCtx(XhciSlotState& slot, u8 dci) const noexcept {
    return slot.inputCtxVirt + static_cast<usize>(dci + 1U) * m_contextSize;
}

u8* XhciController::getOutputSlotCtx(XhciSlotState& slot) const noexcept {
    return slot.outputCtxVirt;
}

u8* XhciController::getOutputEpCtx(XhciSlotState& slot, u8 dci) const noexcept {
    return slot.outputCtxVirt + static_cast<usize>(dci) * m_contextSize;
}

bool XhciController::waitOpBit(u32 regOffset, u32 mask, u32 expected, u32 timeoutMs) const noexcept {
    for (u32 elapsed = 0; elapsed < timeoutMs; ++elapsed) {
        if ((readOp32(regOffset) & mask) == expected) {
            return true;
        }
        arch::ApicController::instance().sleepMs(1);
    }
    return ((readOp32(regOffset) & mask) == expected);
}

// =============================================================================
// BIOS / UEFI Handoff & Chipset Quirks
// =============================================================================

void XhciController::performBiosHandoff() noexcept {
    u32 hcc1 = readCap32(XHCI_CAP_HCCPARAMS1);
    u32 xecp = (hcc1 >> 16) & 0xFFFFU;
    if (xecp == 0) return;

    u32 offset = xecp << 2;
    for (u32 guard = 0; offset != 0 && offset + 8 <= 131072U && guard < 64; ++guard) {
        u32 capHdr  = readCap32(offset);
        u8  capId   = static_cast<u8>(capHdr & 0xFFU);
        u8  nextPtr = static_cast<u8>((capHdr >> 8) & 0xFFU);

        if (capId == XHCI_EXT_CAP_LEGACY) {
            // Request OS Ownership (Bit 24 = HC OS Owned Semaphore)
            *reinterpret_cast<volatile u32*>(m_capBaseVirt + offset) = capHdr | (1U << 24);

            // Wait up to 1000 ms for BIOS to release ownership (Bit 16 = HC BIOS Owned Semaphore)
            for (u32 ms = 0; ms < 1000; ++ms) {
                if ((readCap32(offset) & (1U << 16)) == 0) {
                    break;
                }
                arch::ApicController::instance().sleepMs(1);
            }

            // Force-clear BIOS ownership if firmware hung and disable legacy SMIs in USBLEGCTLSTS
            u32 legSup = readCap32(offset);
            if (legSup & (1U << 16)) {
                *reinterpret_cast<volatile u32*>(m_capBaseVirt + offset) =
                    (legSup & ~(1U << 16)) | (1U << 24);
            }

            u32 legCtlSts = readCap32(offset + 4);
            // Disable SMI bits (0, 4, 13, 14, 15) and clear RW1C status bits (16, 20, 29, 30, 31)
            legCtlSts &= ~((1U << 0) | (1U << 4) | (1U << 13) | (1U << 14) | (1U << 15));
            legCtlSts |=  ((1U << 16) | (1U << 20) | (1U << 29) | (1U << 30) | (1U << 31));
            *reinterpret_cast<volatile u32*>(m_capBaseVirt + offset + 4) = legCtlSts;

            pr_info("xhci", "Completed BIOS/UEFI-to-OS xHCI handoff (USBLEGSUP @ +0x%x)", offset);
            break;
        }

        if (nextPtr == 0) break;
        offset += static_cast<u32>(nextPtr) << 2;
    }
}

void XhciController::enableIntelPortRouting() noexcept {
    // On Intel chipsets with a companion EHCI controller (0C:03:20), route USB 2.0/3.0 ports to xHCI
    if (m_pciDev.vendorId == 0x8086U &&
        arch::PcieManager::instance().findByClass(0x0C, 0x03, 0x20) != nullptr) {
        auto& pcie = arch::PcieManager::instance();
        u32 usb3Mask = pcie.readConfig32(m_pciDev.bus, m_pciDev.device, m_pciDev.function, 0xDC);
        pcie.writeConfig32(m_pciDev.bus, m_pciDev.device, m_pciDev.function, 0xD8, usb3Mask);
        u32 usb2Mask = pcie.readConfig32(m_pciDev.bus, m_pciDev.device, m_pciDev.function, 0xD4);
        pcie.writeConfig32(m_pciDev.bus, m_pciDev.device, m_pciDev.function, 0xD0, usb2Mask);
    }
}

// =============================================================================
// Controller Setup, DCBAA & Scratchpad Initialization
// =============================================================================

Status XhciController::setupDcbaaAndScratchpad() noexcept {
    auto dcbaaPage = m_pmm->allocPages(1);
    if (dcbaaPage.isError()) return dcbaaPage.status();

    m_dcbaaPhys = dcbaaPage.value();
    m_dcbaaVirt = reinterpret_cast<u64*>(m_dcbaaPhys + m_hhdmOffset);
    memset(m_dcbaaVirt, 0, PAGE_SIZE_4K);

    // Check HCSPARAMS2 for required Scratchpad Buffers (Hi = bits 21..25, Lo = bits 27..31)
    u32 hcs2 = readCap32(XHCI_CAP_HCSPARAMS2);
    u32 spHi = (hcs2 >> 21) & 0x1FU;
    u32 spLo = (hcs2 >> 27) & 0x1FU;
    u32 numScratchpad = (spHi << 5) | spLo;

    if (numScratchpad > 0) {
        if (numScratchpad > 256) numScratchpad = 256;

        auto spArrPage = m_pmm->allocPages(1);
        if (spArrPage.isError()) return spArrPage.status();

        phys_addr_t spArrPhys = spArrPage.value();
        auto*       spArrVirt = reinterpret_cast<u64*>(spArrPhys + m_hhdmOffset);
        memset(spArrVirt, 0, PAGE_SIZE_4K);

        for (u32 i = 0; i < numScratchpad; ++i) {
            auto bufPage = m_pmm->allocPages(1);
            if (bufPage.isError()) return bufPage.status();
            phys_addr_t bufPhys = bufPage.value();
            memset(reinterpret_cast<void*>(bufPhys + m_hhdmOffset), 0, PAGE_SIZE_4K);
            spArrVirt[i] = bufPhys;
        }

        m_dcbaaVirt[0] = spArrPhys;
    }

    arch::memoryBarrier();
    writeOp64(XHCI_OP_DCBAAP, m_dcbaaPhys);
    return Status::Ok;
}

Status XhciController::init(const arch::PciDeviceInfo& pciDev,
                            memory::IPhysicalAllocator* pmm,
                            memory::IVirtualMapper* vmm,
                            u64 hhdmOffset) noexcept {
    if (!pmm || !vmm || pciDev.bar0Phys == 0) {
        return Status::InvalidArgument;
    }

    m_pciDev     = pciDev;
    m_pmm        = pmm;
    m_vmm        = vmm;
    m_hhdmOffset = hhdmOffset;

    pr_info("xhci", "Found PCIe xHCI controller at %02x:%02x.%u [VID:DID %04x:%04x] BAR0=0x%llx",
            pciDev.bus, pciDev.device, pciDev.function,
            pciDev.vendorId, pciDev.deviceId, pciDev.bar0Phys);

    arch::PcieManager::instance().enableBusMastering(pciDev);
    enableIntelPortRouting();

    constexpr usize INITIAL_MMIO_SIZE = 131072; // 128 KiB
    auto mapRes = vmm->mapMmio(pciDev.bar0Phys, INITIAL_MMIO_SIZE);
    if (mapRes.isError()) return mapRes.status();
    m_capBaseVirt = reinterpret_cast<volatile u8*>(mapRes.value());

    u32 cap0      = readCap32(XHCI_CAP_CAPLENGTH);
    m_capLength   = static_cast<u8>(cap0 & 0xFFU);
    m_hciVersion  = static_cast<u16>((cap0 >> 16) & 0xFFFFU);
    u32 hcs1      = readCap32(XHCI_CAP_HCSPARAMS1);
    u32 hcc1      = readCap32(XHCI_CAP_HCCPARAMS1);
    u32 dboff     = readCap32(XHCI_CAP_DBOFF) & ~0x03U;
    u32 rtsoff    = readCap32(XHCI_CAP_RTSOFF) & ~0x1FU;

    m_maxSlots    = static_cast<u8>(hcs1 & 0xFFU);
    m_maxPorts    = static_cast<u8>((hcs1 >> 24) & 0xFFU);
    m_contextSize = (hcc1 & (1U << 2)) ? 64U : 32U;

    m_opBaseVirt  = m_capBaseVirt + m_capLength;

    if (rtsoff + 0x1000U <= INITIAL_MMIO_SIZE) {
        m_rtBaseVirt = m_capBaseVirt + rtsoff;
    } else {
        auto rtRes = vmm->mapMmio(pciDev.bar0Phys + rtsoff, 0x2000U);
        if (rtRes.isError()) return rtRes.status();
        m_rtBaseVirt = reinterpret_cast<volatile u8*>(rtRes.value());
    }

    if (dboff + 0x1000U <= INITIAL_MMIO_SIZE) {
        m_dbBaseVirt = reinterpret_cast<volatile u32*>(m_capBaseVirt + dboff);
    } else {
        auto dbRes = vmm->mapMmio(pciDev.bar0Phys + dboff, 0x1000U);
        if (dbRes.isError()) return dbRes.status();
        m_dbBaseVirt = reinterpret_cast<volatile u32*>(dbRes.value());
    }

    // Allocate EP0 control transfer DMA bounce page
    auto ep0Page = m_pmm->allocPages(1);
    if (ep0Page.isError()) return ep0Page.status();
    m_ep0DmaPhys = ep0Page.value();
    m_ep0DmaVirt = reinterpret_cast<u8*>(m_ep0DmaPhys + m_hhdmOffset);

    // Step 1: Perform BIOS/UEFI Legacy Ownership Handoff
    performBiosHandoff();

    // Step 2: Halt controller before reset (USBCMD.RS = 0 -> wait for USBSTS.HCH = 1)
    u32 cmd = readOp32(XHCI_OP_USBCMD);
    if (cmd & XHCI_CMD_RS) {
        writeOp32(XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RS);
    }
    if (!waitOpBit(XHCI_OP_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, 500)) {
        pr_warn("xhci", "Controller did not report HCH=1 before reset; proceeding with HCRST");
    }

    // Step 3: Issue Host Controller Reset (USBCMD.HCRST = 1) and wait for HCRST=0 and CNR=0
    writeOp32(XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    if (!waitOpBit(XHCI_OP_USBCMD, XHCI_CMD_HCRST, 0, 1000) ||
        !waitOpBit(XHCI_OP_USBSTS, XHCI_STS_CNR, 0, 1000)) {
        pr_err("xhci", "Timeout waiting for xHCI reset completion");
        return Status::Timeout;
    }
    arch::ApicController::instance().sleepMs(10);

    // Step 4: Configure Max Device Slots Enabled (CONFIG) & Device Notification (DNCTRL)
    u8 maxUsableSlots = (m_maxSlots >= MAX_SLOTS_PER_CTRL)
                            ? static_cast<u8>(MAX_SLOTS_PER_CTRL - 1)
                            : m_maxSlots;
    writeOp32(XHCI_OP_CONFIG, maxUsableSlots);
    writeOp32(XHCI_OP_DNCTRL, 0x02U);

    // Step 5: Set up DCBAA & Scratchpad Buffers
    Status st = setupDcbaaAndScratchpad();
    if (st != Status::Ok) return st;

    // Step 6: Initialize Command Ring and program CRCR
    st = m_cmdRing.init(m_pmm, m_hhdmOffset);
    if (st != Status::Ok) return st;
    writeOp64(XHCI_OP_CRCR, m_cmdRing.getPhysBase() | XHCI_TRB_CYCLE);

    // Step 7: Initialize Interrupter 0 Event Ring
    st = m_eventRing.init(m_rtBaseVirt, m_pmm, m_hhdmOffset);
    if (st != Status::Ok) return st;

    // Step 8: Enable PCI MSI/MSI-X (optional, EventRing also supports synchronous polling)
    (void)arch::PcieManager::instance().enableMsiOrMsix(
        pciDev,
        arch::IdtManager::VEC_XHCI_MSI,
        arch::ApicController::instance().getApicId());

    // Clear pending status bits in USBSTS and start controller (RS | INTE | HSEE)
    writeOp32(XHCI_OP_USBSTS, XHCI_STS_EINT | XHCI_STS_PCD | XHCI_STS_HSE);
    writeOp32(XHCI_OP_USBCMD, XHCI_CMD_RS | XHCI_CMD_INTE | XHCI_CMD_HSEE);

    if (!waitOpBit(XHCI_OP_USBSTS, XHCI_STS_HCH, 0, 500)) {
        pr_err("xhci", "Timeout waiting for xHCI controller to enter Running state (HCH=0)");
        return Status::Timeout;
    }

    m_ready = true;
    pr_ok("xhci", "xHCI %x.%02x ready: %u ports, %u slots, %zuB contexts",
          (m_hciVersion >> 8) & 0xFFU, m_hciVersion & 0xFFU,
          m_maxPorts, m_maxSlots, m_contextSize);

    // Step 9: Power and probe all root hub ports
    probePorts();
    return Status::Ok;
}

// =============================================================================
// Command Submission, Control Transfers & Bulk Transfers
// =============================================================================

Status XhciController::submitCommand(const XhciTrb& cmdTrb,
                                     XhciTrb* outCompletion,
                                     u32 timeoutMs) noexcept {
    m_cmdRing.enqueue(cmdTrb);
    ringDoorbell(0, 0); // Doorbell 0, Target 0 = Host Controller Command Ring
    return m_eventRing.pollEvent(XHCI_TRB_CMD_COMPLETION, outCompletion, timeoutMs);
}

Status XhciController::controlTransfer(u8 slotId,
                                       const UsbSetupPacket& setup,
                                       void* dataBuffer,
                                       u16 length,
                                       bool isDataIn) noexcept {
    if (slotId == 0 || slotId >= MAX_SLOTS_PER_CTRL || !m_slots[slotId].active) {
        return Status::InvalidArgument;
    }
    if (length > PAGE_SIZE_4K) {
        return Status::InvalidArgument;
    }

    IrqLockGuard guard(m_ctrlLock);
    XhciSlotState& slot = m_slots[slotId];

    // 1. Setup Stage TRB (Immediate Data = 8-byte Setup Packet, TRT = 0 [No Data], 2 [OUT], 3 [IN])
    u32 trt = 0;
    if (length > 0) {
        trt = isDataIn ? 3U : 2U;
    }

    u64 setupParam = 0;
    memcpy(&setupParam, &setup, sizeof(UsbSetupPacket));

    XhciTrb setupTrb{};
    setupTrb.param   = setupParam;
    setupTrb.status  = 8U; // Always 8 bytes for Setup Packet
    setupTrb.control = (static_cast<u32>(XHCI_TRB_SETUP_STAGE) << 10) |
                       XHCI_TRB_IDT |
                       (trt << 16);
    slot.ep0Ring.enqueue(setupTrb);

    // 2. Optional Data Stage TRB
    if (length > 0) {
        if (!isDataIn && dataBuffer) {
            memcpy(m_ep0DmaVirt, dataBuffer, length);
        } else {
            memset(m_ep0DmaVirt, 0, length);
        }

        XhciTrb dataTrb{};
        dataTrb.param   = m_ep0DmaPhys;
        dataTrb.status  = length;
        dataTrb.control = (static_cast<u32>(XHCI_TRB_DATA_STAGE) << 10) |
                          (isDataIn ? XHCI_TRB_DIR_IN : 0U);
        slot.ep0Ring.enqueue(dataTrb);
    }

    // 3. Status Stage TRB (Direction is opposite of Data Stage, or IN if No Data Stage; IOC = 1)
    bool statusDirIn = (length == 0) || !isDataIn;
    XhciTrb statusTrb{};
    statusTrb.param   = 0;
    statusTrb.status  = 0;
    statusTrb.control = (static_cast<u32>(XHCI_TRB_STATUS_STAGE) << 10) |
                        XHCI_TRB_IOC |
                        (statusDirIn ? XHCI_TRB_DIR_IN : 0U);
    slot.ep0Ring.enqueue(statusTrb);

    // Ring Doorbell for Slot (Target = 1 for Control Endpoint 0)
    ringDoorbell(slotId, 1U);

    XhciTrb ev{};
    Status st = m_eventRing.pollEvent(XHCI_TRB_TRANSFER_EVENT, &ev, 3000);
    if (st != Status::Ok) {
        return st;
    }

    if (length > 0 && isDataIn && dataBuffer) {
        memcpy(dataBuffer, m_ep0DmaVirt, length);
    }
    return Status::Ok;
}

Status XhciController::clearEndpointStall(u8 slotId,
                                          u8 dci,
                                          u8 epAddr,
                                          XhciTrbRing& ring) noexcept {
    // 1. Issue xHCI Reset Endpoint Command TRB (TSP = 0)
    {
        XhciTrb resetCmd{};
        resetCmd.param   = 0;
        resetCmd.status  = 0;
        resetCmd.control = (static_cast<u32>(XHCI_TRB_RESET_EP) << 10) |
                           (static_cast<u32>(dci) << 16) |
                           (static_cast<u32>(slotId) << 24);
        XhciTrb comp{};
        (void)submitCommand(resetCmd, &comp, 1000);
    }

    // 2. Issue xHCI Set TR Dequeue Pointer Command TRB to advance past the stalled TRB
    {
        phys_addr_t newDeq = ring.getCurrentDequeuePhys() | ring.getCycleState();
        XhciTrb setDeqCmd{};
        setDeqCmd.param   = newDeq;
        setDeqCmd.status  = 0;
        setDeqCmd.control = (static_cast<u32>(XHCI_TRB_SET_TR_DEQ) << 10) |
                            (static_cast<u32>(dci) << 16) |
                            (static_cast<u32>(slotId) << 24);
        XhciTrb comp{};
        (void)submitCommand(setDeqCmd, &comp, 1000);
    }

    // 3. Issue standard USB CLEAR_FEATURE(ENDPOINT_HALT = 0) on EP0
    UsbSetupPacket clearHalt{};
    clearHalt.bmRequestType = 0x02U; // Host-to-Device | Standard | Endpoint
    clearHalt.bRequest      = USB_REQ_CLEAR_FEATURE;
    clearHalt.wValue        = 0;     // ENDPOINT_HALT
    clearHalt.wIndex        = epAddr;
    clearHalt.wLength       = 0;
    return controlTransfer(slotId, clearHalt, nullptr, 0, false);
}

Status XhciController::bulkTransfer(u8 slotId,
                                    u8 dci,
                                    u8 epAddr,
                                    XhciTrbRing& ring,
                                    phys_addr_t dmaPhys,
                                    u32 length,
                                    bool isDataIn) noexcept {
    if (slotId == 0 || slotId >= MAX_SLOTS_PER_CTRL || !m_slots[slotId].active) {
        return Status::InvalidArgument;
    }

    XhciTrb xferTrb{};
    xferTrb.param   = dmaPhys;
    xferTrb.status  = length; // TRB Transfer Length (bits 0..16), TD Size = 0, Interrupter = 0
    xferTrb.control = (static_cast<u32>(XHCI_TRB_NORMAL) << 10) |
                      XHCI_TRB_IOC |
                      (isDataIn ? XHCI_TRB_ISP : 0U);

    ring.enqueue(xferTrb);
    ringDoorbell(slotId, dci);

    XhciTrb ev{};
    Status st = m_eventRing.pollEvent(XHCI_TRB_TRANSFER_EVENT, &ev, 5000);
    if (st != Status::Ok) {
        if (ev.getCompletionCode() == XHCI_CC_STALL_ERROR) {
            (void)clearEndpointStall(slotId, dci, epAddr, ring);
        }
        return st;
    }

    // If Interrupt on Short Packet (ISP) generated a Transfer Event before an IOC TRB,
    // Normal TRB has both ISP and IOC on the same single TRB so only one event is generated.
    return Status::Ok;
}

// =============================================================================
// Port Reset & USB Device / Mass Storage Enumeration
// =============================================================================

void XhciController::probePorts() noexcept {
    // Ensure Port Power (PP) is asserted on all ports
    for (u8 port = 1; port <= m_maxPorts; ++port) {
        u32 portsc = readPortsc(port);
        if (!(portsc & XHCI_PORTSC_PP)) {
            writePortsc(port, XHCI_PORTSC_PP);
        }
    }

    // Wait 50 ms for port power & USB 3.x / 2.0 link detection to settle
    arch::ApicController::instance().sleepMs(50);
    m_eventRing.drainEvents();

    for (u8 port = 1; port <= m_maxPorts; ++port) {
        u32 portsc = readPortsc(port);
        if (!(portsc & XHCI_PORTSC_CCS)) {
            continue;
        }

        u8 speedId = 0;
        if (resetPort(port, &speedId)) {
            (void)enumeratePortDevice(port, speedId);
        }
    }
}

bool XhciController::resetPort(u8 portNum, u8* outSpeedId) noexcept {
    u32 portsc = readPortsc(portNum);
    if (!(portsc & XHCI_PORTSC_CCS)) return false;

    // Clear any latched status change bits while preserving Port Power
    writePortsc(portNum, (portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_CHANGE_BITS);

    // Assert Port Reset (PR = bit 4)
    portsc = readPortsc(portNum);
    writePortsc(portNum, (portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_PR);

    bool resetDone = false;
    for (u32 ms = 0; ms < 500; ++ms) {
        arch::ApicController::instance().sleepMs(1);
        portsc = readPortsc(portNum);
        if ((portsc & XHCI_PORTSC_PRC) ||
            (!(portsc & XHCI_PORTSC_PR) && (portsc & XHCI_PORTSC_PED))) {
            resetDone = true;
            break;
        }
    }

    // Fallback to Warm Port Reset (WPR) for USB 3.x ports if standard reset didn't enable PED
    if (!resetDone || !(readPortsc(portNum) & XHCI_PORTSC_PED)) {
        portsc = readPortsc(portNum);
        writePortsc(portNum, (portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_WPR);
        for (u32 ms = 0; ms < 500; ++ms) {
            arch::ApicController::instance().sleepMs(1);
            portsc = readPortsc(portNum);
            if ((portsc & (XHCI_PORTSC_WRC | XHCI_PORTSC_PRC)) || (portsc & XHCI_PORTSC_PED)) {
                break;
            }
        }
    }

    // Clear reset change bits (PRC / WRC / CSC / PEC / PLC)
    portsc = readPortsc(portNum);
    writePortsc(portNum, (portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_CHANGE_BITS);

    // Allow 20 ms USB reset recovery (TRSTRCY) and drain Port Status Change Events
    arch::ApicController::instance().sleepMs(20);
    m_eventRing.drainEvents();

    portsc = readPortsc(portNum);
    if (!(portsc & XHCI_PORTSC_PED)) {
        pr_warn("xhci", "Port %u connected (PORTSC=0x%08x) but failed to enable after reset",
                portNum, portsc);
        return false;
    }

    u8 speed = static_cast<u8>((portsc >> 10) & 0x0FU);
    if (outSpeedId) *outSpeedId = speed;
    return (speed != 0);
}

Status XhciController::enumeratePortDevice(u8 portNum, u8 speedId) noexcept {
    // Step 1: Enable Slot Command
    XhciTrb enableSlotCmd{};
    enableSlotCmd.param   = 0;
    enableSlotCmd.status  = 0;
    enableSlotCmd.control = (static_cast<u32>(XHCI_TRB_ENABLE_SLOT) << 10);

    XhciTrb cmdComp{};
    Status st = submitCommand(enableSlotCmd, &cmdComp, 2000);
    if (st != Status::Ok) {
        pr_err("xhci", "Port %u: Enable Slot command failed", portNum);
        return st;
    }

    u8 slotId = cmdComp.getSlotId();
    if (slotId == 0 || slotId >= MAX_SLOTS_PER_CTRL) {
        pr_err("xhci", "Port %u: Invalid Slot ID %u returned by controller", portNum, slotId);
        return Status::IoError;
    }

    XhciSlotState& slot = m_slots[slotId];
    slot.slotId  = slotId;
    slot.portNum = portNum;
    slot.speedId = speedId;

    switch (speedId) {
        case XHCI_SPEED_LOW:   slot.maxPacketSize0 = 8;   break;
        case XHCI_SPEED_FULL:  slot.maxPacketSize0 = 64;  break;
        case XHCI_SPEED_HIGH:  slot.maxPacketSize0 = 64;  break;
        default:               slot.maxPacketSize0 = 512; break;
    }

    // Step 2: Allocate Output Device Context, Input Context, and EP0 Transfer Ring
    auto outPage = m_pmm->allocPages(1);
    if (outPage.isError()) return outPage.status();
    auto inPage = m_pmm->allocPages(1);
    if (inPage.isError()) return inPage.status();

    slot.outputCtxPhys = outPage.value();
    slot.outputCtxVirt = reinterpret_cast<u8*>(slot.outputCtxPhys + m_hhdmOffset);
    slot.inputCtxPhys  = inPage.value();
    slot.inputCtxVirt  = reinterpret_cast<u8*>(slot.inputCtxPhys + m_hhdmOffset);

    memset(slot.outputCtxVirt, 0, PAGE_SIZE_4K);
    memset(slot.inputCtxVirt, 0, PAGE_SIZE_4K);

    st = slot.ep0Ring.init(m_pmm, m_hhdmOffset);
    if (st != Status::Ok) return st;

    // Populate Input Context for Address Device (A0 = Slot, A1 = EP0)
    auto* icc     = reinterpret_cast<XhciInputControlContext*>(getInputControlCtx(slot));
    auto* slotCtx = reinterpret_cast<XhciSlotContext*>(getInputSlotCtx(slot));
    auto* ep0Ctx  = reinterpret_cast<XhciEndpointContext*>(getInputEpCtx(slot, 1));

    icc->dropFlags = 0;
    icc->addFlags  = (1U << 0) | (1U << 1);

    // Slot Context: Speed (bits 20..23), ContextEntries = 1 (bits 27..31), RootHubPortNumber (dw1 bits 16..23)
    slotCtx->dw0 = (static_cast<u32>(speedId) << 20) | (1U << 27);
    slotCtx->dw1 = (static_cast<u32>(portNum) << 16);
    slotCtx->dw2 = 0; // Interrupter 0

    // Endpoint 0 Context: CErr = 3 (bits 1..2), EPType = Control (4, bits 3..5), MaxPacketSize (bits 16..31)
    ep0Ctx->dw0       = 0;
    ep0Ctx->dw1       = (3U << 1) | (XHCI_EP_TYPE_CONTROL << 3) |
                        (static_cast<u32>(slot.maxPacketSize0) << 16);
    ep0Ctx->trDequeue = slot.ep0Ring.getPhysBase() | XHCI_TRB_CYCLE;
    ep0Ctx->dw4       = 8U; // Average TRB Length = 8

    m_dcbaaVirt[slotId] = slot.outputCtxPhys;
    arch::memoryBarrier();

    // Step 3: Submit Address Device Command (BSR = 0)
    XhciTrb addrCmd{};
    addrCmd.param   = slot.inputCtxPhys;
    addrCmd.status  = 0;
    addrCmd.control = (static_cast<u32>(XHCI_TRB_ADDRESS_DEVICE) << 10) |
                      (static_cast<u32>(slotId) << 24);

    st = submitCommand(addrCmd, &cmdComp, 3000);
    if (st != Status::Ok) {
        pr_warn("xhci", "Port %u (Slot %u): Address Device command failed (CC=%u)",
                portNum, slotId, cmdComp.getCompletionCode());
        return st;
    }

    slot.active = true;
    arch::ApicController::instance().sleepMs(10);

    // Step 4: Fetch 18-byte USB Device Descriptor
    UsbDeviceDescriptor devDesc{};
    UsbSetupPacket getDevDesc{};
    getDevDesc.bmRequestType = 0x80U;
    getDevDesc.bRequest      = USB_REQ_GET_DESCRIPTOR;
    getDevDesc.wValue        = static_cast<u16>((USB_DESC_DEVICE << 8) | 0U);
    getDevDesc.wIndex        = 0;
    getDevDesc.wLength       = sizeof(UsbDeviceDescriptor);

    st = controlTransfer(slotId, getDevDesc, &devDesc, sizeof(devDesc), true);
    if (st != Status::Ok) {
        pr_warn("xhci", "Port %u (Slot %u): Failed to read USB Device Descriptor", portNum, slotId);
        return st;
    }

    // If Full-Speed device reports a different EP0 MaxPacketSize, update EP0 context via Evaluate Context
    if (speedId == XHCI_SPEED_FULL &&
        (devDesc.bMaxPacketSize0 == 8 || devDesc.bMaxPacketSize0 == 16 ||
         devDesc.bMaxPacketSize0 == 32 || devDesc.bMaxPacketSize0 == 64) &&
        devDesc.bMaxPacketSize0 != slot.maxPacketSize0) {
        slot.maxPacketSize0 = devDesc.bMaxPacketSize0;
        memset(slot.inputCtxVirt, 0, PAGE_SIZE_4K);
        icc->addFlags = (1U << 1);
        ep0Ctx->dw1   = (3U << 1) | (XHCI_EP_TYPE_CONTROL << 3) |
                        (static_cast<u32>(slot.maxPacketSize0) << 16);

        XhciTrb evalCmd{};
        evalCmd.param   = slot.inputCtxPhys;
        evalCmd.status  = 0;
        evalCmd.control = (static_cast<u32>(XHCI_TRB_EVALUATE_CTX) << 10) |
                          (static_cast<u32>(slotId) << 24);
        (void)submitCommand(evalCmd, &cmdComp, 1000);
    }

    pr_info("xhci", "Port %u (Slot %u, %s): USB Device [VID:PID %04x:%04x] Class %02x:%02x",
            portNum, slotId, XhciManager::speedToString(speedId),
            devDesc.idVendor, devDesc.idProduct,
            devDesc.bDeviceClass, devDesc.bDeviceSubClass);

    // Step 5: Fetch Configuration Descriptor header (9 bytes), then full Configuration bundle
    UsbConfigDescriptor cfgHdr{};
    UsbSetupPacket getCfgHdr{};
    getCfgHdr.bmRequestType = 0x80U;
    getCfgHdr.bRequest      = USB_REQ_GET_DESCRIPTOR;
    getCfgHdr.wValue        = static_cast<u16>((USB_DESC_CONFIGURATION << 8) | 0U);
    getCfgHdr.wIndex        = 0;
    getCfgHdr.wLength       = sizeof(UsbConfigDescriptor);

    st = controlTransfer(slotId, getCfgHdr, &cfgHdr, sizeof(cfgHdr), true);
    if (st != Status::Ok || cfgHdr.wTotalLength < sizeof(UsbConfigDescriptor)) {
        return Status::IoError;
    }

    u16 totalCfgLen = cfgHdr.wTotalLength;
    if (totalCfgLen > 1024U) totalCfgLen = 1024U;

    u8 cfgBuf[1024]{};
    getCfgHdr.wLength = totalCfgLen;
    st = controlTransfer(slotId, getCfgHdr, cfgBuf, totalCfgLen, true);
    if (st != Status::Ok) return st;

    // Step 6: Parse descriptors for a USB Mass Storage Bulk-Only Transport (BOT) Interface
    bool foundMscInterface = false;
    bool inMscInterface    = false;
    u8   mscInterfaceNum   = 0;
    u8   bulkInEpAddr      = 0;
    u8   bulkOutEpAddr     = 0;
    u16  bulkInMaxPkt      = 512;
    u16  bulkOutMaxPkt     = 512;
    u8   bulkInMaxBurst    = 0;
    u8   bulkOutMaxBurst   = 0;
    u8   lastEpDirIn       = 0;

    usize offset = 0;
    while (offset + 2 <= totalCfgLen) {
        u8 bLen  = cfgBuf[offset];
        u8 bType = cfgBuf[offset + 1];
        if (bLen < 2 || offset + bLen > totalCfgLen) break;

        if (bType == USB_DESC_INTERFACE && bLen >= sizeof(UsbInterfaceDescriptor)) {
            const auto* ifDesc = reinterpret_cast<const UsbInterfaceDescriptor*>(cfgBuf + offset);
            bool isMscBot = (ifDesc->bInterfaceClass == USB_CLASS_MASS_STORAGE) &&
                            (ifDesc->bInterfaceProtocol == USB_MSC_PROTO_BOT) &&
                            (ifDesc->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI ||
                             ifDesc->bInterfaceSubClass == USB_MSC_SUBCLASS_UFI  ||
                             ifDesc->bInterfaceSubClass == USB_MSC_SUBCLASS_RBC  ||
                             ifDesc->bInterfaceSubClass == 0x02U ||
                             ifDesc->bInterfaceSubClass == 0x05U);
            if (isMscBot && !foundMscInterface) {
                inMscInterface  = true;
                mscInterfaceNum = ifDesc->bInterfaceNumber;
            } else {
                inMscInterface = false;
            }
        } else if (bType == USB_DESC_ENDPOINT && bLen >= sizeof(UsbEndpointDescriptor) && inMscInterface) {
            const auto* epDesc = reinterpret_cast<const UsbEndpointDescriptor*>(cfgBuf + offset);
            u8 xferType = epDesc->bmAttributes & 0x03U;
            if (xferType == 0x02U) { // Bulk Endpoint
                if (epDesc->bEndpointAddress & 0x80U) {
                    bulkInEpAddr = epDesc->bEndpointAddress;
                    bulkInMaxPkt = epDesc->wMaxPacketSize & 0x07FFU;
                    lastEpDirIn  = 1;
                } else {
                    bulkOutEpAddr = epDesc->bEndpointAddress;
                    bulkOutMaxPkt = epDesc->wMaxPacketSize & 0x07FFU;
                    lastEpDirIn   = 2;
                }
                if (bulkInEpAddr != 0 && bulkOutEpAddr != 0) {
                    foundMscInterface = true;
                }
            }
        } else if (bType == USB_DESC_SS_EP_COMPANION && bLen >= sizeof(UsbSsEpCompanionDescriptor) && inMscInterface) {
            const auto* ssComp = reinterpret_cast<const UsbSsEpCompanionDescriptor*>(cfgBuf + offset);
            if (lastEpDirIn == 1) bulkInMaxBurst  = ssComp->bMaxBurst;
            if (lastEpDirIn == 2) bulkOutMaxBurst = ssComp->bMaxBurst;
        }

        offset += bLen;
    }

    if (!foundMscInterface || bulkInEpAddr == 0 || bulkOutEpAddr == 0) {
        return Status::Ok; // Non-storage USB device (e.g., HID, Hub, Bluetooth)
    }

    // Step 7: Allocate UsbMscDevice & Configure Bulk IN / Bulk OUT Endpoints in xHCI
    u32 mscIdx = 0;
    UsbMscDevice* mscDev = XhciManager::instance().allocateMscDevice(&mscIdx);
    if (!mscDev) {
        pr_warn("xhci", "Maximum USB Mass Storage device count reached");
        return Status::NoSpace;
    }

    st = mscDev->getBulkInRing().init(m_pmm, m_hhdmOffset);
    if (st != Status::Ok) return st;
    st = mscDev->getBulkOutRing().init(m_pmm, m_hhdmOffset);
    if (st != Status::Ok) return st;

    u8 bulkInDci  = static_cast<u8>(((bulkInEpAddr & 0x0FU) * 2U) + 1U);
    u8 bulkOutDci = static_cast<u8>((bulkOutEpAddr & 0x0FU) * 2U);
    u8 maxDci     = (bulkInDci > bulkOutDci) ? bulkInDci : bulkOutDci;

    memset(slot.inputCtxVirt, 0, PAGE_SIZE_4K);
    icc->dropFlags = 0;
    icc->addFlags  = (1U << 0) | (1U << bulkInDci) | (1U << bulkOutDci);

    const auto* outSlotCtx = reinterpret_cast<const XhciSlotContext*>(getOutputSlotCtx(slot));
    slotCtx->dw0 = (outSlotCtx->dw0 & ~(0x1FU << 27)) | (static_cast<u32>(maxDci) << 27);
    slotCtx->dw1 = outSlotCtx->dw1;
    slotCtx->dw2 = outSlotCtx->dw2;

    auto* epInCtx  = reinterpret_cast<XhciEndpointContext*>(getInputEpCtx(slot, bulkInDci));
    epInCtx->dw0       = 0;
    epInCtx->dw1       = (3U << 1) | (XHCI_EP_TYPE_BULK_IN << 3) |
                         (static_cast<u32>(bulkInMaxBurst) << 8) |
                         (static_cast<u32>(bulkInMaxPkt) << 16);
    epInCtx->trDequeue = mscDev->getBulkInRing().getPhysBase() | XHCI_TRB_CYCLE;
    epInCtx->dw4       = 1024U;

    auto* epOutCtx = reinterpret_cast<XhciEndpointContext*>(getInputEpCtx(slot, bulkOutDci));
    epOutCtx->dw0       = 0;
    epOutCtx->dw1       = (3U << 1) | (XHCI_EP_TYPE_BULK_OUT << 3) |
                          (static_cast<u32>(bulkOutMaxBurst) << 8) |
                          (static_cast<u32>(bulkOutMaxPkt) << 16);
    epOutCtx->trDequeue = mscDev->getBulkOutRing().getPhysBase() | XHCI_TRB_CYCLE;
    epOutCtx->dw4       = 1024U;

    arch::memoryBarrier();

    XhciTrb cfgEpCmd{};
    cfgEpCmd.param   = slot.inputCtxPhys;
    cfgEpCmd.status  = 0;
    cfgEpCmd.control = (static_cast<u32>(XHCI_TRB_CONFIGURE_EP) << 10) |
                       (static_cast<u32>(slotId) << 24);

    st = submitCommand(cfgEpCmd, &cmdComp, 3000);
    if (st != Status::Ok) {
        pr_err("xhci", "Port %u (Slot %u): Configure Endpoint failed (CC=%u)",
               portNum, slotId, cmdComp.getCompletionCode());
        return st;
    }

    // Step 8: Send USB SET_CONFIGURATION Control Transfer
    UsbSetupPacket setCfg{};
    setCfg.bmRequestType = 0x00U;
    setCfg.bRequest      = USB_REQ_SET_CONFIGURATION;
    setCfg.wValue        = cfgHdr.bConfigurationValue;
    setCfg.wIndex        = 0;
    setCfg.wLength       = 0;

    st = controlTransfer(slotId, setCfg, nullptr, 0, false);
    if (st != Status::Ok) {
        pr_err("xhci", "Port %u (Slot %u): SET_CONFIGURATION(%u) failed",
               portNum, slotId, cfgHdr.bConfigurationValue);
        return st;
    }

    arch::ApicController::instance().sleepMs(10);

    // Step 9: Initialize USB Mass Storage BOT / SCSI Block Device and scan GPT partitions
    st = mscDev->init(this,
                      mscIdx,
                      slotId,
                      portNum,
                      speedId,
                      devDesc.idVendor,
                      devDesc.idProduct,
                      mscInterfaceNum,
                      bulkInDci,
                      bulkOutDci,
                      bulkInEpAddr,
                      bulkOutEpAddr,
                      m_pmm,
                      m_hhdmOffset);
    if (st == Status::Ok) {
        drivers::storage::GptPartitionScanner::instance().scanDisk(mscDev);
    }

    return st;
}

// =============================================================================
// XhciManager Implementation
// =============================================================================

XhciManager& XhciManager::instance() noexcept {
    static XhciManager s_mgr;
    return s_mgr;
}

const char* XhciManager::speedToString(u8 speedId) noexcept {
    switch (speedId) {
        case XHCI_SPEED_LOW:        return "USB 1.0 Low-Speed (1.5 Mbps)";
        case XHCI_SPEED_FULL:       return "USB 1.1 Full-Speed (12 Mbps)";
        case XHCI_SPEED_HIGH:       return "USB 2.0 High-Speed (480 Mbps)";
        case XHCI_SPEED_SUPER:      return "USB 3.0 SuperSpeed (5 Gbps)";
        case XHCI_SPEED_SUPER_PLUS: return "USB 3.1 SuperSpeed+ (10 Gbps)";
        default:                    return "USB Unknown Speed";
    }
}

UsbMscDevice* XhciManager::allocateMscDevice(u32* outDeviceIndex) noexcept {
    if (m_mscCount >= MAX_USB_MSC_DEVICES) {
        return nullptr;
    }
    u32 idx = static_cast<u32>(m_mscCount++);
    if (outDeviceIndex) *outDeviceIndex = idx;
    return &m_mscDevices[idx];
}

Status XhciManager::init(memory::IPhysicalAllocator* pmm,
                         memory::IVirtualMapper* vmm,
                         u64 hhdmOffset) noexcept {
    if (!pmm || !vmm) return Status::InvalidArgument;

    m_ctrlCount = 0;
    m_mscCount  = 0;

    arch::IdtManager::instance().registerHandler(arch::IdtManager::VEC_XHCI_MSI, this);

    for (usize occ = 0; occ < MAX_XHCI_CONTROLLERS; ++occ) {
        const arch::PciDeviceInfo* pciDev =
            arch::PcieManager::instance().findByClass(0x0C, 0x03, 0x30, occ);
        if (!pciDev) {
            break;
        }
        if (pciDev->bar0Phys == 0) {
            continue;
        }

        XhciController& ctrl = m_controllers[m_ctrlCount];
        if (ctrl.init(*pciDev, pmm, vmm, hhdmOffset) == Status::Ok) {
            ++m_ctrlCount;
        }
    }

    if (m_ctrlCount == 0) {
        pr_info("xhci", "No PCIe xHCI controllers (0C:03:30) detected");
        return Status::DeviceNotFound;
    }

    return Status::Ok;
}

void XhciManager::onInterrupt(arch::InterruptFrame*) noexcept {
    // MSI/MSI-X interrupt notification (Event Ring is drained by XhciEventRing)
}

} // namespace cortez::drivers::usb
