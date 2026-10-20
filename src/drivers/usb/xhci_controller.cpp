/**
 * @file xhci_controller.cpp
 * @brief Implementation of the PCIe xHCI Host Controller, BIOS Handoff, USB Port Enumeration,
 *        and Multi-Controller XhciManager.
 */

#include "xhci_controller.hpp"
#include "usb_port.hpp"
#include "usb_keyboard_device.hpp"
#include "../input/input_manager.hpp"
#include "../storage/storage_manager.hpp"
#include "../storage/gpt_partition.hpp"
#include "../storage/mbr_partition.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../klibc/klibc.hpp"
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

void XhciController::safeWritePortsc(u8 portNum, u32 setBits, u32 clearChangeBits) noexcept {
    u32 current = readPortsc(portNum);
    // Always preserve Port Power (PP = bit 9).
    // Clear PLS field (bits 5..8) and mask out RW1C bits so uncommanded state transitions are prevented.
    u32 val = (current & ~XHCI_PORTSC_RW1C_MASK & ~XHCI_PORTSC_PLS_MASK) | XHCI_PORTSC_PP;
    val |= setBits;
    val |= (clearChangeBits & XHCI_PORTSC_CHANGE_BITS);
    writePortsc(portNum, val);
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

            pr_dbg("xhci", "Completed BIOS/UEFI-to-OS xHCI handoff (USBLEGSUP @ +0x%x)", offset);
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

void XhciController::parseSupportedProtocols() noexcept {
    m_protocolCount = 0;
    u32 hcc1 = readCap32(XHCI_CAP_HCCPARAMS1);
    u32 xecp = (hcc1 >> 16) & 0xFFFFU;
    if (xecp == 0) return;

    u32 offset = xecp << 2;
    for (u32 guard = 0; offset != 0 && offset + 12 <= 131072U && guard < 64; ++guard) {
        u32 capHdr  = readCap32(offset);
        u8  capId   = static_cast<u8>(capHdr & 0xFFU);
        u8  nextPtr = static_cast<u8>((capHdr >> 8) & 0xFFU);

        if (capId == XHCI_EXT_CAP_PROTO && m_protocolCount < MAX_PROTOCOLS) {
            u8 minorRev = static_cast<u8>((capHdr >> 16) & 0xFFU);
            u8 majorRev = static_cast<u8>((capHdr >> 24) & 0xFFU);
            u32 dw2     = readCap32(offset + 8);
            u8  pOffset = static_cast<u8>(dw2 & 0xFFU);
            u8  pCount  = static_cast<u8>((dw2 >> 8) & 0xFFU);

            if (pOffset >= 1 && pCount > 0) {
                m_protocols[m_protocolCount++] = XhciProtocolRange{majorRev, minorRev, pOffset, pCount};
                pr_dbg("xhci", "[usb-bus %u] Supported Protocol USB %u.%02x: Ports %u..%u (count %u)",
                        m_busIndex, majorRev, minorRev, pOffset, pOffset + pCount - 1, pCount);
            }
        }

        if (nextPtr == 0) break;
        offset += static_cast<u32>(nextPtr) << 2;
    }
}

bool XhciController::isPortValid(u8 portNum, u8* outMajor, u8* outMinor) const noexcept {
    if (m_protocolCount == 0) {
        if (portNum >= 1 && portNum <= m_maxPorts) {
            if (outMajor) *outMajor = 2;
            if (outMinor) *outMinor = 0;
            return true;
        }
        return false;
    }
    for (usize i = 0; i < m_protocolCount; ++i) {
        const auto& p = m_protocols[i];
        if (portNum >= p.portOffset && portNum < (p.portOffset + p.portCount)) {
            if (outMajor) *outMajor = p.majorRev;
            if (outMinor) *outMinor = p.minorRev;
            return true;
        }
    }
    return false;
}

bool XhciController::debouncePort(u8 portNum, bool expectedCcs) noexcept {
    u32 stableMs = 0;
    constexpr u32 STEP_MS = 25;
    constexpr u32 STABLE_REQUIRED_MS = 100;
    constexpr u32 MAX_WAIT_MS = 400;

    for (u32 elapsed = 0; elapsed < MAX_WAIT_MS; elapsed += STEP_MS) {
        arch::ApicController::instance().sleepMs(STEP_MS);
        bool currentCcs = (readPortsc(portNum) & XHCI_PORTSC_CCS) != 0;
        if (currentCcs == expectedCcs) {
            stableMs += STEP_MS;
            if (stableMs >= STABLE_REQUIRED_MS) {
                return true;
            }
        } else {
            stableMs = 0;
        }
    }
    return ((readPortsc(portNum) & XHCI_PORTSC_CCS) != 0) == expectedCcs;
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
                            u8 busIndex,
                            memory::IPhysicalAllocator* pmm,
                            memory::IVirtualMapper* vmm,
                            u64 hhdmOffset) noexcept {
    if (!pmm || !vmm || pciDev.bar0Phys == 0) {
        return Status::InvalidArgument;
    }

    m_pciDev     = pciDev;
    m_busIndex   = busIndex;
    m_pmm        = pmm;
    m_vmm        = vmm;
    m_hhdmOffset = hhdmOffset;

    pr_dbg("xhci", "[usb-bus %u] Found PCIe xHCI controller at %02x:%02x.%u [VID:DID %04x:%04x] BAR0=0x%llx",
            m_busIndex, pciDev.bus, pciDev.device, pciDev.function,
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

    // Step 1b: Parse Supported Protocol Extended Capabilities (§7.2)
    parseSupportedProtocols();

    // Step 2: Halt controller before reset (USBCMD.RS = 0 -> wait for USBSTS.HCH = 1)
    u32 cmd = readOp32(XHCI_OP_USBCMD);
    if (cmd & XHCI_CMD_RS) {
        writeOp32(XHCI_OP_USBCMD, cmd & ~XHCI_CMD_RS);
    }
    if (!waitOpBit(XHCI_OP_USBSTS, XHCI_STS_HCH, XHCI_STS_HCH, 500)) {
        pr_dbg("xhci", "Controller did not report HCH=1 before reset; proceeding with HCRST");
    }

    // Step 3: Issue Host Controller Reset (USBCMD.HCRST = 1) and wait for HCRST=0 and CNR=0
    writeOp32(XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    if (!waitOpBit(XHCI_OP_USBCMD, XHCI_CMD_HCRST, 0, 1000) ||
        !waitOpBit(XHCI_OP_USBSTS, XHCI_STS_CNR, 0, 1000)) {
        pr_dbg("xhci", "Timeout waiting for xHCI reset completion");
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
        pr_dbg("xhci", "Timeout waiting for xHCI controller to enter Running state (HCH=0)");
        return Status::Timeout;
    }

    m_ready = true;
    pr_dbg("xhci", "[usb-bus %u] xHCI %x.%02x ready: %u ports, %u slots, %zuB contexts",
          m_busIndex, (m_hciVersion >> 8) & 0xFFU, m_hciVersion & 0xFFU,
          m_maxPorts, m_maxSlots, m_contextSize);

    // Register only verified physical ports discovered from Supported Protocol capabilities
    for (u8 port = 1; port <= m_maxPorts && port <= MAX_PORTS_PER_CTRL; ++port) {
        u8 major = 2, minor = 0;
        if (isPortValid(port, &major, &minor)) {
            m_ports[port - 1].init(this, m_busIndex, port, major, minor);
            (void)UsbPortManager::instance().registerPort(&m_ports[port - 1]);
        }
    }

    // Set up peer port mappings between USB 2.0 and USB 3.x ports sharing physical connectors
    const XhciProtocolRange* proto2 = nullptr;
    const XhciProtocolRange* proto3 = nullptr;
    for (usize i = 0; i < m_protocolCount; ++i) {
        if (m_protocols[i].majorRev == 2 && !proto2) proto2 = &m_protocols[i];
        if (m_protocols[i].majorRev == 3 && !proto3) proto3 = &m_protocols[i];
    }
    if (proto2 && proto3) {
        u8 pairCount = (proto2->portCount < proto3->portCount) ? proto2->portCount : proto3->portCount;
        for (u8 p = 0; p < pairCount; ++p) {
            u8 p2 = proto2->portOffset + p;
            u8 p3 = proto3->portOffset + p;
            if (p2 <= MAX_PORTS_PER_CTRL && p3 <= MAX_PORTS_PER_CTRL) {
                m_ports[p2 - 1].setPeerPortNum(p3);
                m_ports[p3 - 1].setPeerPortNum(p2);
            }
        }
    }

    // Step 9: Power and probe all root hub ports
    probePorts();
    return Status::Ok;
}

UsbPort* XhciController::getUsbPort(u8 portNum) noexcept {
    if (portNum >= 1 && portNum <= m_maxPorts && portNum <= MAX_PORTS_PER_CTRL) {
        return &m_ports[portNum - 1];
    }
    return nullptr;
}

// =============================================================================
// Command Submission, Control Transfers & Bulk Transfers
// =============================================================================

bool XhciController::dispatchToWaiters(const XhciTrb& trb) noexcept {
    IrqLockGuard guard(m_waiterLock);
    u8          type    = trb.getType();
    u8          slotId  = trb.getSlotId();
    u8          dci     = trb.getEndpointId();
    phys_addr_t trbPhys = trb.param;

    for (usize i = 0; i < MAX_WAITERS; ++i) {
        auto* w = m_waiters[i];
        if (!w || w->completed) continue;
        if (w->expectedType != type) continue;

        if (type == XHCI_TRB_CMD_COMPLETION) {
            if (w->trbPhys == 0 || w->trbPhys == trbPhys) {
                w->capturedTrb = trb;
                w->completed   = true;
                return true;
            }
        } else if (type == XHCI_TRB_TRANSFER_EVENT) {
            if ((w->slotId == 0 || w->slotId == slotId) &&
                (w->dci == 0 || w->dci == dci) &&
                (w->trbPhys == 0 || w->trbPhys == trbPhys)) {
                w->capturedTrb = trb;
                w->completed   = true;
                return true;
            }
        }
    }
    return false;
}

void XhciController::registerWaiter(XhciEventWaiter* w) noexcept {
    if (!w) return;
    IrqLockGuard guard(m_waiterLock);
    for (usize i = 0; i < MAX_WAITERS; ++i) {
        if (!m_waiters[i]) {
            m_waiters[i] = w;
            return;
        }
    }
}

void XhciController::unregisterWaiter(XhciEventWaiter* w) noexcept {
    if (!w) return;
    IrqLockGuard guard(m_waiterLock);
    for (usize i = 0; i < MAX_WAITERS; ++i) {
        if (m_waiters[i] == w) {
            m_waiters[i] = nullptr;
            return;
        }
    }
}

Status XhciController::waitForEvent(XhciEventWaiter* waiter, u32 timeoutMs) noexcept {
    if (!waiter) return Status::InvalidArgument;
    u64 startMs = arch::ApicController::instance().getUptimeMs();
    u32 spinGuard = 0;
    u32 maxSpins  = timeoutMs * 40000U;

    while (!waiter->completed) {
        m_eventRing.dispatchEvents(this);
        if (waiter->completed) break;

        arch::pause();
        ++spinGuard;
        u64 nowMs = arch::ApicController::instance().getUptimeMs();
        if ((nowMs > startMs && (nowMs - startMs) >= timeoutMs) || spinGuard > maxSpins) {
            return Status::Timeout;
        }
    }

    u8 cc = waiter->capturedTrb.getCompletionCode();
    if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
        return Status::Ok;
    }
    return Status::IoError;
}

Status XhciController::submitCommand(const XhciTrb& cmdTrb,
                                     XhciTrb* outCompletion,
                                     u32 timeoutMs) noexcept {
    IrqLockGuard guard(m_cmdLock);
    XhciEventWaiter waiter{};
    waiter.expectedType = XHCI_TRB_CMD_COMPLETION;

    phys_addr_t cmdPhys = m_cmdRing.enqueue(cmdTrb);
    waiter.trbPhys      = cmdPhys;

    registerWaiter(&waiter);
    ringDoorbell(0, 0); // Doorbell 0, Target 0 = Host Controller Command Ring

    Status st = waitForEvent(&waiter, timeoutMs);
    unregisterWaiter(&waiter);

    if (outCompletion) {
        *outCompletion = waiter.capturedTrb;
    }
    return st;
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

    XhciEventWaiter waiter{};
    waiter.expectedType = XHCI_TRB_TRANSFER_EVENT;
    waiter.slotId       = slotId;
    waiter.dci          = 1;
    phys_addr_t statusPhys = slot.ep0Ring.enqueue(statusTrb);
    waiter.trbPhys      = statusPhys;

    registerWaiter(&waiter);
    // Ring Doorbell for Slot (Target = 1 for Control Endpoint 0)
    ringDoorbell(slotId, 1U);

    Status st = waitForEvent(&waiter, 3000);
    unregisterWaiter(&waiter);

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
                                    bool isDataIn,
                                    u32* outActualLength) noexcept {
    if (slotId == 0 || slotId >= MAX_SLOTS_PER_CTRL || !m_slots[slotId].active) {
        return Status::InvalidArgument;
    }

    XhciTrb xferTrb{};
    xferTrb.param   = dmaPhys;
    xferTrb.status  = length; // TRB Transfer Length (bits 0..16), TD Size = 0, Interrupter = 0
    xferTrb.control = (static_cast<u32>(XHCI_TRB_NORMAL) << 10) |
                      XHCI_TRB_IOC |
                      (isDataIn ? XHCI_TRB_ISP : 0U);

    XhciEventWaiter waiter{};
    waiter.expectedType = XHCI_TRB_TRANSFER_EVENT;
    waiter.slotId       = slotId;
    waiter.dci          = dci;

    phys_addr_t trbPhys = ring.enqueue(xferTrb);
    waiter.trbPhys      = trbPhys;

    registerWaiter(&waiter);
    ringDoorbell(slotId, dci);

    Status st = waitForEvent(&waiter, 5000);
    unregisterWaiter(&waiter);

    if (st != Status::Ok) {
        if (waiter.capturedTrb.getCompletionCode() == XHCI_CC_STALL_ERROR) {
            (void)clearEndpointStall(slotId, dci, epAddr, ring);
        }
        return st;
    }

    if (outActualLength) {
        u32 residual = waiter.capturedTrb.status & 0x00FFFFFFU;
        *outActualLength = (length >= residual) ? (length - residual) : length;
    }

    return Status::Ok;
}

// =============================================================================
// Port Reset & USB Device / Mass Storage Enumeration
// =============================================================================

void XhciController::probePorts() noexcept {
    // Ensure Port Power (PP) is asserted on all valid physical ports
    for (u8 port = 1; port <= m_maxPorts; ++port) {
        if (!isPortValid(port)) continue;
        safeWritePortsc(port, XHCI_PORTSC_PP, 0);
    }

    // Wait 100 ms for port power & USB 3.x / 2.0 link detection to settle
    arch::ApicController::instance().sleepMs(100);
    m_eventRing.dispatchEvents(this);

    pr_dbg("xhci", "[usb-bus %u] Scanning valid root hub ports for connected USB devices...", m_busIndex);

    // Pass 1: Probe USB 3.x ports first so SuperSpeed devices bind before companion USB 2.0 lines
    for (u8 port = 1; port <= m_maxPorts; ++port) {
        if (!isPortValid(port)) continue;
        if (port <= MAX_PORTS_PER_CTRL && !m_ports[port - 1].isUsb3()) continue;

        u32 portsc = readPortsc(port);
        bool connected = (portsc & XHCI_PORTSC_CCS) != 0;
        if (port <= MAX_PORTS_PER_CTRL) {
            m_ports[port - 1].setPhysicallyConnected(connected);
        }
        if (!connected) continue;

        pr_dbg("xhci", "[usb-bus %u] Port %u (USB 3.x): Connection detected (PORTSC=0x%08x), probing device...",
                m_busIndex, port, portsc);
        u8 speedId = 0;
        if (resetPort(port, &speedId)) {
            (void)enumeratePortDevice(port, speedId);
        } else {
            pr_dbg("xhci", "[usb-bus %u] Port %u: Failed to reset and enable port", m_busIndex, port);
        }
    }

    // Pass 2: Probe USB 2.0 ports
    for (u8 port = 1; port <= m_maxPorts; ++port) {
        if (!isPortValid(port)) continue;
        if (port <= MAX_PORTS_PER_CTRL && m_ports[port - 1].isUsb3()) continue;

        u32 portsc = readPortsc(port);
        bool connected = (portsc & XHCI_PORTSC_CCS) != 0;
        if (port <= MAX_PORTS_PER_CTRL) {
            m_ports[port - 1].setPhysicallyConnected(connected);
        }
        if (!connected) continue;

        // If companion USB 3.x port on the same physical connector already claimed this device, skip USB 2.0 probe
        if (port <= MAX_PORTS_PER_CTRL) {
            u8 peer = m_ports[port - 1].getPeerPortNum();
            if (peer >= 1 && peer <= m_maxPorts) {
                if (m_ports[peer - 1].hasAttachedDevice()) {
                    continue;
                }
                u32 peerPortsc = readPortsc(peer);
                u8 peerSpeed = static_cast<u8>((peerPortsc >> 10) & 0x0FU);
                if ((peerPortsc & XHCI_PORTSC_PED) && peerSpeed >= XHCI_SPEED_SUPER) {
                    continue;
                }
            }
        }

        pr_dbg("xhci", "[usb-bus %u] Port %u (USB 2.0): Connection detected (PORTSC=0x%08x), probing device...",
                m_busIndex, port, portsc);
        u8 speedId = 0;
        if (resetPort(port, &speedId)) {
            (void)enumeratePortDevice(port, speedId);
        } else {
            pr_dbg("xhci", "[usb-bus %u] Port %u: Failed to reset and enable port", m_busIndex, port);
        }
    }

    // Settle port events from initial probe, clear change bits, and drain event ring
    arch::ApicController::instance().sleepMs(20);
    for (u8 p = 1; p <= m_maxPorts; ++p) {
        if (isPortValid(p)) {
            safeWritePortsc(p, 0, XHCI_PORTSC_CHANGE_BITS);
        }
    }
    m_eventRing.drainEvents();
    m_pendingPortMask = 0;
    writeOp32(XHCI_OP_USBSTS, XHCI_STS_PCD | XHCI_STS_EINT);
}

bool XhciController::resetPort(u8 portNum, u8* outSpeedId) noexcept {
    u8 major = 2, minor = 0;
    bool isUsb3Port = isPortValid(portNum, &major, &minor) && (major >= 3);

    u32 portsc = readPortsc(portNum);
    if (!(portsc & XHCI_PORTSC_CCS)) return false;

    u8 speed = static_cast<u8>((portsc >> 10) & 0x0FU);
    u32 pls = (portsc & XHCI_PORTSC_PLS_MASK) >> 5;

    // xHCI §4.3.1: For SuperSpeed ports already trained to U0 and enabled, skip redundant reset.
    if ((portsc & XHCI_PORTSC_PED) && speed >= XHCI_SPEED_SUPER && pls == 0) {
        safeWritePortsc(portNum, 0, XHCI_PORTSC_CHANGE_BITS);
        m_eventRing.dispatchEvents(this);
        if (outSpeedId) *outSpeedId = speed;
        return true;
    }

    // Clear any latched status change bits while preserving Port Power and PLS=0
    safeWritePortsc(portNum, 0, XHCI_PORTSC_CHANGE_BITS);

    // Assert Port Reset (PR = bit 4)
    safeWritePortsc(portNum, XHCI_PORTSC_PR, 0);

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

    // Fallback to Warm Port Reset (WPR) ONLY for USB 3.x ports if standard reset didn't enable PED
    if (isUsb3Port && (!resetDone || !(readPortsc(portNum) & XHCI_PORTSC_PED))) {
        safeWritePortsc(portNum, XHCI_PORTSC_WPR, 0);
        for (u32 ms = 0; ms < 500; ++ms) {
            arch::ApicController::instance().sleepMs(1);
            portsc = readPortsc(portNum);
            if ((portsc & (XHCI_PORTSC_WRC | XHCI_PORTSC_PRC)) || (portsc & XHCI_PORTSC_PED)) {
                break;
            }
        }
    }

    // Clear reset change bits
    safeWritePortsc(portNum, 0, XHCI_PORTSC_CHANGE_BITS);

    // Allow 20 ms USB reset recovery (TRSTRCY) and dispatch any concurrent events
    arch::ApicController::instance().sleepMs(20);
    m_eventRing.dispatchEvents(this);

    portsc = readPortsc(portNum);
    if (!(portsc & XHCI_PORTSC_PED)) {
        pr_dbg("xhci", "[usb-bus %u] Port %u connected (PORTSC=0x%08x) but failed to enable after reset",
                m_busIndex, portNum, portsc);
        return false;
    }

    speed = static_cast<u8>((portsc >> 10) & 0x0FU);
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
        pr_dbg("xhci", "Port %u: Enable Slot command failed", portNum);
        return st;
    }

    u8 slotId = cmdComp.getSlotId();
    if (slotId == 0 || slotId >= MAX_SLOTS_PER_CTRL) {
        pr_dbg("xhci", "Port %u: Invalid Slot ID %u returned by controller", portNum, slotId);
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
    if (outPage.isError()) {
        teardownSlot(slotId);
        return outPage.status();
    }
    auto inPage = m_pmm->allocPages(1);
    if (inPage.isError()) {
        teardownSlot(slotId);
        return inPage.status();
    }

    slot.outputCtxPhys = outPage.value();
    slot.outputCtxVirt = reinterpret_cast<u8*>(slot.outputCtxPhys + m_hhdmOffset);
    slot.inputCtxPhys  = inPage.value();
    slot.inputCtxVirt  = reinterpret_cast<u8*>(slot.inputCtxPhys + m_hhdmOffset);

    memset(slot.outputCtxVirt, 0, PAGE_SIZE_4K);
    memset(slot.inputCtxVirt, 0, PAGE_SIZE_4K);

    st = slot.ep0Ring.init(m_pmm, m_hhdmOffset);
    if (st != Status::Ok) {
        teardownSlot(slotId);
        return st;
    }

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
        pr_dbg("xhci", "Port %u (Slot %u): Address Device command failed (CC=%u)",
                portNum, slotId, cmdComp.getCompletionCode());
        teardownSlot(slotId);
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
        pr_dbg("xhci", "Port %u (Slot %u): Failed to read USB Device Descriptor", portNum, slotId);
        teardownSlot(slotId);
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

    pr_dbg("xhci", "Port %u (Slot %u, %s): USB Device [VID:PID %04x:%04x] Class %02x:%02x",
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
        teardownSlot(slotId);
        return Status::IoError;
    }

    u16 totalCfgLen = cfgHdr.wTotalLength;
    if (totalCfgLen > 1024U) totalCfgLen = 1024U;

    u8 cfgBuf[1024]{};
    getCfgHdr.wLength = totalCfgLen;
    st = controlTransfer(slotId, getCfgHdr, cfgBuf, totalCfgLen, true);
    if (st != Status::Ok) {
        teardownSlot(slotId);
        return st;
    }

    // Step 6: Parse descriptors and classify USB device (keyboard, mouse, audio, hub, video, storage)
    bool isAudioDev = (devDesc.bDeviceClass == 0x01U);
    bool isHubDev   = (devDesc.bDeviceClass == 0x09U);
    bool isVideoDev = (devDesc.bDeviceClass == 0x0EU);

    bool hasAudioInterface = false;
    bool hasHubInterface   = false;
    bool hasVideoInterface = false;
    bool hasMouseInterface = false;

    bool foundMscInterface = false;
    bool inMscInterface    = false;
    u8   mscInterfaceNum   = 0;
    u8   mscAltSetting     = 0;
    u8   bulkInEpAddr      = 0;
    u8   bulkOutEpAddr     = 0;
    u16  bulkInMaxPkt      = 512;
    u16  bulkOutMaxPkt     = 512;
    u8   bulkInMaxBurst    = 0;
    u8   bulkOutMaxBurst   = 0;
    u8   lastEpDirIn       = 0;

    bool foundKbdInterface = false;
    bool inKbdInterface    = false;
    u8   kbdInterfaceNum   = 0;
    u8   intrInEpAddr      = 0;
    u16  intrInMaxPkt      = 8;
    u8   intrInInterval    = 10;

    usize offset = 0;
    while (offset + 2 <= totalCfgLen) {
        u8 bLen  = cfgBuf[offset];
        u8 bType = cfgBuf[offset + 1];
        if (bLen < 2 || offset + bLen > totalCfgLen) break;

        if (bType == USB_DESC_INTERFACE && bLen >= sizeof(UsbInterfaceDescriptor)) {
            const auto* ifDesc = reinterpret_cast<const UsbInterfaceDescriptor*>(cfgBuf + offset);

            if (ifDesc->bInterfaceClass == 0x01U) {
                hasAudioInterface = true;
            } else if (ifDesc->bInterfaceClass == 0x09U) {
                hasHubInterface = true;
            } else if (ifDesc->bInterfaceClass == 0x0EU) {
                hasVideoInterface = true;
            } else if (ifDesc->bInterfaceClass == 0x03U) { // HID
                if (ifDesc->bInterfaceProtocol == 0x02U) {
                    hasMouseInterface = true;
                }
            }

            bool isMscBot = (ifDesc->bInterfaceClass == USB_CLASS_MASS_STORAGE) &&
                            (ifDesc->bInterfaceProtocol == USB_MSC_PROTO_BOT || ifDesc->bInterfaceProtocol == 0x00U) &&
                            (ifDesc->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI ||
                             ifDesc->bInterfaceSubClass == USB_MSC_SUBCLASS_UFI  ||
                             ifDesc->bInterfaceSubClass == USB_MSC_SUBCLASS_RBC  ||
                             ifDesc->bInterfaceSubClass == 0x00U ||
                             ifDesc->bInterfaceSubClass == 0x02U ||
                             ifDesc->bInterfaceSubClass == 0x05U);

            // Strict keyboard detection: HID class (0x03) and Keyboard protocol (0x01)
            // Exclude audio devices, mice, hubs
            bool isKeyboard = (ifDesc->bInterfaceClass == 0x03U) &&
                              (ifDesc->bInterfaceProtocol == 0x01U) &&
                              !isAudioDev;

            if (isMscBot && !foundMscInterface) {
                inMscInterface  = true;
                inKbdInterface  = false;
                mscInterfaceNum = ifDesc->bInterfaceNumber;
                mscAltSetting   = ifDesc->bAlternateSetting;
            } else if (isKeyboard && !foundKbdInterface) {
                inKbdInterface  = true;
                inMscInterface  = false;
                kbdInterfaceNum = ifDesc->bInterfaceNumber;
            } else {
                inMscInterface = false;
                inKbdInterface = false;
            }
        } else if (bType == USB_DESC_ENDPOINT && bLen >= sizeof(UsbEndpointDescriptor)) {
            const auto* epDesc = reinterpret_cast<const UsbEndpointDescriptor*>(cfgBuf + offset);
            u8 xferType = epDesc->bmAttributes & 0x03U;
            if (inMscInterface && xferType == 0x02U) { // Bulk Endpoint
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
            } else if (inKbdInterface && xferType == 0x03U) { // Interrupt Endpoint
                if (epDesc->bEndpointAddress & 0x80U) {
                    intrInEpAddr      = epDesc->bEndpointAddress;
                    intrInMaxPkt      = epDesc->wMaxPacketSize & 0x07FFU;
                    intrInInterval    = epDesc->bInterval;
                    foundKbdInterface = true;
                }
            }
        } else if (bType == USB_DESC_SS_EP_COMPANION && bLen >= sizeof(UsbSsEpCompanionDescriptor) && inMscInterface) {
            const auto* ssComp = reinterpret_cast<const UsbSsEpCompanionDescriptor*>(cfgBuf + offset);
            if (lastEpDirIn == 1) bulkInMaxBurst  = ssComp->bMaxBurst;
            if (lastEpDirIn == 2) bulkOutMaxBurst = ssComp->bMaxBurst;
        }

        offset += bLen;
    }

    char vBuf[16]{}, pBuf[32]{};
    klibc_snprintf(vBuf, sizeof(vBuf), "%04x", devDesc.idVendor);
    klibc_snprintf(pBuf, sizeof(pBuf), "%04x", devDesc.idProduct);

    // Strict classification: only acknowledge USB Keyboards and USB Mass Storage.
    // Anything other than that is strictly ignored.
    bool isKeyboard = (foundKbdInterface && intrInEpAddr != 0 &&
                       !isAudioDev && !hasAudioInterface &&
                       !hasMouseInterface &&
                       !isHubDev && !hasHubInterface &&
                       !isVideoDev && !hasVideoInterface);
    bool isStorage  = (foundMscInterface && bulkInEpAddr != 0 && bulkOutEpAddr != 0 &&
                       !isHubDev && !hasHubInterface);

    if (!isKeyboard && !isStorage) {
        UsbAttachedType classifiedType = UsbAttachedType::Unsupported;
        if (isVideoDev || hasVideoInterface) {
            classifiedType = UsbAttachedType::Video;
        } else if (devDesc.bDeviceClass == 0xE0U || devDesc.idVendor == 0x8087U) {
            classifiedType = UsbAttachedType::Bluetooth;
        } else if (hasMouseInterface || (devDesc.bDeviceClass == 0x03U)) {
            classifiedType = UsbAttachedType::MouseOrTouch;
        } else if (isHubDev || hasHubInterface) {
            classifiedType = UsbAttachedType::Hub;
        } else if (isAudioDev || hasAudioInterface) {
            classifiedType = UsbAttachedType::Audio;
        } else if (devDesc.bDeviceClass == 0xFFU) {
            classifiedType = UsbAttachedType::VendorSpecific;
        }

        teardownSlot(slotId);
        if (portNum >= 1 && portNum <= m_maxPorts && portNum <= MAX_PORTS_PER_CTRL) {
            m_ports[portNum - 1].setAttachedDevice(classifiedType, 0, vBuf, pBuf);
        }
        return Status::Ok;
    }

    // Branch A: Verified USB Keyboard
    if (isKeyboard) {
        pr_dbg("xhci", "Port %u (Slot %u): Verified USB Keyboard [%s:%s] (Protocol 1) -> Configuring keyboard driver",
              portNum, slotId, vBuf, pBuf);
        // Step 7: Allocate UsbKeyboardDevice & Configure Interrupt IN Endpoint in xHCI
        u32 kbdIdx = 0;
        UsbKeyboardDevice* kbdDev = XhciManager::instance().allocateKeyboardDevice(&kbdIdx);
        if (!kbdDev) {
            pr_dbg("xhci", "Maximum USB Keyboard device count reached");
            teardownSlot(slotId);
            return Status::NoSpace;
        }

        u8 intrInDci = static_cast<u8>(((intrInEpAddr & 0x0FU) * 2U) + 1U);

        memset(slot.inputCtxVirt, 0, PAGE_SIZE_4K);
        icc->dropFlags = 0;
        icc->addFlags  = (1U << 0) | (1U << intrInDci);

        const auto* outSlotCtx = reinterpret_cast<const XhciSlotContext*>(getOutputSlotCtx(slot));
        slotCtx->dw0 = (outSlotCtx->dw0 & ~(0x1FU << 27)) | (static_cast<u32>(intrInDci) << 27);
        slotCtx->dw1 = outSlotCtx->dw1;
        slotCtx->dw2 = outSlotCtx->dw2;

        auto* epInCtx  = reinterpret_cast<XhciEndpointContext*>(getInputEpCtx(slot, intrInDci));
        u32 ival = 3;
        if (intrInInterval > 0) {
            u32 microframes = static_cast<u32>(intrInInterval) * 8U;
            while ((1U << (ival + 1)) <= microframes && ival < 11) {
                ++ival;
            }
        }
        epInCtx->dw0       = (ival << 16);
        epInCtx->dw1       = (3U << 1) | (XHCI_EP_TYPE_INTR_IN << 3) |
                             (static_cast<u32>(intrInMaxPkt) << 16);
        epInCtx->trDequeue = kbdDev->getIntrRing().getPhysBase() | XHCI_TRB_CYCLE;
        epInCtx->dw4       = intrInMaxPkt;

        arch::memoryBarrier();

        XhciTrb cfgEpCmd{};
        cfgEpCmd.param   = slot.inputCtxPhys;
        cfgEpCmd.status  = 0;
        cfgEpCmd.control = (static_cast<u32>(XHCI_TRB_CONFIGURE_EP) << 10) |
                           (static_cast<u32>(slotId) << 24);

        st = submitCommand(cfgEpCmd, &cmdComp, 3000);
        if (st != Status::Ok) {
            pr_dbg("xhci", "Port %u (Slot %u): Configure Endpoint for Keyboard failed (CC=%u)",
                   portNum, slotId, cmdComp.getCompletionCode());
            teardownSlot(slotId);
            return st;
        }

        UsbSetupPacket setCfg{};
        setCfg.bmRequestType = 0x00U;
        setCfg.bRequest      = USB_REQ_SET_CONFIGURATION;
        setCfg.wValue        = cfgHdr.bConfigurationValue;
        setCfg.wIndex        = 0;
        setCfg.wLength       = 0;

        st = controlTransfer(slotId, setCfg, nullptr, 0, false);
        if (st != Status::Ok) {
            pr_dbg("xhci", "Port %u (Slot %u): SET_CONFIGURATION(%u) failed",
                   portNum, slotId, cfgHdr.bConfigurationValue);
            teardownSlot(slotId);
            return st;
        }

        arch::ApicController::instance().sleepMs(10);

        st = kbdDev->init(this,
                          kbdIdx,
                          slotId,
                          portNum,
                          speedId,
                          kbdInterfaceNum,
                          intrInDci,
                          intrInEpAddr,
                          intrInMaxPkt,
                          m_pmm,
                          m_hhdmOffset);
        if (st == Status::Ok) {
            input::InputManager::instance().registerDevice(kbdDev);
            drivers::DeviceRegistry::instance().registerCharDevice(kbdDev);
            if (portNum >= 1 && portNum <= m_maxPorts && portNum <= MAX_PORTS_PER_CTRL) {
                m_ports[portNum - 1].setAttachedDevice(UsbAttachedType::Keyboard, slotId, vBuf, pBuf);
            }
        } else {
            XhciManager::instance().releaseKeyboardDevice(kbdDev);
            teardownSlot(slotId);
        }
        return st;
    }

    // Branch B: Verified USB Mass Storage
    // Step 7: Allocate UsbMscDevice & Configure Bulk IN / Bulk OUT Endpoints in xHCI
    u32 mscIdx = 0;
    UsbMscDevice* mscDev = XhciManager::instance().allocateMscDevice(&mscIdx);
    if (!mscDev) {
        pr_dbg("xhci", "Maximum USB Mass Storage device count reached");
        teardownSlot(slotId);
        return Status::NoSpace;
    }

    st = mscDev->getBulkInRing().init(m_pmm, m_hhdmOffset);
    if (st != Status::Ok) {
        XhciManager::instance().releaseMscDevice(mscDev);
        teardownSlot(slotId);
        return st;
    }
    st = mscDev->getBulkOutRing().init(m_pmm, m_hhdmOffset);
    if (st != Status::Ok) {
        XhciManager::instance().releaseMscDevice(mscDev);
        teardownSlot(slotId);
        return st;
    }

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
        pr_dbg("xhci", "Port %u (Slot %u): Configure Endpoint failed (CC=%u)",
               portNum, slotId, cmdComp.getCompletionCode());
        XhciManager::instance().releaseMscDevice(mscDev);
        teardownSlot(slotId);
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
        pr_dbg("xhci", "Port %u (Slot %u): SET_CONFIGURATION(%u) failed",
               portNum, slotId, cfgHdr.bConfigurationValue);
        XhciManager::instance().releaseMscDevice(mscDev);
        teardownSlot(slotId);
        return st;
    }

    if (mscAltSetting != 0) {
        UsbSetupPacket setAlt{};
        setAlt.bmRequestType = 0x01U; // Host-to-Device | Standard | Interface
        setAlt.bRequest      = USB_REQ_SET_INTERFACE;
        setAlt.wValue        = mscAltSetting;
        setAlt.wIndex        = mscInterfaceNum;
        setAlt.wLength       = 0;
        (void)controlTransfer(slotId, setAlt, nullptr, 0, false);
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
        if (drivers::storage::MbrPartitionScanner::instance().scanDisk(mscDev) != Status::Ok) {
            drivers::storage::GptPartitionScanner::instance().scanDisk(mscDev);
        }

        char vBuf[16]{}, pBuf[32]{};
        klibc_snprintf(vBuf, sizeof(vBuf), "%04x", devDesc.idVendor);
        klibc_snprintf(pBuf, sizeof(pBuf), "%04x", devDesc.idProduct);
        if (portNum >= 1 && portNum <= m_maxPorts && portNum <= MAX_PORTS_PER_CTRL) {
            m_ports[portNum - 1].setAttachedDevice(UsbAttachedType::MassStorage, slotId, vBuf, pBuf);
        }

        const char* prodName = mscDev->getProductString();
        char cleanName[32]{};
        if (prodName && prodName[0] != '\0') {
            klibc_snprintf(cleanName, sizeof(cleanName), "%s", prodName);
        } else {
            klibc_snprintf(cleanName, sizeof(cleanName), "USB_Drive_%u", mscIdx);
        }
        drivers::storage::StorageManager::instance().onUsbStorageConnected(mscDev, cleanName);
    } else {
        XhciManager::instance().releaseMscDevice(mscDev);
        teardownSlot(slotId);
        if (portNum >= 1 && portNum <= m_maxPorts && portNum <= MAX_PORTS_PER_CTRL) {
            m_ports[portNum - 1].clearAttachedDevice();
        }
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
    for (usize i = 0; i < MAX_USB_MSC_DEVICES; ++i) {
        if (!m_mscDevices[i].isActive()) {
            if (outDeviceIndex) *outDeviceIndex = static_cast<u32>(i);
            return &m_mscDevices[i];
        }
    }
    return nullptr;
}

void XhciManager::releaseMscDevice(UsbMscDevice* dev) noexcept {
    if (!dev) return;
    dev->deinit();
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
        if (ctrl.init(*pciDev, static_cast<u8>(m_ctrlCount), pmm, vmm, hhdmOffset) == Status::Ok) {
            ++m_ctrlCount;
        }
    }

    if (m_ctrlCount == 0) {
        pr_dbg("xhci", "No PCIe xHCI controllers (0C:03:30) detected");
        return Status::DeviceNotFound;
    }

    return Status::Ok;
}

void XhciController::dispatchTransferEvent(const XhciTrb& trb) noexcept {
    u8 slotId = static_cast<u8>((trb.control >> 24) & 0xFFU);
    u8 dci    = static_cast<u8>((trb.control >> 16) & 0x1FU);
    if (auto* kbd = XhciManager::instance().findKeyboardBySlot(slotId)) {
        if (kbd->getIntrInDci() == dci) {
            kbd->handleTransferEvent(trb);
        }
    }
}

void XhciController::handlePortStatusChange(u8 portNum) noexcept {
    if (portNum < 1 || portNum > m_maxPorts) return;
    if (!isPortValid(portNum)) return;

    u32 portsc = readPortsc(portNum);
    bool ccs = (portsc & XHCI_PORTSC_CCS) != 0;
    bool csc = (portsc & XHCI_PORTSC_CSC) != 0;
    bool ped = (portsc & XHCI_PORTSC_PED) != 0;
    bool attached = (portNum <= MAX_PORTS_PER_CTRL) && m_ports[portNum - 1].hasAttachedDevice();
    bool physConnected = (portNum <= MAX_PORTS_PER_CTRL) && m_ports[portNum - 1].isPhysicallyConnected();

    // Clear change bits
    safeWritePortsc(portNum, 0, XHCI_PORTSC_CHANGE_BITS);

    // Case 1: Device is already attached on this port
    if (attached) {
        if (!ccs) {
            // Physical disconnect: verify with debounce (100ms stable low)
            if (!debouncePort(portNum, false)) {
                // False alarm, still connected
                return;
            }
            handlePortDisconnect(portNum);
            return;
        }

        // Port is still connected (CCS=1)
        if (ped) {
            // Port is enabled and device is operating. Normal link state or reset change
            // bits must NOT disconnect or re-enumerate an active device!
            return;
        }

        // If port is no longer enabled (e.g. quick unplug/replug with CSC set)
        if (csc) {
            handlePortDisconnect(portNum);
            handlePortConnect(portNum);
        }
        return;
    }

    // Case 2: No device is attached
    if (!ccs) {
        if (portNum <= MAX_PORTS_PER_CTRL) {
            m_ports[portNum - 1].setPhysicallyConnected(false);
        }
        return;
    }

    // CCS is 1, but no device attached.
    // If we already probed this port and it failed or was unsupported (physConnected is true)
    // and no CSC occurred, do NOT repeatedly probe floating/dead lines!
    if (physConnected && !csc) {
        return;
    }

    handlePortConnect(portNum);
}

void XhciController::handlePortConnect(u8 portNum) noexcept {
    if (portNum < 1 || portNum > m_maxPorts) return;
    if (!isPortValid(portNum)) return;

    // Check if peer USB 3.x port on the same physical connector is already active (SuperSpeed)
    if (portNum <= MAX_PORTS_PER_CTRL) {
        u8 peer = m_ports[portNum - 1].getPeerPortNum();
        if (peer >= 1 && peer <= m_maxPorts && !m_ports[portNum - 1].isUsb3()) {
            if (m_ports[peer - 1].hasAttachedDevice()) {
                m_ports[portNum - 1].setPhysicallyConnected(true);
                safeWritePortsc(portNum, 0, XHCI_PORTSC_CHANGE_BITS);
                return;
            }
            u32 peerPortsc = readPortsc(peer);
            u8 peerSpeed = static_cast<u8>((peerPortsc >> 10) & 0x0FU);
            if ((peerPortsc & XHCI_PORTSC_PED) && peerSpeed >= XHCI_SPEED_SUPER) {
                // Peer SuperSpeed port is active; suppress redundant USB 2.0 reset
                m_ports[portNum - 1].setPhysicallyConnected(true);
                safeWritePortsc(portNum, 0, XHCI_PORTSC_CHANGE_BITS);
                return;
            }
            // If peer USB 3.x port detects a connection or is training, allow it a moment to finish link training
            if (peerPortsc & XHCI_PORTSC_CCS) {
                arch::ApicController::instance().sleepMs(50);
                peerPortsc = readPortsc(peer);
                peerSpeed = static_cast<u8>((peerPortsc >> 10) & 0x0FU);
                if ((peerPortsc & XHCI_PORTSC_PED) && peerSpeed >= XHCI_SPEED_SUPER) {
                    m_ports[portNum - 1].setPhysicallyConnected(true);
                    safeWritePortsc(portNum, 0, XHCI_PORTSC_CHANGE_BITS);
                    return;
                }
            }
        }
    }

    // If an attached device is already active on this port, do NOT disconnect or reconnect!
    if (portNum <= MAX_PORTS_PER_CTRL && m_ports[portNum - 1].hasAttachedDevice()) {
        return;
    }

    // Clean up any stale slot referencing this port
    for (usize s = 1; s < MAX_SLOTS_PER_CTRL; ++s) {
        if (m_slots[s].active && m_slots[s].portNum == portNum) {
            teardownSlot(static_cast<u8>(s));
        }
    }

    // Debounce: ensure connection remains stably high for 100ms
    if (!debouncePort(portNum, true)) {
        if (portNum <= MAX_PORTS_PER_CTRL) {
            m_ports[portNum - 1].setPhysicallyConnected(false);
        }
        return;
    }

    if (portNum <= MAX_PORTS_PER_CTRL) {
        m_ports[portNum - 1].setPhysicallyConnected(true);
    }

    u8 speedId = 0;
    if (resetPort(portNum, &speedId)) {
        if (enumeratePortDevice(portNum, speedId) != Status::Ok) {
            pr_dbg("usb", "Port %u: Failed to enumerate USB device", portNum);
        }
    } else {
        pr_dbg("usb", "Port %u: Failed to enable USB port after reset", portNum);
    }
}

void XhciController::handlePortDisconnect(u8 portNum) noexcept {
    if (portNum < 1 || portNum > m_maxPorts) return;
    if (!isPortValid(portNum)) return;

    if (portNum <= MAX_PORTS_PER_CTRL) {
        m_ports[portNum - 1].setPhysicallyConnected(false);
    }

    u8 targetSlot = 0;
    for (usize s = 1; s < MAX_SLOTS_PER_CTRL; ++s) {
        if (m_slots[s].active && m_slots[s].portNum == portNum) {
            targetSlot = static_cast<u8>(s);
            break;
        }
    }

    // Unconditionally teardown active slot on disconnect (matches Linux usb_disconnect)
    if (targetSlot != 0) {
        if (auto* kbd = XhciManager::instance().findKeyboardBySlot(targetSlot)) {
            input::InputManager::instance().unregisterDevice(kbd);
            drivers::DeviceRegistry::instance().unregisterCharDevice(kbd);
            XhciManager::instance().releaseKeyboardDevice(kbd);
        }

        for (usize i = 0; i < XhciManager::MAX_USB_MSC_DEVICES; ++i) {
            auto* msc = XhciManager::instance().getMscDeviceRaw(i);
            if (msc && msc->isActive() && msc->getSlotId() == targetSlot) {
                msc->markDisconnected();
                drivers::storage::StorageManager::instance().onUsbStorageDisconnected(msc);
                XhciManager::instance().releaseMscDevice(msc);
                break;
            }
        }

        teardownSlot(targetSlot);
    }

    if (portNum <= MAX_PORTS_PER_CTRL) {
        m_ports[portNum - 1].clearAttachedDevice();
    }

    safeWritePortsc(portNum, 0, XHCI_PORTSC_CHANGE_BITS);
}

void XhciController::teardownSlot(u8 slotId) noexcept {
    if (slotId == 0 || slotId >= MAX_SLOTS_PER_CTRL) return;

    XhciSlotState& slot = m_slots[slotId];
    if (!slot.active && slot.outputCtxPhys == 0 && slot.inputCtxPhys == 0 && slot.slotId == 0) {
        return;
    }

    XhciTrb disableSlotCmd{};
    disableSlotCmd.control = (static_cast<u32>(XHCI_TRB_DISABLE_SLOT) << 10) |
                             (static_cast<u32>(slotId) << 24);
    XhciTrb comp{};
    (void)submitCommand(disableSlotCmd, &comp, 1000);

    if (slot.outputCtxPhys && m_pmm) {
        m_pmm->freePages(slot.outputCtxPhys, 1);
        slot.outputCtxPhys = 0;
        slot.outputCtxVirt = nullptr;
    }
    if (slot.inputCtxPhys && m_pmm) {
        m_pmm->freePages(slot.inputCtxPhys, 1);
        slot.inputCtxPhys = 0;
        slot.inputCtxVirt = nullptr;
    }

    slot.ep0Ring.reset();
    if (m_dcbaaVirt) {
        m_dcbaaVirt[slotId] = 0;
        arch::memoryBarrier();
    }
    slot.active   = false;
    slot.slotId   = 0;
    slot.portNum  = 0;
    slot.speedId  = 0;

    pr_dbg("xhci", "[usb-bus %u] Slot %u disabled and deallocated", m_busIndex, slotId);
}

void XhciController::markPortChange(u8 portNum) noexcept {
    if (portNum >= 1 && portNum <= m_maxPorts) {
        m_pendingPortMask |= (1U << portNum);
    }
}

void XhciController::pollPortsAndEvents() noexcept {
    if (!m_ready) return;

    m_eventRing.processEvents(this);

    u64 nowMs = arch::ApicController::instance().getUptimeMs();
    if (nowMs - m_lastPortScanMs >= 100) {
        m_lastPortScanMs = nowMs;

        for (u8 port = 1; port <= m_maxPorts; ++port) {
            if (!isPortValid(port)) continue;
            u32 portsc = readPortsc(port);

            bool currentCcs = (portsc & XHCI_PORTSC_CCS) != 0;
            bool lastCcs = (port <= MAX_PORTS_PER_CTRL) ? m_ports[port - 1].isPhysicallyConnected() : false;
            bool cscSet = (portsc & XHCI_PORTSC_CSC) != 0;

            // Follow Linux USB hub logic: only trigger port connect/disconnect handling
            // when the physical connect state transitions (CSC or currentCcs != lastCcs).
            if (cscSet || (currentCcs != lastCcs)) {
                safeWritePortsc(port, 0, XHCI_PORTSC_CHANGE_BITS);
                markPortChange(port);
            } else if (portsc & (XHCI_PORTSC_PRC | XHCI_PORTSC_WRC | XHCI_PORTSC_PLC)) {
                // Clear transient reset/link change bits without re-probing
                safeWritePortsc(port, 0, portsc & (XHCI_PORTSC_PRC | XHCI_PORTSC_WRC | XHCI_PORTSC_PLC));
            }
        }

        // Always clear USBSTS.PCD to unmask hardware port change interrupts / events
        writeOp32(XHCI_OP_USBSTS, XHCI_STS_PCD | XHCI_STS_EINT);
    }

    if (!m_inPortScan && m_pendingPortMask != 0) {
        m_inPortScan = true;
        u32 mask = m_pendingPortMask;
        m_pendingPortMask = 0;

        // Pass 1: Handle USB 3.x ports first so SuperSpeed devices bind before companion USB 2.0 lines
        for (u8 port = 1; port <= m_maxPorts; ++port) {
            if ((mask & (1U << port)) && (port <= MAX_PORTS_PER_CTRL) && m_ports[port - 1].isUsb3()) {
                handlePortStatusChange(port);
            }
        }
        // Pass 2: Handle USB 2.0 / legacy ports
        for (u8 port = 1; port <= m_maxPorts; ++port) {
            if ((mask & (1U << port)) && ((port > MAX_PORTS_PER_CTRL) || !m_ports[port - 1].isUsb3())) {
                handlePortStatusChange(port);
            }
        }
        m_inPortScan = false;
    }
}

void XhciManager::onInterrupt(arch::InterruptFrame*) noexcept {
    for (usize i = 0; i < m_ctrlCount; ++i) {
        if (m_controllers[i].isReady()) {
            m_controllers[i].getEventRing()->processEvents(&m_controllers[i]);
        }
    }
}

UsbKeyboardDevice* XhciManager::allocateKeyboardDevice(u32* outDeviceIndex) noexcept {
    for (usize i = 0; i < MAX_USB_KBD_DEVICES; ++i) {
        if (!m_kbdDevices[i].isActive()) {
            if (outDeviceIndex) *outDeviceIndex = static_cast<u32>(i);
            return &m_kbdDevices[i];
        }
    }
    return nullptr;
}

void XhciManager::releaseKeyboardDevice(UsbKeyboardDevice* dev) noexcept {
    if (!dev) return;
    dev->stop();
}

UsbKeyboardDevice* XhciManager::findKeyboardBySlot(u8 slotId) noexcept {
    for (usize i = 0; i < MAX_USB_KBD_DEVICES; ++i) {
        if (m_kbdDevices[i].isActive() && m_kbdDevices[i].getSlotId() == slotId) {
            return &m_kbdDevices[i];
        }
    }
    return nullptr;
}

void XhciManager::pollEventsAndPorts() noexcept {
    for (usize i = 0; i < m_ctrlCount; ++i) {
        m_controllers[i].pollPortsAndEvents();
    }
    for (usize i = 0; i < MAX_USB_KBD_DEVICES; ++i) {
        if (m_kbdDevices[i].isActive()) {
            m_kbdDevices[i].pollAutorepeat();
        }
    }
}

} // namespace cortez::drivers::usb
