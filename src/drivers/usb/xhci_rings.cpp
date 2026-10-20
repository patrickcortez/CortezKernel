/**
 * @file xhci_rings.cpp
 * @brief Implementation of xHCI Producer TRB Rings and Interrupter 0 Event Ring polling.
 */

#include "xhci_rings.hpp"
#include "xhci_controller.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../lib/string_utils.hpp"

namespace cortez::drivers::usb {

Status XhciTrbRing::init(memory::IPhysicalAllocator* pmm, u64 hhdmOffset) noexcept {
    if (!pmm) return Status::InvalidArgument;

    if (m_ringPhys == 0) {
        auto pageRes = pmm->allocPages(1);
        if (pageRes.isError()) return pageRes.status();
        m_ringPhys = pageRes.value();
        m_ringVirt = reinterpret_cast<volatile XhciTrb*>(m_ringPhys + hhdmOffset);
    }

    reset();
    return Status::Ok;
}

void XhciTrbRing::reset() noexcept {
    if (!m_ringVirt) return;

    memset(const_cast<XhciTrb*>(m_ringVirt), 0, PAGE_SIZE_4K);
    m_enqueueIdx = 0;
    m_cycleState = 1;

    // Configure final entry (index 255) as a Link TRB pointing back to the start of the ring
    volatile XhciTrb& link = m_ringVirt[RING_CAPACITY - 1];
    link.param   = m_ringPhys;
    link.status  = 0;
    link.control = (static_cast<u32>(XHCI_TRB_LINK) << 10) | XHCI_TRB_TC;
}

phys_addr_t XhciTrbRing::enqueue(const XhciTrb& trb) noexcept {
    volatile XhciTrb& slot = m_ringVirt[m_enqueueIdx];
    phys_addr_t trbPhys    = m_ringPhys + static_cast<phys_addr_t>(m_enqueueIdx * sizeof(XhciTrb));

    slot.param  = trb.param;
    slot.status = trb.status;
    arch::memoryBarrier();

    u32 ctrl = (trb.control & ~XHCI_TRB_CYCLE) | (m_cycleState ? XHCI_TRB_CYCLE : 0U);
    slot.control = ctrl;
    arch::memoryBarrier();

    ++m_enqueueIdx;
    if (m_enqueueIdx >= RING_CAPACITY - 1) {
        // Update Link TRB cycle bit and toggle producer cycle state
        volatile XhciTrb& link = m_ringVirt[RING_CAPACITY - 1];
        link.param  = m_ringPhys;
        link.status = 0;
        u32 linkCtrl = (static_cast<u32>(XHCI_TRB_LINK) << 10) | XHCI_TRB_TC |
                       (m_cycleState ? XHCI_TRB_CYCLE : 0U);
        link.control = linkCtrl;
        arch::memoryBarrier();

        m_enqueueIdx = 0;
        m_cycleState ^= 1U;
    }

    return trbPhys;
}

// =============================================================================
// XhciEventRing Implementation
// =============================================================================

Status XhciEventRing::init(volatile u8* runtimeBaseVirt,
                           memory::IPhysicalAllocator* pmm,
                           u64 hhdmOffset) noexcept {
    if (!runtimeBaseVirt || !pmm) return Status::InvalidArgument;

    m_ir0Virt = runtimeBaseVirt + XHCI_IR_BASE;

    auto trbPage = pmm->allocPages(1);
    if (trbPage.isError()) return trbPage.status();

    auto erstPage = pmm->allocPages(1);
    if (erstPage.isError()) {
        pmm->freePages(trbPage.value(), 1);
        return erstPage.status();
    }

    m_trbsPhys = trbPage.value();
    m_erstPhys = erstPage.value();
    m_trbsVirt = reinterpret_cast<volatile XhciTrb*>(m_trbsPhys + hhdmOffset);
    m_erstVirt = reinterpret_cast<volatile XhciErstEntry*>(m_erstPhys + hhdmOffset);

    memset(const_cast<XhciTrb*>(m_trbsVirt), 0, PAGE_SIZE_4K);
    memset(const_cast<XhciErstEntry*>(m_erstVirt), 0, PAGE_SIZE_4K);

    m_dequeueIdx = 0;
    m_cycleState = 1;

    // Populate single ERST segment entry
    m_erstVirt[0].ringSegmentBaseAddr = m_trbsPhys;
    m_erstVirt[0].ringSegmentSize     = static_cast<u16>(EVENT_RING_CAPACITY);
    m_erstVirt[0].reserved0           = 0;
    m_erstVirt[0].reserved1           = 0;
    arch::memoryBarrier();

    // Program Interrupter 0 registers per xHCI 1.2 §4.9.4:
    // 1. Initialize ERSTSZ = 1
    *reinterpret_cast<volatile u32*>(m_ir0Virt + XHCI_IR_ERSTSZ) = 1U;

    // 2. Initialize ERDP to start of Event Ring
    u64 erdpVal = (m_trbsPhys & ~0x0FULL) | (1ULL << 3); // Clear EHB
    *reinterpret_cast<volatile u32*>(m_ir0Virt + XHCI_IR_ERDP)     = static_cast<u32>(erdpVal & 0xFFFFFFFFULL);
    *reinterpret_cast<volatile u32*>(m_ir0Virt + XHCI_IR_ERDP + 4) = static_cast<u32>(erdpVal >> 32);

    // 3. Initialize ERSTBA (writing ERSTBA triggers internal Event Ring state machine)
    *reinterpret_cast<volatile u32*>(m_ir0Virt + XHCI_IR_ERSTBA)     = static_cast<u32>(m_erstPhys & 0xFFFFFFFFULL);
    *reinterpret_cast<volatile u32*>(m_ir0Virt + XHCI_IR_ERSTBA + 4) = static_cast<u32>(m_erstPhys >> 32);

    // 4. Enable Interrupter 0 (IMOD = ~4000 ints/sec, IMAN.IE = 1 | IMAN.IP = 1 RW1C)
    *reinterpret_cast<volatile u32*>(m_ir0Virt + XHCI_IR_IMOD) = 400U;
    u32 iman = *reinterpret_cast<volatile const u32*>(m_ir0Virt + XHCI_IR_IMAN);
    *reinterpret_cast<volatile u32*>(m_ir0Virt + XHCI_IR_IMAN) = iman | 0x03U;

    return Status::Ok;
}

void XhciEventRing::updateErdp() noexcept {
    phys_addr_t deqPhys = m_trbsPhys + static_cast<phys_addr_t>(m_dequeueIdx * sizeof(XhciTrb));
    u64 erdpVal = (deqPhys & ~0x0FULL) | (1ULL << 3); // Bit 3 = EHB (Event Handler Busy RW1C)
    *reinterpret_cast<volatile u32*>(m_ir0Virt + XHCI_IR_ERDP)     = static_cast<u32>(erdpVal & 0xFFFFFFFFULL);
    *reinterpret_cast<volatile u32*>(m_ir0Virt + XHCI_IR_ERDP + 4) = static_cast<u32>(erdpVal >> 32);

    // Clear Interrupter Pending (IP, bit 0 RW1C) in IMAN while keeping IE (bit 1) set
    u32 iman = *reinterpret_cast<volatile const u32*>(m_ir0Virt + XHCI_IR_IMAN);
    if (iman & 0x01U) {
        *reinterpret_cast<volatile u32*>(m_ir0Virt + XHCI_IR_IMAN) = iman | 0x01U;
    }
}

void XhciEventRing::drainEvents() noexcept {
    IrqLockGuard guard(m_lock);
    for (usize guardIdx = 0; guardIdx < EVENT_RING_CAPACITY; ++guardIdx) {
        volatile const XhciTrb& ev = m_trbsVirt[m_dequeueIdx];
        u8 cycle = static_cast<u8>(ev.control & XHCI_TRB_CYCLE);
        if (cycle != m_cycleState) {
            break;
        }

        if (++m_dequeueIdx >= EVENT_RING_CAPACITY) {
            m_dequeueIdx = 0;
            m_cycleState ^= 1U;
        }
        updateErdp();
    }
}

void XhciEventRing::dispatchEvents(XhciController* ctrl) noexcept {
    if (!ctrl) return;
    IrqLockGuard guard(m_lock);

    for (usize guardIdx = 0; guardIdx < EVENT_RING_CAPACITY; ++guardIdx) {
        volatile const XhciTrb& ev = m_trbsVirt[m_dequeueIdx];
        u32 ctrlBits = ev.control;
        u8 cycle = static_cast<u8>(ctrlBits & XHCI_TRB_CYCLE);
        if (cycle != m_cycleState) {
            break;
        }

        arch::memoryBarrier();
        XhciTrb captured{};
        captured.param   = ev.param;
        captured.status  = ev.status;
        captured.control = ctrlBits;

        if (++m_dequeueIdx >= EVENT_RING_CAPACITY) {
            m_dequeueIdx = 0;
            m_cycleState ^= 1U;
        }
        updateErdp();

        u8 type = captured.getType();
        if (type == XHCI_TRB_PORT_STATUS_CHG) {
            u8 portNum = static_cast<u8>((captured.param >> 24) & 0xFFU);
            ctrl->markPortChange(portNum);
            continue;
        }

        // 1. Dispatch to registered synchronous waiters first
        if (ctrl->dispatchToWaiters(captured)) {
            continue;
        }

        // 2. If not claimed by a waiter, dispatch asynchronous transfer event (e.g. keyboard)
        if (type == XHCI_TRB_TRANSFER_EVENT) {
            ctrl->dispatchTransferEvent(captured);
        }
    }
}

void XhciEventRing::processEvents(XhciController* ctrl) noexcept {
    dispatchEvents(ctrl);
}

Status XhciEventRing::pollEvent(u8 expectedType, XhciTrb* outEvent, u32 timeoutMs) noexcept {
    u64 startMs = arch::ApicController::instance().getUptimeMs();
    u32 spinGuard = 0;
    u32 maxSpins  = timeoutMs * 40000U;

    for (;;) {
        {
            IrqLockGuard guard(m_lock);
            volatile const XhciTrb& ev = m_trbsVirt[m_dequeueIdx];
            u32 ctrl  = ev.control;
            u8  cycle = static_cast<u8>(ctrl & XHCI_TRB_CYCLE);

            if (cycle == m_cycleState) {
                arch::memoryBarrier();
                XhciTrb captured{};
                captured.param   = ev.param;
                captured.status  = ev.status;
                captured.control = ctrl;

                if (++m_dequeueIdx >= EVENT_RING_CAPACITY) {
                    m_dequeueIdx = 0;
                    m_cycleState ^= 1U;
                }
                updateErdp();

                if (captured.getType() == expectedType) {
                    if (outEvent) {
                        *outEvent = captured;
                    }
                    u8 cc = captured.getCompletionCode();
                    if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
                        return Status::Ok;
                    }
                    return Status::IoError;
                }
            }
        }

        arch::pause();
        ++spinGuard;
        u64 nowMs = arch::ApicController::instance().getUptimeMs();
        if ((nowMs > startMs && (nowMs - startMs) >= timeoutMs) || spinGuard > maxSpins) {
            return Status::Timeout;
        }
    }
}

} // namespace cortez::drivers::usb
