/**
 * @file nvme_queue.hpp
 * @brief Encapsulates an NVMe Submission/Completion Queue Pair with DMA rings and doorbells.
 */

#pragma once

#include "nvme_regs.hpp"
#include "../../memory/i_memory.hpp"
#include "../../lib/spinlock.hpp"
#include "../../lib/result.hpp"

namespace cortez::drivers::storage {

class NvmeQueuePair final {
public:
    NvmeQueuePair() noexcept = default;

    Status init(u16 qid,
                u16 depth,
                volatile u8* bar0Virt,
                u32 doorbellStride,
                memory::IPhysicalAllocator* pmm,
                u64 hhdmOffset) noexcept;

    Status submitAndWait(NvmeSqe& sqe, u32* outResult = nullptr, u32 timeoutMs = 5000) noexcept;

    phys_addr_t getSqPhys() const noexcept { return m_sqPhys; }
    phys_addr_t getCqPhys() const noexcept { return m_cqPhys; }
    u16         getDepth() const noexcept { return m_depth; }
    u16         getQid() const noexcept { return m_qid; }

private:
    Spinlock        m_lock;
    u16             m_qid        = 0;
    u16             m_depth      = 0;
    u16             m_sqTail     = 0;
    u16             m_cqHead     = 0;
    u8              m_cqPhase    = 1;
    u16             m_nextCid    = 1;
    phys_addr_t     m_sqPhys     = 0;
    phys_addr_t     m_cqPhys     = 0;
    volatile NvmeSqe* m_sqVirt   = nullptr;
    volatile NvmeCqe* m_cqVirt   = nullptr;
    volatile u32*   m_sqDoorbell = nullptr;
    volatile u32*   m_cqDoorbell = nullptr;
};

} // namespace cortez::drivers::storage
