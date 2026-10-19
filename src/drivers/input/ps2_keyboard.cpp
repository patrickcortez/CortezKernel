/**
 * @file ps2_keyboard.cpp
 * @brief Implementation of the Laptop EC-safe i8042 PS/2 Keyboard Driver.
 */

#include "ps2_keyboard.hpp"
#include "../../arch/x86_64/io_ports.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::input {

namespace {
constexpr u16 PS2_DATA_PORT   = 0x60;
constexpr u16 PS2_STATUS_PORT = 0x64;
constexpr u16 PS2_CMD_PORT    = 0x64;

constexpr char kScancodeNormal[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

constexpr char kScancodeShifted[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};
} // namespace

Ps2KeyboardDriver& Ps2KeyboardDriver::instance() noexcept {
    static Ps2KeyboardDriver s_kbd;
    return s_kbd;
}

bool Ps2KeyboardDriver::waitInputReady() const noexcept {
    for (u32 i = 0; i < 16384; ++i) {
        if ((arch::inb(PS2_STATUS_PORT) & 0x02U) == 0) {
            return true;
        }
    }
    return false;
}

bool Ps2KeyboardDriver::waitOutputReady() const noexcept {
    for (u32 i = 0; i < 16384; ++i) {
        if ((arch::inb(PS2_STATUS_PORT) & 0x01U) != 0) {
            return true;
        }
    }
    return false;
}

Status Ps2KeyboardDriver::init() noexcept {
    // Non-destructive i8042 flush (safe on laptop Embedded Controllers)
    for (u32 i = 0; i < 32; ++i) {
        if ((arch::inb(PS2_STATUS_PORT) & 0x01U) == 0) break;
        (void)arch::inb(PS2_DATA_PORT);
        arch::ioWait();
    }

    // Read Controller Configuration Byte (0x20), enable Port 1 IRQ (bit 0) + Set 1 translation (bit 6)
    if (waitInputReady()) {
        arch::outb(PS2_CMD_PORT, 0x20);
        if (waitOutputReady()) {
            u8 ccb = arch::inb(PS2_DATA_PORT);
            ccb |= (1U << 0) | (1U << 6); // Enable IRQ1 + Scancode Set 1 translation
            ccb &= ~(1U << 4);            // Ensure Port 1 clock is enabled
            if (waitInputReady()) {
                arch::outb(PS2_CMD_PORT, 0x60);
                if (waitInputReady()) {
                    arch::outb(PS2_DATA_PORT, ccb);
                }
            }
        }
    }

    // Enable first PS/2 port (0xAE)
    if (waitInputReady()) {
        arch::outb(PS2_CMD_PORT, 0xAE);
    }

    // Register interrupt handler and unmask ISA IRQ 1 in I/O APIC
    arch::IdtManager::instance().registerHandler(arch::IdtManager::VEC_KEYBOARD, this);
    arch::ApicController::instance().routeIsaIrq(1, arch::IdtManager::VEC_KEYBOARD, false);

    pr_ok("kbd", "Initialized i8042 PS/2 & Laptop EC Keyboard Driver (IRQ 1 -> Vec 0x%02x)",
          arch::IdtManager::VEC_KEYBOARD);
    return Status::Ok;
}

void Ps2KeyboardDriver::pushSequence(const char* seq) noexcept {
    if (!seq) return;
    while (*seq) {
        m_buffer.push(*seq++);
    }
}

void Ps2KeyboardDriver::processScancode(u8 scancode) noexcept {
    if (scancode == 0xE0U) {
        m_extendedE0 = true;
        return;
    }

    if (m_extendedE0) {
        m_extendedE0     = false;
        bool extReleased = (scancode & 0x80U) != 0;
        u8   extKey      = scancode & 0x7FU;

        if (extKey == 0x1DU) { // Right Ctrl
            m_rightCtrlHeld = !extReleased;
            m_ctrlHeld      = m_leftCtrlHeld || m_rightCtrlHeld;
            return;
        }
        if (extKey == 0x38U) { // Right Alt (AltGr)
            m_rightAltHeld = !extReleased;
            m_altHeld      = m_leftAltHeld || m_rightAltHeld;
            return;
        }
        // Ignore fake shift bytes emitted by laptop ECs around extended navigation keys
        if (extKey == 0x2AU || extKey == 0x36U) {
            return;
        }
        if (extReleased) {
            return;
        }

        switch (extKey) {
            case 0x48U: pushSequence("\033[A");  break; // Up Arrow
            case 0x50U: pushSequence("\033[B");  break; // Down Arrow
            case 0x4DU: pushSequence("\033[C");  break; // Right Arrow
            case 0x4BU: pushSequence("\033[D");  break; // Left Arrow
            case 0x47U: pushSequence("\033[H");  break; // Home
            case 0x4FU: pushSequence("\033[F");  break; // End
            case 0x52U: pushSequence("\033[2~"); break; // Insert
            case 0x53U: pushSequence("\033[3~"); break; // Delete
            case 0x49U: pushSequence("\033[5~"); break; // Page Up
            case 0x51U: pushSequence("\033[6~"); break; // Page Down
            default: break;
        }
        return;
    }

    bool released = (scancode & 0x80U) != 0;
    u8   key      = scancode & 0x7FU;

    if (key == 0x2AU) { // Left Shift
        m_leftShift = !released;
        return;
    }
    if (key == 0x36U) { // Right Shift
        m_rightShift = !released;
        return;
    }
    if (key == 0x1DU) { // Left Ctrl
        m_leftCtrlHeld = !released;
        m_ctrlHeld     = m_leftCtrlHeld || m_rightCtrlHeld;
        return;
    }
    if (key == 0x38U) { // Left Alt
        m_leftAltHeld = !released;
        m_altHeld     = m_leftAltHeld || m_rightAltHeld;
        return;
    }
    if (key == 0x3AU && !released) { // Caps Lock toggle
        m_capsLock = !m_capsLock;
        return;
    }

    if (released) return;

    bool shift = m_leftShift || m_rightShift;
    char ch    = shift ? kScancodeShifted[key] : kScancodeNormal[key];
    if (ch == 0) return;

    // Apply Caps Lock to alphabetic characters
    if (m_capsLock) {
        if (ch >= 'a' && ch <= 'z') {
            ch = str::toUpper(ch);
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = str::toLower(ch);
        }
    }

    if (m_ctrlHeld) {
        char lower = str::toLower(ch);
        if (lower >= 'a' && lower <= 'z') {
            m_buffer.push(static_cast<char>(lower - 'a' + 1));
            return;
        }
        if (ch == '\\' || ch == '|') {
            m_buffer.push('\x1C'); // Ctrl+\ (File Separator)
            return;
        }
        if (ch == '6' || ch == '^' || ch == ' ') {
            m_buffer.push('\x1E'); // Ctrl+6 / Ctrl+^ / Ctrl+Space (Record Separator / Mark)
            return;
        }
        if (ch == '-' || ch == '_' || ch == '/' || ch == '?') {
            m_buffer.push('\x1F'); // Ctrl+_ / Ctrl+- / Ctrl+/ (Unit Separator)
            return;
        }
    }

    if (m_altHeld) {
        m_buffer.push('\033');
        m_buffer.push(ch);
        return;
    }

    m_buffer.push(ch);
}

void Ps2KeyboardDriver::onInterrupt(arch::InterruptFrame*) noexcept {
    while (arch::inb(PS2_STATUS_PORT) & 0x01U) {
        u8 status   = arch::inb(PS2_STATUS_PORT);
        u8 scancode = arch::inb(PS2_DATA_PORT);
        if ((status & 0x20U) == 0) { // Ensure byte is from keyboard, not PS/2 mouse
            processScancode(scancode);
        }
    }
}

void Ps2KeyboardDriver::pollHardware() noexcept {
    u64 flags = arch::disableInterruptsSave();
    u8 status = arch::inb(PS2_STATUS_PORT);
    if ((status & 0x01U) != 0 && (status & 0x20U) == 0) {
        u8 scancode = arch::inb(PS2_DATA_PORT);
        processScancode(scancode);
    }
    arch::restoreInterrupts(flags);
}

bool Ps2KeyboardDriver::hasData() const noexcept {
    return !m_buffer.isEmpty();
}

char Ps2KeyboardDriver::readCharBlocking() noexcept {
    char ch = 0;
    while (!m_buffer.pop(&ch)) {
        pollHardware();
        if (m_buffer.pop(&ch)) {
            return ch;
        }
        if (arch::interruptsEnabled()) {
            arch::hlt();
        } else {
            arch::pause();
        }
    }
    return ch;
}

Result<usize> Ps2KeyboardDriver::read(void* buffer, usize count) {
    if (!buffer || count == 0) return Status::InvalidArgument;
    auto* out = static_cast<char*>(buffer);
    usize readCount = 0;
    while (readCount < count) {
        char c = 0;
        if (m_buffer.pop(&c)) {
            out[readCount++] = c;
        } else if (readCount > 0) {
            break;
        } else {
            out[readCount++] = readCharBlocking();
            break;
        }
    }
    return readCount;
}

Result<usize> Ps2KeyboardDriver::write(const void*, usize) {
    return Status::NotSupported;
}

} // namespace cortez::drivers::input
