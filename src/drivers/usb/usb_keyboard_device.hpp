/**
 * @file usb_keyboard_device.hpp
 * @brief USB HID Boot Keyboard driver implementing ICharDevice with 6KRO decoding and LED control.
 */

#pragma once

#include "usb_hid_regs.hpp"
#include "xhci_regs.hpp"
#include "xhci_rings.hpp"
#include "../i_device.hpp"
#include "../input/ring_buffer.hpp"
#include "../../lib/spinlock.hpp"
#include "../../memory/i_memory.hpp"

namespace cortez::drivers::usb {

class XhciController;

class UsbKeyboardDevice final : public ICharDevice {
public:
    static constexpr usize BUFFER_CAPACITY = 512;

    UsbKeyboardDevice() noexcept = default;

    Status init(XhciController* ctrl,
                u32 deviceIndex,
                u8 slotId,
                u8 portNum,
                u8 speedId,
                u8 interfaceNum,
                u8 intrInDci,
                u8 intrInEpAddr,
                u16 maxPacketSize,
                memory::IPhysicalAllocator* pmm,
                u64 hhdmOffset) noexcept;

    void stop() noexcept;

    // ICharDevice overrides
    const char*   getName() const noexcept override { return m_name; }
    Result<usize> read(void* buffer, usize count) override;
    Result<usize> write(const void*, usize) override { return Status::NotSupported; }
    bool          hasData() const noexcept override;

    // Event & Transfer Handling
    void handleTransferEvent(const XhciTrb& trb) noexcept;
    void startTransfer() noexcept;
    void pollAutorepeat() noexcept;

    bool isActive() const noexcept { return m_active; }
    u8   getSlotId() const noexcept { return m_slotId; }
    u8   getPortNum() const noexcept { return m_portNum; }
    u8   getIntrInDci() const noexcept { return m_intrInDci; }
    XhciTrbRing& getIntrRing() noexcept { return m_intrRing; }

private:
    void processReport(const UsbHidBootKeyboardReport& report) noexcept;
    void updateLeds() noexcept;
    void pushKeyChar(char ch, bool ctrlHeld, bool altHeld) noexcept;
    void pushSequence(const char* seq) noexcept;

    XhciController*             m_ctrl          = nullptr;
    u32                         m_deviceIndex   = 0;
    u8                          m_slotId        = 0;
    u8                          m_portNum       = 0;
    u8                          m_speedId       = 0;
    u8                          m_interfaceNum  = 0;
    u8                          m_intrInDci     = 0;
    u8                          m_intrInEpAddr  = 0;
    u16                         m_maxPacketSize = 8;
    bool                        m_active        = false;
    char                        m_name[16]{};

    memory::IPhysicalAllocator* m_pmm           = nullptr;
    u64                         m_hhdmOffset    = 0;

    XhciTrbRing                 m_intrRing{};
    phys_addr_t                 m_dmaPhys       = 0;
    u8*                         m_dmaVirt       = nullptr;

    Spinlock                                m_lock;
    input::RingBuffer<char, BUFFER_CAPACITY> m_buffer{};

    UsbHidBootKeyboardReport                m_prevReport{};
    bool                                    m_capsLock      = false;
    bool                                    m_numLock       = true;
    bool                                    m_scrollLock    = false;

    // Autorepeat state
    u8                                      m_lastRepeatKey = 0;
    u64                                     m_repeatStartTimeMs = 0;
    u64                                     m_nextRepeatTimeMs  = 0;
};

} // namespace cortez::drivers::usb
