/**
 * @file usb_port.hpp
 * @brief Dedicated USB Port Driver and Port Manager for USB 2.0 & USB 3.x physical interfaces.
 */

#pragma once

#include "xhci_regs.hpp"
#include "../../lib/types.hpp"
#include "../../lib/result.hpp"

namespace cortez::drivers::usb {

class XhciController;

enum class UsbPortSpeed : u8 {
    Unknown   = 0,
    FullSpeed = 1, // 12 Mbps (USB 1.1)
    LowSpeed  = 2, // 1.5 Mbps (USB 1.0)
    HighSpeed = 3, // 480 Mbps (USB 2.0)
    Super     = 4, // 5 Gbps (USB 3.0 Gen 1)
    SuperPlus = 5  // 10 Gbps (USB 3.1 Gen 2)
};

enum class UsbAttachedType : u8 {
    None,            // Empty / Disconnected
    Keyboard,        // USB HID Keyboard
    MassStorage,     // USB Bulk-Only Mass Storage
    MouseOrTouch,    // USB Mouse / Touchpad / Digitizer (HID)
    Video,           // USB Video Class (Integrated Webcam)
    Bluetooth,       // Wireless Bluetooth Controller
    Audio,           // USB Audio Device
    Hub,             // USB Hub
    VendorSpecific,  // Fingerprint sensor, vendor controller
    Unsupported      // Other unrecognized USB class
};

/**
 * @brief Encapsulates a single physical USB Root Hub Port (USB 2.0 or USB 3.x).
 */
class UsbPort final {
public:
    UsbPort() noexcept = default;

    void init(XhciController* ctrl, u8 busIndex, u8 portNum, u8 majorRev, u8 minorRev) noexcept;

    u8              getPortNum() const noexcept { return m_portNum; }
    u8              getBusIndex() const noexcept { return m_busIndex; }
    u8              getPeerPortNum() const noexcept { return m_peerPortNum; }
    void            setPeerPortNum(u8 peer) noexcept { m_peerPortNum = peer; }
    bool            isUsb3() const noexcept { return m_majorRev >= 3; }
    u8              getMajorRev() const noexcept { return m_majorRev; }
    u8              getMinorRev() const noexcept { return m_minorRev; }
    const char*     getProtocolString() const noexcept;
    XhciController* getController() const noexcept { return m_ctrl; }

    bool            isConnected() const noexcept;
    bool            isEnabled() const noexcept;
    bool            hasConnectChange() const noexcept;
    bool            isPhysicallyConnected() const noexcept { return m_physConnected; }
    void            setPhysicallyConnected(bool v) noexcept { m_physConnected = v; }
    UsbPortSpeed    getSpeed() const noexcept;
    u8              getSpeedId() const noexcept { return m_speedId; }
    const char*     getSpeedString() const noexcept;

    UsbAttachedType getAttachedType() const noexcept { return m_attachedType; }
    bool            hasAttachedDevice() const noexcept { return m_attachedType != UsbAttachedType::None; }
    const char*     getAttachedTypeString() const noexcept;
    u8              getSlotId() const noexcept { return m_slotId; }
    const char*     getVendorName() const noexcept { return m_vendorName; }
    const char*     getProductName() const noexcept { return m_productName; }

    // Physical port operations
    void powerOn() noexcept;
    void powerOff() noexcept;
    bool reset(u8* outSpeedId = nullptr) noexcept;
    bool warmReset() noexcept;
    void clearChangeBits() noexcept;

    u32  readPortsc() const noexcept;
    void writePortsc(u32 val) noexcept;
    void safeWritePortsc(u32 setBits = 0, u32 clearChangeBits = 0) noexcept;

    // Attached device lifecycle management
    void setAttachedDevice(UsbAttachedType type,
                           u8 slotId,
                           const char* vendor,
                           const char* product) noexcept;
    void clearAttachedDevice() noexcept;

private:
    XhciController* m_ctrl         = nullptr;
    u8              m_busIndex     = 0;
    u8              m_portNum      = 0;
    u8              m_peerPortNum  = 0;
    u8              m_majorRev     = 2;
    u8              m_minorRev     = 0;
    u8              m_speedId      = 0;
    bool            m_physConnected = false;
    UsbAttachedType m_attachedType = UsbAttachedType::None;
    u8              m_slotId       = 0;
    char            m_vendorName[16]{};
    char            m_productName[32]{};
};

/**
 * @brief Global USB Port Manager orchestrating all physical root hub ports across controllers.
 */
class UsbPortManager final {
public:
    static constexpr usize MAX_MANAGED_PORTS = 64;

    static UsbPortManager& instance() noexcept;

    void     clear() noexcept { m_portCount = 0; }
    Status   registerPort(UsbPort* port) noexcept;
    UsbPort* getPort(usize idx) noexcept;
    UsbPort* findPort(XhciController* ctrl, u8 portNum) noexcept;
    UsbPort* findPortBySlot(XhciController* ctrl, u8 slotId) noexcept;
    UsbPort* findPortByBus(u8 busIndex, u8 portNum) noexcept;
    usize    getPortCount() const noexcept { return m_portCount; }

private:
    UsbPortManager() noexcept = default;

    UsbPort* m_ports[MAX_MANAGED_PORTS]{};
    usize    m_portCount = 0;
};

} // namespace cortez::drivers::usb
