/**
 * @file usb_port.cpp
 * @brief Implementation of UsbPort and UsbPortManager.
 */

#include "usb_port.hpp"
#include "xhci_controller.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::usb {

void UsbPort::init(XhciController* ctrl, u8 busIndex, u8 portNum, u8 majorRev, u8 minorRev) noexcept {
    m_ctrl         = ctrl;
    m_busIndex     = busIndex;
    m_portNum      = portNum;
    m_peerPortNum  = 0;
    m_majorRev     = majorRev;
    m_minorRev     = minorRev;
    m_speedId      = 0;
    m_physConnected = false;
    m_attachedType = UsbAttachedType::None;
    m_slotId       = 0;
    m_vendorName[0]  = '\0';
    m_productName[0] = '\0';
}

const char* UsbPort::getProtocolString() const noexcept {
    if (m_majorRev == 3) {
        if (m_minorRev >= 0x20) return "USB 3.2";
        if (m_minorRev >= 0x10) return "USB 3.1";
        return "USB 3.0";
    }
    if (m_majorRev == 2) return "USB 2.0";
    return "USB";
}

u32 UsbPort::readPortsc() const noexcept {
    if (!m_ctrl) return 0;
    return m_ctrl->readPortsc(m_portNum);
}

void UsbPort::writePortsc(u32 val) noexcept {
    if (!m_ctrl) return;
    m_ctrl->writePortsc(m_portNum, val);
}

void UsbPort::safeWritePortsc(u32 setBits, u32 clearChangeBits) noexcept {
    u32 current = readPortsc();
    // Always preserve Port Power (PP = bit 9).
    // Clear PLS field (bits 5..8) and mask out RW1C bits so uncommanded state transitions are prevented.
    u32 val = (current & ~XHCI_PORTSC_RW1C_MASK & ~XHCI_PORTSC_PLS_MASK) | XHCI_PORTSC_PP;
    val |= setBits;
    val |= (clearChangeBits & XHCI_PORTSC_CHANGE_BITS);
    writePortsc(val);
}

bool UsbPort::isConnected() const noexcept {
    return (readPortsc() & XHCI_PORTSC_CCS) != 0;
}

bool UsbPort::isEnabled() const noexcept {
    return (readPortsc() & XHCI_PORTSC_PED) != 0;
}

bool UsbPort::hasConnectChange() const noexcept {
    return (readPortsc() & XHCI_PORTSC_CSC) != 0;
}

UsbPortSpeed UsbPort::getSpeed() const noexcept {
    u32 portsc = readPortsc();
    u8 spd = static_cast<u8>((portsc >> 10) & 0x0FU);
    switch (spd) {
        case XHCI_SPEED_FULL:       return UsbPortSpeed::FullSpeed;
        case XHCI_SPEED_LOW:        return UsbPortSpeed::LowSpeed;
        case XHCI_SPEED_HIGH:       return UsbPortSpeed::HighSpeed;
        case XHCI_SPEED_SUPER:      return UsbPortSpeed::Super;
        case XHCI_SPEED_SUPER_PLUS: return UsbPortSpeed::SuperPlus;
        default:                    return UsbPortSpeed::Unknown;
    }
}

const char* UsbPort::getSpeedString() const noexcept {
    switch (getSpeed()) {
        case UsbPortSpeed::LowSpeed:  return "USB 1.0 (Low-Speed, 1.5 Mbps)";
        case UsbPortSpeed::FullSpeed: return "USB 1.1 (Full-Speed, 12 Mbps)";
        case UsbPortSpeed::HighSpeed: return "USB 2.0 (High-Speed, 480 Mbps)";
        case UsbPortSpeed::Super:     return "USB 3.0 (SuperSpeed, 5 Gbps)";
        case UsbPortSpeed::SuperPlus: return "USB 3.1 (SuperSpeed+, 10 Gbps)";
        default:                      return "USB (Unknown Speed)";
    }
}

void UsbPort::powerOn() noexcept {
    safeWritePortsc(XHCI_PORTSC_PP, 0);
}

void UsbPort::powerOff() noexcept {
    u32 current = readPortsc();
    u32 val = (current & ~XHCI_PORTSC_RW1C_MASK & ~XHCI_PORTSC_PLS_MASK & ~XHCI_PORTSC_PP);
    writePortsc(val);
}

void UsbPort::clearChangeBits() noexcept {
    safeWritePortsc(0, XHCI_PORTSC_CHANGE_BITS);
}

bool UsbPort::reset(u8* outSpeedId) noexcept {
    if (!m_ctrl) return false;
    bool ok = m_ctrl->resetPort(m_portNum, &m_speedId);
    if (outSpeedId) *outSpeedId = m_speedId;
    return ok;
}

bool UsbPort::warmReset() noexcept {
    if (!isUsb3()) return false;
    safeWritePortsc(XHCI_PORTSC_WPR, 0);

    for (u32 ms = 0; ms < 500; ++ms) {
        arch::ApicController::instance().sleepMs(1);
        u32 portsc = readPortsc();
        if ((portsc & (XHCI_PORTSC_WRC | XHCI_PORTSC_PRC)) || (portsc & XHCI_PORTSC_PED)) {
            break;
        }
    }
    clearChangeBits();
    return (readPortsc() & XHCI_PORTSC_PED) != 0;
}

void UsbPort::setAttachedDevice(UsbAttachedType type,
                                u8 slotId,
                                const char* vendor,
                                const char* product) noexcept {
    m_attachedType = type;
    m_slotId       = slotId;
    if (vendor) {
        str::copy(m_vendorName, vendor, sizeof(m_vendorName));
    } else {
        m_vendorName[0] = '\0';
    }
    if (product) {
        str::copy(m_productName, product, sizeof(m_productName));
    } else {
        m_productName[0] = '\0';
    }

    const char* typeStr = getAttachedTypeString();
    pr_dbg("usb-port", "[bus %u] Port %u (%s, Slot %u): Bound %s ['%s' '%s']",
           m_busIndex, m_portNum, getProtocolString(), slotId, typeStr, m_vendorName, m_productName);
}

const char* UsbPort::getAttachedTypeString() const noexcept {
    switch (m_attachedType) {
        case UsbAttachedType::Keyboard:       return "Keyboard";
        case UsbAttachedType::MassStorage:    return "Mass Storage";
        case UsbAttachedType::MouseOrTouch:   return "HID (Mouse/Touchpad)";
        case UsbAttachedType::Video:          return "Webcam / Video Device";
        case UsbAttachedType::Bluetooth:      return "Bluetooth Controller";
        case UsbAttachedType::Audio:          return "USB Audio Device";
        case UsbAttachedType::Hub:            return "USB Hub";
        case UsbAttachedType::VendorSpecific: return "Vendor-Specific Device";
        case UsbAttachedType::Unsupported:    return "Unsupported Device";
        case UsbAttachedType::None:           return "Disconnected (empty)";
        default:                              return "Unknown";
    }
}

void UsbPort::clearAttachedDevice() noexcept {
    if (m_attachedType != UsbAttachedType::None) {
        pr_dbg("usb-port", "[bus %u] Port %u (Slot %u): Device detached (%s) ['%s' '%s']",
               m_busIndex, m_portNum, m_slotId, getAttachedTypeString(), m_vendorName, m_productName);
    }
    m_attachedType   = UsbAttachedType::None;
    m_slotId         = 0;
    m_vendorName[0]  = '\0';
    m_productName[0] = '\0';
}

// =============================================================================
// UsbPortManager Implementation
// =============================================================================

UsbPortManager& UsbPortManager::instance() noexcept {
    static UsbPortManager s_mgr;
    return s_mgr;
}

Status UsbPortManager::registerPort(UsbPort* port) noexcept {
    if (!port) return Status::InvalidArgument;
    if (m_portCount >= MAX_MANAGED_PORTS) return Status::NoSpace;
    m_ports[m_portCount++] = port;
    return Status::Ok;
}

UsbPort* UsbPortManager::getPort(usize idx) noexcept {
    if (idx >= m_portCount) return nullptr;
    return m_ports[idx];
}

UsbPort* UsbPortManager::findPort(XhciController* ctrl, u8 portNum) noexcept {
    for (usize i = 0; i < m_portCount; ++i) {
        if (m_ports[i] && m_ports[i]->getController() == ctrl && m_ports[i]->getPortNum() == portNum) {
            return m_ports[i];
        }
    }
    return nullptr;
}

UsbPort* UsbPortManager::findPortBySlot(XhciController* ctrl, u8 slotId) noexcept {
    for (usize i = 0; i < m_portCount; ++i) {
        if (m_ports[i] && m_ports[i]->getController() == ctrl && m_ports[i]->getSlotId() == slotId) {
            return m_ports[i];
        }
    }
    return nullptr;
}

UsbPort* UsbPortManager::findPortByBus(u8 busIndex, u8 portNum) noexcept {
    for (usize i = 0; i < m_portCount; ++i) {
        if (m_ports[i] && m_ports[i]->getBusIndex() == busIndex && m_ports[i]->getPortNum() == portNum) {
            return m_ports[i];
        }
    }
    return nullptr;
}

} // namespace cortez::drivers::usb
