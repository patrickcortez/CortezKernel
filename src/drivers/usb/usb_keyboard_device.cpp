/**
 * @file usb_keyboard_device.cpp
 * @brief Implementation of UsbKeyboardDevice.
 */

#include "usb_keyboard_device.hpp"
#include "xhci_controller.hpp"
#include "../input/input_manager.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../klibc/klibc.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::usb {

Status UsbKeyboardDevice::init(XhciController* ctrl,
                               u32 deviceIndex,
                               u8 slotId,
                               u8 portNum,
                               u8 speedId,
                               u8 interfaceNum,
                               u8 intrInDci,
                               u8 intrInEpAddr,
                               u16 maxPacketSize,
                               memory::IPhysicalAllocator* pmm,
                               u64 hhdmOffset) noexcept {
    if (!ctrl || !pmm) return Status::InvalidArgument;

    m_ctrl          = ctrl;
    m_deviceIndex   = deviceIndex;
    m_slotId        = slotId;
    m_portNum       = portNum;
    m_speedId       = speedId;
    m_interfaceNum  = interfaceNum;
    m_intrInDci     = intrInDci;
    m_intrInEpAddr  = intrInEpAddr;
    m_maxPacketSize = maxPacketSize;
    m_pmm           = pmm;
    m_hhdmOffset    = hhdmOffset;
    m_active        = false;

    klibc_snprintf(m_name, sizeof(m_name), "ukbd%u", deviceIndex);

    // 1. Allocate DMA buffer for 8-byte reports
    auto pageRes = pmm->allocPages(1);
    if (pageRes.isError()) return pageRes.status();
    m_dmaPhys = pageRes.value();
    m_dmaVirt = reinterpret_cast<u8*>(m_dmaPhys + hhdmOffset);
    memset(m_dmaVirt, 0, PAGE_SIZE_4K);

    // 2. Initialize Interrupt IN TRB ring
    Status st = m_intrRing.init(pmm, hhdmOffset);
    if (st != Status::Ok) {
        pmm->freePages(m_dmaPhys, 1);
        return st;
    }

    // 3. Send SET_PROTOCOL(0) (Boot Protocol)
    UsbSetupPacket setProto{};
    setProto.bmRequestType = 0x21U; // Host-to-Device | Class | Interface
    setProto.bRequest      = HID_REQ_SET_PROTOCOL;
    setProto.wValue        = HID_PROTOCOL_BOOT;
    setProto.wIndex        = static_cast<u16>(m_interfaceNum);
    setProto.wLength       = 0;
    (void)m_ctrl->controlTransfer(m_slotId, setProto, nullptr, 0, false);

    // 4. Send SET_IDLE(0) (Report only on state change)
    UsbSetupPacket setIdle{};
    setIdle.bmRequestType = 0x21U;
    setIdle.bRequest      = HID_REQ_SET_IDLE;
    setIdle.wValue        = 0;
    setIdle.wIndex        = static_cast<u16>(m_interfaceNum);
    setIdle.wLength       = 0;
    (void)m_ctrl->controlTransfer(m_slotId, setIdle, nullptr, 0, false);

    memset(&m_prevReport, 0, sizeof(m_prevReport));
    m_capsLock   = false;
    m_numLock    = true;
    m_scrollLock = false;

    // 5. Set initial LEDs (Num Lock on)
    updateLeds();

    m_active = true;

    // 6. Submit first Interrupt Transfer TRB
    startTransfer();

    pr_dbg("usb-kbd", "Initialized USB Keyboard '%s' (Slot %u, Port %u, DCI %u)",
          m_name, m_slotId, m_portNum, m_intrInDci);
    return Status::Ok;
}

void UsbKeyboardDevice::stop() noexcept {
    m_active = false;
    m_lastRepeatKey = 0;
    m_repeatStartTimeMs = 0;
    m_nextRepeatTimeMs = 0;
    m_buffer.clear();

    if (m_ctrl && m_slotId != 0 && m_intrInDci != 0) {
        // Stop endpoint
        XhciTrb stopCmd{};
        stopCmd.control = (static_cast<u32>(XHCI_TRB_STOP_EP) << 10) |
                          (static_cast<u32>(m_intrInDci) << 16) |
                          (static_cast<u32>(m_slotId) << 24);
        XhciTrb comp{};
        (void)m_ctrl->submitCommand(stopCmd, &comp, 1000);
    }

    if (m_dmaPhys != 0 && m_pmm) {
        m_pmm->freePages(m_dmaPhys, 1);
        m_dmaPhys = 0;
        m_dmaVirt = nullptr;
    }

    m_slotId = 0;
    m_portNum = 0;
    m_intrInDci = 0;
}

void UsbKeyboardDevice::startTransfer() noexcept {
    if (!m_active || !m_ctrl || m_dmaPhys == 0) return;

    XhciTrb trb{};
    trb.param   = m_dmaPhys;
    trb.status  = 8U; // 8 bytes for Boot Keyboard Report
    trb.control = (static_cast<u32>(XHCI_TRB_NORMAL) << 10) |
                  XHCI_TRB_IOC |
                  XHCI_TRB_ISP;

    m_intrRing.enqueue(trb);
    m_ctrl->ringDoorbell(m_slotId, m_intrInDci);
}

void UsbKeyboardDevice::updateLeds() noexcept {
    if (!m_ctrl || m_slotId == 0) return;

    u8 ledByte = 0;
    if (m_numLock)    ledByte |= HID_LED_NUM_LOCK;
    if (m_capsLock)   ledByte |= HID_LED_CAPS_LOCK;
    if (m_scrollLock) ledByte |= HID_LED_SCROLL_LOCK;

    UsbSetupPacket setReport{};
    setReport.bmRequestType = 0x21U;
    setReport.bRequest      = HID_REQ_SET_REPORT;
    setReport.wValue        = static_cast<u16>((HID_REPORT_TYPE_OUTPUT << 8) | 0U);
    setReport.wIndex        = static_cast<u16>(m_interfaceNum);
    setReport.wLength       = 1;

    (void)m_ctrl->controlTransfer(m_slotId, setReport, &ledByte, 1, false);
}

void UsbKeyboardDevice::pushKeyChar(char ch, bool ctrlHeld, bool altHeld) noexcept {
    if (ch == 0) return;

    // Apply Caps Lock to alphabetic characters
    if (m_capsLock) {
        if (ch >= 'a' && ch <= 'z') {
            ch = str::toUpper(ch);
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = str::toLower(ch);
        }
    }

    if (ctrlHeld) {
        char lower = str::toLower(ch);
        if (lower >= 'a' && lower <= 'z') {
            char code = static_cast<char>(lower - 'a' + 1);
            IrqLockGuard guard(m_lock);
            m_buffer.push(code);
            return;
        }
    }

    if (altHeld) {
        IrqLockGuard guard(m_lock);
        m_buffer.push('\033');
        m_buffer.push(ch);
        return;
    }

    IrqLockGuard guard(m_lock);
    m_buffer.push(ch);
}

void UsbKeyboardDevice::pushSequence(const char* seq) noexcept {
    if (!seq) return;
    IrqLockGuard guard(m_lock);
    const char* p = seq;
    while (*p) {
        m_buffer.push(*p++);
    }
}

void UsbKeyboardDevice::processReport(const UsbHidBootKeyboardReport& report) noexcept {
    // 1. Check for ErrorRollOver
    for (usize i = 0; i < 6; ++i) {
        if (report.keycodes[i] == HID_KEY_ERROR_ROLLOVER) {
            return; // Ignore ghosting
        }
    }

    bool shiftHeld = (report.modifiers & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0;
    bool ctrlHeld  = (report.modifiers & (HID_MOD_LCTRL  | HID_MOD_RCTRL))  != 0;
    bool altHeld   = (report.modifiers & (HID_MOD_LALT   | HID_MOD_RALT))   != 0;

    u64 nowMs = arch::ApicController::instance().getUptimeMs();

    // 2. Identify new keys in report that were not in prevReport
    for (usize i = 0; i < 6; ++i) {
        u8 key = report.keycodes[i];
        if (key == 0) continue;

        bool wasPressed = false;
        for (usize j = 0; j < 6; ++j) {
            if (m_prevReport.keycodes[j] == key) {
                wasPressed = true;
                break;
            }
        }

        if (!wasPressed) {
            // New key press
            if (key == 0x39) { // Caps Lock toggle
                m_capsLock = !m_capsLock;
                updateLeds();
                continue;
            }
            if (key == 0x53) { // Num Lock toggle
                m_numLock = !m_numLock;
                updateLeds();
                continue;
            }

            const char* ansi = getHidAnsiSequence(key);
            if (ansi) {
                pushSequence(ansi);
            } else {
                char ch = shiftHeld ? kHidUsageToAsciiShifted[key] : kHidUsageToAsciiNormal[key];
                pushKeyChar(ch, ctrlHeld, altHeld);
            }

            // Set up autorepeat for this key
            m_lastRepeatKey     = key;
            m_repeatStartTimeMs = nowMs + 500;
            m_nextRepeatTimeMs  = m_repeatStartTimeMs;
        }
    }

    // 3. Clear repeat key if no longer held
    if (m_lastRepeatKey != 0) {
        bool stillHeld = false;
        for (usize i = 0; i < 6; ++i) {
            if (report.keycodes[i] == m_lastRepeatKey) {
                stillHeld = true;
                break;
            }
        }
        if (!stillHeld) {
            m_lastRepeatKey = 0;
        }
    }

    m_prevReport = report;
}

void UsbKeyboardDevice::pollAutorepeat() noexcept {
    if (!m_active || m_lastRepeatKey == 0) return;

    u64 nowMs = arch::ApicController::instance().getUptimeMs();
    if (nowMs >= m_nextRepeatTimeMs) {
        bool shiftHeld = (m_prevReport.modifiers & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0;
        bool ctrlHeld  = (m_prevReport.modifiers & (HID_MOD_LCTRL  | HID_MOD_RCTRL))  != 0;
        bool altHeld   = (m_prevReport.modifiers & (HID_MOD_LALT   | HID_MOD_RALT))   != 0;

        const char* ansi = getHidAnsiSequence(m_lastRepeatKey);
        if (ansi) {
            pushSequence(ansi);
        } else {
            char ch = shiftHeld ? kHidUsageToAsciiShifted[m_lastRepeatKey] : kHidUsageToAsciiNormal[m_lastRepeatKey];
            pushKeyChar(ch, ctrlHeld, altHeld);
        }

        m_nextRepeatTimeMs = nowMs + 33; // ~30 Hz repeat rate
    }
}

void UsbKeyboardDevice::handleTransferEvent(const XhciTrb& trb) noexcept {
    u8 cc = trb.getCompletionCode();
    if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
        UsbHidBootKeyboardReport report{};
        memcpy(&report, m_dmaVirt, sizeof(report));
        processReport(report);
    }
    // Re-arm transfer for the next interrupt report
    startTransfer();
}

bool UsbKeyboardDevice::hasData() const noexcept {
    return !m_buffer.isEmpty();
}

Result<usize> UsbKeyboardDevice::read(void* buffer, usize count) {
    if (!buffer || count == 0) return 0;
    auto* out = static_cast<char*>(buffer);

    IrqLockGuard guard(m_lock);
    usize bytesRead = 0;
    while (bytesRead < count) {
        char c = 0;
        if (!m_buffer.pop(&c)) {
            break;
        }
        out[bytesRead++] = c;
    }
    return bytesRead;
}

} // namespace cortez::drivers::usb
