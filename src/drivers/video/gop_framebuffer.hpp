/**
 * @file gop_framebuffer.hpp
 * @brief UEFI GOP Linear Framebuffer pixel and glyph renderer.
 */

#pragma once

#include "../../boot/boot_info.hpp"
#include "../../lib/types.hpp"

namespace cortez::drivers::video {

class GopFramebuffer final {
public:
    GopFramebuffer() noexcept = default;

    Status init(const boot::FramebufferInfo& fbInfo) noexcept;

    void putPixel(u32 x, u32 y, u32 rgbColor) noexcept;
    void fillRect(u32 x, u32 y, u32 w, u32 h, u32 rgbColor) noexcept;
    void drawGlyph8x16(u32 cellCol, u32 cellRow, char ch, u32 fgColor, u32 bgColor) noexcept;
    void clearScreen(u32 bgColor) noexcept;

    u32  getWidth() const noexcept { return static_cast<u32>(m_info.width); }
    u32  getHeight() const noexcept { return static_cast<u32>(m_info.height); }
    u32  getCols() const noexcept { return m_cols; }
    u32  getRows() const noexcept { return m_rows; }
    bool isReady() const noexcept { return m_pixels != nullptr; }

private:
    u32 packColor(u32 rgb) const noexcept;

    boot::FramebufferInfo m_info{};
    u32*                  m_pixels     = nullptr;
    u32                   m_pitchDwords = 0;
    u32                   m_cols       = 0;
    u32                   m_rows       = 0;
};

} // namespace cortez::drivers::video
