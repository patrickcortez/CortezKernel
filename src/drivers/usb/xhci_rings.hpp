/**
 * @file xhci_rings.hpp
 * @brief xHCI Producer TRB Ring (Command / Control / Bulk) and Consumer Event Ring abstractions.
 */

#pragma once

#include "xhci_regs.hpp"
#include "../../memory/i_memory.hpp"
#include "../../lib/result.hpp"

namespace cortez::drivers::usb {

/**
 * @brief Encapsulates a 256-entry (4 KiB page) Producer TRB Ring with Link TRB wrap-around.
 *        Used for the Host Controller Command Ring, EP0 Control Ring, and Bulk IN/OUT Rings.
 */
class XhciTrbRing final {
public:
    static constexpr usize RING_CAPACITY = 256; // 255 usable slots + 1 Link TRB = 4096 bytes

    XhciTrbRing() noexcept = default;

    Status init(memory::IPhysicalAllocator* pmm, u64 hhdmOffset) noexcept;
    void   reset() noexcept;

    phys_addr_t enqueue(const XhciTrb& trb) noexcept;

    phys_addr_t getPhysBase() const noexcept { return m_ringPhys; }
    u8          getCycleState() const noexcept { return m_cycleState; }
    phys_addr_t getCurrentDequeuePhys() const noexcept {
        return m_ringPhys + static_cast<phys_addr_t>(m_enqueueIdx * sizeof(XhciTrb));
    }

private:
    phys_addr_t       m_ringPhys   = 0;
    volatile XhciTrb* m_ringVirt   = nullptr;
    usize             m_enqueueIdx = 0;
    u8                m_cycleState = 1;
};

/**
 * @brief Encapsulates Interrupter 0's Event Ring and Event Ring Segment Table (ERST).
 */
class XhciEventRing final {
public:
    static constexpr usize EVENT_RING_CAPACITY = 256;

    XhciEventRing() noexcept = default;

    Status init(volatile u8* runtimeBaseVirt,
                memory::IPhysicalAllocator* pmm,
                u64 hhdmOffset) noexcept;

    /**
     * @brief Polls the Event Ring until an event of `expectedType` arrives or `timeoutMs` expires.
     *        Non-matching events (such as asynchronous Port Status Change Events) are consumed
     *        and acknowledged automatically.
     */
    Status pollEvent(u8 expectedType, XhciTrb* outEvent, u32 timeoutMs = 5000) noexcept;

    /**
     * @brief Drains any pending asynchronous events (e.g. Port Status Change Events after reset).
     */
    void drainEvents() noexcept;

private:
    void updateErdp() noexcept;

    volatile u8*           m_ir0Virt    = nullptr;
    phys_addr_t            m_trbsPhys   = 0;
    phys_addr_t            m_erstPhys   = 0;
    volatile XhciTrb*      m_trbsVirt   = nullptr;
    volatile XhciErstEntry* m_erstVirt  = nullptr;
    usize                  m_dequeueIdx = 0;
    u8                     m_cycleState = 1;
};

} // namespace cortez::drivers::usb
