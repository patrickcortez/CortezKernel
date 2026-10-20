/**
 * @file usb_hid_regs.hpp
 * @brief USB Human Interface Device (HID) 1.11 Boot Protocol definitions, requests,
 *        report structures, and HID usage scancode translation tables.
 */

#pragma once

#include "../../lib/types.hpp"

namespace cortez::drivers::usb {

// =============================================================================
// USB HID Class-Specific Descriptor Types
// =============================================================================
constexpr u8 USB_DESC_HID      = 0x21;
constexpr u8 USB_DESC_REPORT   = 0x22;
constexpr u8 USB_DESC_PHYSICAL = 0x23;

// =============================================================================
// USB HID Class-Specific Request Codes (bmRequestType = 0x21 or 0xA1)
// =============================================================================
constexpr u8 HID_REQ_GET_REPORT   = 0x01;
constexpr u8 HID_REQ_GET_IDLE     = 0x02;
constexpr u8 HID_REQ_GET_PROTOCOL = 0x03;
constexpr u8 HID_REQ_SET_REPORT   = 0x09;
constexpr u8 HID_REQ_SET_IDLE     = 0x0A;
constexpr u8 HID_REQ_SET_PROTOCOL = 0x0B;

// HID Report Types (wValue high byte in GET_REPORT / SET_REPORT)
constexpr u8 HID_REPORT_TYPE_INPUT   = 0x01;
constexpr u8 HID_REPORT_TYPE_OUTPUT  = 0x02;
constexpr u8 HID_REPORT_TYPE_FEATURE = 0x03;

// HID Protocol Values (wValue in SET_PROTOCOL)
constexpr u16 HID_PROTOCOL_BOOT   = 0x0000;
constexpr u16 HID_PROTOCOL_REPORT = 0x0001;

// =============================================================================
// USB HID Boot Keyboard Modifier Bits (Byte 0 of Boot Report)
// =============================================================================
constexpr u8 HID_MOD_LCTRL  = (1U << 0);
constexpr u8 HID_MOD_LSHIFT = (1U << 1);
constexpr u8 HID_MOD_LALT   = (1U << 2);
constexpr u8 HID_MOD_LGUI   = (1U << 3);
constexpr u8 HID_MOD_RCTRL  = (1U << 4);
constexpr u8 HID_MOD_RSHIFT = (1U << 5);
constexpr u8 HID_MOD_RALT   = (1U << 6);
constexpr u8 HID_MOD_RGUI   = (1U << 7);

// =============================================================================
// USB HID Keyboard LED Output Report Bits (Byte 0 of Output Report)
// =============================================================================
constexpr u8 HID_LED_NUM_LOCK    = (1U << 0);
constexpr u8 HID_LED_CAPS_LOCK   = (1U << 1);
constexpr u8 HID_LED_SCROLL_LOCK = (1U << 2);
constexpr u8 HID_LED_COMPOSE     = (1U << 3);
constexpr u8 HID_LED_KANA        = (1U << 4);

// =============================================================================
// Standard 8-byte USB HID Boot Keyboard Input Report
// =============================================================================
struct CORTEZ_PACKED UsbHidBootKeyboardReport {
    u8 modifiers;   // Bitmask of active modifier keys (HID_MOD_*)
    u8 reserved;    // Reserved by USB spec (0x00)
    u8 keycodes[6]; // Up to 6 concurrently pressed USB HID Usage IDs
};
static_assert(sizeof(UsbHidBootKeyboardReport) == 8, "UsbHidBootKeyboardReport must be exactly 8 bytes");

// Special HID Keyboard Usages
constexpr u8 HID_KEY_RESERVED      = 0x00;
constexpr u8 HID_KEY_ERROR_ROLLOVER= 0x01;
constexpr u8 HID_KEY_POST_FAIL     = 0x02;
constexpr u8 HID_KEY_ERROR_UNDEF   = 0x03;

// =============================================================================
// HID Usage ID (Page 0x07) to ASCII Normal / Shifted Translation Tables
// =============================================================================
inline constexpr char kHidUsageToAsciiNormal[256] = {
    // 0x00 - 0x03: Special
    0, 0, 0, 0,
    // 0x04 - 0x1D: Letters a - z
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    // 0x1E - 0x27: Digits 1 - 9, 0
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    // 0x28 - 0x2C: Enter, Escape, Backspace, Tab, Space
    '\n', '\033', '\b', '\t', ' ',
    // 0x2D - 0x38: Punctuation: -, =, [, ], \, Non-US#, ;, ', `, ,, ., /
    '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/',
    // 0x39: Caps Lock (handled dynamically)
    0,
    // 0x3A - 0x45: F1 - F12
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x46 - 0x48: PrintScreen, ScrollLock, Pause
    0, 0, 0,
    // 0x49 - 0x4E: Insert, Home, PageUp, Delete, End, PageDown (handled via ANSI sequences)
    0, 0, 0, 0, 0, 0,
    // 0x4F - 0x52: Right, Left, Down, Up (handled via ANSI sequences)
    0, 0, 0, 0,
    // 0x53 - 0x63: Keypad NumLock, /, *, -, +, Enter, 1-9, 0, .
    0, '/', '*', '-', '+', '\n',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.',
    // 0x64 - 0xFF: Remaining reserved / non-standard
    0
};

inline constexpr char kHidUsageToAsciiShifted[256] = {
    // 0x00 - 0x03: Special
    0, 0, 0, 0,
    // 0x04 - 0x1D: Letters A - Z
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    // 0x1E - 0x27: Shifted digits !, @, #, $, %, ^, &, *, (, )
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    // 0x28 - 0x2C: Enter, Escape, Backspace, Tab, Space
    '\n', '\033', '\b', '\t', ' ',
    // 0x2D - 0x38: Shifted punctuation: _, +, {, }, |, ~, :, ", ~, <, >, ?
    '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?',
    // 0x39: Caps Lock (handled dynamically)
    0,
    // 0x3A - 0x45: F1 - F12
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x46 - 0x48: PrintScreen, ScrollLock, Pause
    0, 0, 0,
    // 0x49 - 0x4E: Insert, Home, PageUp, Delete, End, PageDown
    0, 0, 0, 0, 0, 0,
    // 0x4F - 0x52: Right, Left, Down, Up
    0, 0, 0, 0,
    // 0x53 - 0x63: Keypad
    0, '/', '*', '-', '+', '\n',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.',
    0
};

/**
 * @brief Returns the ANSI escape sequence for navigation, arrow, or function keys.
 */
inline const char* getHidAnsiSequence(u8 usageId) noexcept {
    switch (usageId) {
        case 0x49: return "\033[2~"; // Insert
        case 0x4A: return "\033[H";  // Home
        case 0x4B: return "\033[5~"; // Page Up
        case 0x4C: return "\033[3~"; // Delete
        case 0x4D: return "\033[F";  // End
        case 0x4E: return "\033[6~"; // Page Down
        case 0x4F: return "\033[C";  // Right Arrow
        case 0x50: return "\033[D";  // Left Arrow
        case 0x51: return "\033[B";  // Down Arrow
        case 0x52: return "\033[A";  // Up Arrow
        case 0x3A: return "\033OP";  // F1
        case 0x3B: return "\033OQ";  // F2
        case 0x3C: return "\033OR";  // F3
        case 0x3D: return "\033OS";  // F4
        case 0x3E: return "\033[15~"; // F5
        case 0x3F: return "\033[17~"; // F6
        case 0x40: return "\033[18~"; // F7
        case 0x41: return "\033[19~"; // F8
        case 0x42: return "\033[20~"; // F9
        case 0x43: return "\033[21~"; // F10
        case 0x44: return "\033[23~"; // F11
        case 0x45: return "\033[24~"; // F12
        default:   return nullptr;
    }
}

} // namespace cortez::drivers::usb
