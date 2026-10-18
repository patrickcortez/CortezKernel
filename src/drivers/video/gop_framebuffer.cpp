/**
 * @file gop_framebuffer.cpp
 * @brief Implementation of UEFI GOP Linear Framebuffer rendering.
 */

#include "gop_framebuffer.hpp"
#include "font_8x16.hpp"

namespace cortez::drivers::video {

Status GopFramebuffer::init(const boot::FramebufferInfo& fbInfo) noexcept {
    if (!fbInfo.addr || fbInfo.width == 0 || fbInfo.height == 0 || fbInfo.bpp != 32) {
        return Status::DeviceNotFound;
    }

    m_info        = fbInfo;
    m_pixels      = static_cast<u32*>(fbInfo.addr);
    m_pitchDwords = static_cast<u32>(fbInfo.pitch / sizeof(u32));
    m_cols        = static_cast<u32>(fbInfo.width / FONT_WIDTH);
    m_rows        = static_cast<u32>(fbInfo.height / FONT_HEIGHT);

    return Status::Ok;
}

u32 GopFramebuffer::packColor(u32 rgb) const noexcept {
    u8 r = static_cast<u8>((rgb >> 16) & 0xFFU);
    u8 g = static_cast<u8>((rgb >> 8) & 0xFFU);
    u8 b = static_cast<u8>(rgb & 0xFFU);
    return (static_cast<u32>(r) << m_info.redShift) |
           (static_cast<u32>(g) << m_info.greenShift) |
           (static_cast<u32>(b) << m_info.blueShift);
}

void GopFramebuffer::putPixel(u32 x, u32 y, u32 rgbColor) noexcept {
    if (!m_pixels || x >= m_info.width || y >= m_info.height) return;
    m_pixels[y * m_pitchDwords + x] = packColor(rgbColor);
}

void GopFramebuffer::fillRect(u32 x, u32 y, u32 w, u32 h, u32 rgbColor) noexcept {
    if (!m_pixels) return;
    u32 packed = packColor(rgbColor);
    u32 maxX = (x + w > m_info.width) ? static_cast<u32>(m_info.width) : (x + w);
    u32 maxY = (y + h > m_info.height) ? static_cast<u32>(m_info.height) : (y + h);

    for (u32 row = y; row < maxY; ++row) {
        u32* line = &m_pixels[row * m_pitchDwords];
        for (u32 col = x; col < maxX; ++col) {
            line[col] = packed;
        }
    }
}

void GopFramebuffer::drawGlyph8x16(u32 cellCol, u32 cellRow, char ch, u32 fgColor, u32 bgColor) noexcept {
    if (!m_pixels || cellCol >= m_cols || cellRow >= m_rows) return;

    u8 glyphIdx = static_cast<u8>(ch);
    if (glyphIdx >= 128) glyphIdx = '?';

    const u8* glyph = kFont8x16[glyphIdx];
    u32 fg = packColor(fgColor);
    u32 bg = packColor(bgColor);

    u32 baseX = cellCol * FONT_WIDTH;
    u32 baseY = cellRow * FONT_HEIGHT;

    for (u32 gy = 0; gy < FONT_HEIGHT; ++gy) {
        u8 bits   = glyph[gy];
        u32* line = &m_pixels[(baseY + gy) * m_pitchDwords + baseX];
        line[0] = (bits & 0x80U) ? fg : bg;
        line[1] = (bits & 0x40U) ? fg : bg;
        line[2] = (bits & 0x20U) ? fg : bg;
        line[3] = (bits & 0x10U) ? fg : bg;
        line[4] = (bits & 0x08U) ? fg : bg;
        line[5] = (bits & 0x04U) ? fg : bg;
        line[6] = (bits & 0x02U) ? fg : bg;
        line[7] = (bits & 0x01U) ? fg : bg;
    }
}

void GopFramebuffer::clearScreen(u32 bgColor) noexcept {
    fillRect(0, 0, static_cast<u32>(m_info.width), static_cast<u32>(m_info.height), bgColor);
}

} // namespace cortez::drivers::video
