/**
 * @file xhci_controller.hpp
 * @brief PCIe xHCI (USB 3.x / 2.0) Host Controller and Multi-Controller USB Manager.
 */

#pragma once

#include "xhci_regs.hpp"
#include "xhci_rings.hpp"
#include "usb_msc_device.hpp"
#include "../../arch/x86_64/pcie.hpp"
#include "../../arch/x86_64/idt.hpp"
#include "../../memory/i_memory.hpp"
#include "../../lib/spinlock.hpp"

namespace cortez::drivers::usb {

/**
 * @brief Per-slot USB device state managed by an XhciController.
 */
struct XhciSlotState {
    bool        active          = false;
    u8          slotId          = 0;
    u8          portNum         = 0;
    u8          speedId         = 0;
    u16         maxPacketSize0  = 64;
    phys_addr_t outputCtxPhys   = 0;
    u8*         outputCtxVirt   = nullptr;
    phys_addr_t inputCtxPhys    = 0;
    u8*         inputCtxVirt    = nullptr;
    XhciTrbRing ep0Ring{};
};

/**
 * @brief Represents a single physical PCIe xHCI Host Controller (0C:03:30).
 */
class XhciController final {
public:
    static constexpr usize MAX_SLOTS_PER_CTRL = 16;

    XhciController() noexcept = default;

    Status init(const arch::PciDeviceInfo& pciDev,
                memory::IPhysicalAllocator* pmm,
                memory::IVirtualMapper* vmm,
                u64 hhdmOffset) noexcept;

    /**
     * @brief Executes a synchronous USB Control Transfer on Endpoint 0 (DCI 1).
     */
    Status controlTransfer(u8 slotId,
                           const UsbSetupPacket& setup,
                           void* dataBuffer,
                           u16 length,
                           bool isDataIn) noexcept;

    /**
     * @brief Executes a synchronous USB Bulk Transfer on the specified Endpoint DCI.
     */
    Status bulkTransfer(u8 slotId,
                        u8 dci,
                        u8 epAddr,
                        XhciTrbRing& ring,
                        phys_addr_t dmaPhys,
                        u32 length,
                        bool isDataIn) noexcept;

    /**
     * @brief Recovers a stalled Bulk endpoint via Reset Endpoint + Set TR Dequeue + CLEAR_FEATURE.
     */
    Status clearEndpointStall(u8 slotId, u8 dci, u8 epAddr, XhciTrbRing& ring) noexcept;

    bool                      isReady() const noexcept { return m_ready; }
    const arch::PciDeviceInfo& getPciInfo() const noexcept { return m_pciDev; }
    u16                       getHciVersion() const noexcept { return m_hciVersion; }
    u8                        getMaxPorts() const noexcept { return m_maxPorts; }
    u8                        getMaxSlots() const noexcept { return m_maxSlots; }

private:
    u32  readCap32(u32 offset) const noexcept;
    u32  readOp32(u32 offset) const noexcept;
    void writeOp32(u32 offset, u32 value) noexcept;
    u64  readOp64(u32 offset) const noexcept;
    void writeOp64(u32 offset, u64 value) noexcept;
    u32  readPortsc(u8 portNum) const noexcept;
    void writePortsc(u8 portNum, u32 value) noexcept;
    void ringDoorbell(u8 slotId, u32 target) noexcept;

    void   performBiosHandoff() noexcept;
    void   enableIntelPortRouting() noexcept;
    bool   waitOpBit(u32 regOffset, u32 mask, u32 expected, u32 timeoutMs) const noexcept;
    Status setupDcbaaAndScratchpad() noexcept;
    Status submitCommand(const XhciTrb& cmdTrb, XhciTrb* outCompletion, u32 timeoutMs = 3000) noexcept;

    void   probePorts() noexcept;
    bool   resetPort(u8 portNum, u8* outSpeedId) noexcept;
    Status enumeratePortDevice(u8 portNum, u8 speedId) noexcept;

    u8*  getInputControlCtx(XhciSlotState& slot) const noexcept;
    u8*  getInputSlotCtx(XhciSlotState& slot) const noexcept;
    u8*  getInputEpCtx(XhciSlotState& slot, u8 dci) const noexcept;
    u8*  getOutputSlotCtx(XhciSlotState& slot) const noexcept;
    u8*  getOutputEpCtx(XhciSlotState& slot, u8 dci) const noexcept;

    Spinlock                    m_ctrlLock;
    arch::PciDeviceInfo         m_pciDev{};
    memory::IPhysicalAllocator* m_pmm          = nullptr;
    memory::IVirtualMapper*     m_vmm          = nullptr;
    u64                         m_hhdmOffset   = 0;

    volatile u8*                m_capBaseVirt  = nullptr;
    volatile u8*                m_opBaseVirt   = nullptr;
    volatile u8*                m_rtBaseVirt   = nullptr;
    volatile u32*               m_dbBaseVirt   = nullptr;

    u8                          m_capLength    = 0;
    u16                         m_hciVersion   = 0;
    u8                          m_maxSlots     = 0;
    u8                          m_maxPorts     = 0;
    usize                       m_contextSize  = 32;
    bool                        m_ready        = false;

    phys_addr_t                 m_dcbaaPhys    = 0;
    u64*                        m_dcbaaVirt    = nullptr;
    phys_addr_t                 m_ep0DmaPhys   = 0;
    u8*                         m_ep0DmaVirt   = nullptr;

    XhciTrbRing                 m_cmdRing{};
    XhciEventRing               m_eventRing{};
    XhciSlotState               m_slots[MAX_SLOTS_PER_CTRL]{};
};

/**
 * @brief Global USB / xHCI Manager that probes all PCIe xHCI controllers and manages
 *        attached USB Mass Storage block devices.
 */
class XhciManager final : public arch::IInterruptHandler {
public:
    static constexpr usize MAX_XHCI_CONTROLLERS = 4;
    static constexpr usize MAX_USB_MSC_DEVICES  = 8;

    static XhciManager& instance() noexcept;

    Status init(memory::IPhysicalAllocator* pmm,
                memory::IVirtualMapper* vmm,
                u64 hhdmOffset) noexcept;

    UsbMscDevice* allocateMscDevice(u32* outDeviceIndex) noexcept;

    void onInterrupt(arch::InterruptFrame* frame) noexcept override;

    usize                 getControllerCount() const noexcept { return m_ctrlCount; }
    const XhciController* getController(usize idx) const noexcept {
        return (idx < m_ctrlCount) ? &m_controllers[idx] : nullptr;
    }

    usize         getMscDeviceCount() const noexcept { return m_mscCount; }
    UsbMscDevice* getMscDevice(usize idx) noexcept {
        return (idx < m_mscCount) ? &m_mscDevices[idx] : nullptr;
    }

    static const char* speedToString(u8 speedId) noexcept;

private:
    XhciManager() noexcept = default;

    XhciController m_controllers[MAX_XHCI_CONTROLLERS]{};
    usize          m_ctrlCount = 0;

    UsbMscDevice   m_mscDevices[MAX_USB_MSC_DEVICES]{};
    usize          m_mscCount  = 0;
};

} // namespace cortez::drivers::usb
