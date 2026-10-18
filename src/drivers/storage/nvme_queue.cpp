/**
 * @file nvme_queue.cpp
 * @brief Implementation of NVMe Submission/Completion Queue Pair operations.
 */

#include "nvme_queue.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../lib/string_utils.hpp"

namespace cortez::drivers::storage {

Status NvmeQueuePair::init(u16 qid,
                           u16 depth,
                           volatile u8* bar0Virt,
                           u32 doorbellStride,
                           memory::IPhysicalAllocator* pmm,
                           u64 hhdmOffset) noexcept {
    if (!bar0Virt || !pmm || depth < 2 || depth > 64) {
        return Status::InvalidArgument;
    }

    m_qid     = qid;
    m_depth   = depth;
    m_sqTail  = 0;
    m_cqHead  = 0;
    m_cqPhase = 1;
    m_nextCid = 1;

    auto sqRes = pmm->allocPages(1);
    if (sqRes.isError()) return sqRes.status();

    auto cqRes = pmm->allocPages(1);
    if (cqRes.isError()) {
        pmm->freePages(sqRes.value(), 1);
        return cqRes.status();
    }

    m_sqPhys = sqRes.value();
    m_cqPhys = cqRes.value();
    m_sqVirt = reinterpret_cast<volatile NvmeSqe*>(m_sqPhys + hhdmOffset);
    m_cqVirt = reinterpret_cast<volatile NvmeCqe*>(m_cqPhys + hhdmOffset);

    u32 sqDbOffset = NVME_REG_DBS + static_cast<u32>(2U * qid) * doorbellStride;
    u32 cqDbOffset = NVME_REG_DBS + static_cast<u32>(2U * qid + 1U) * doorbellStride;

    m_sqDoorbell = reinterpret_cast<volatile u32*>(bar0Virt + sqDbOffset);
    m_cqDoorbell = reinterpret_cast<volatile u32*>(bar0Virt + cqDbOffset);

    return Status::Ok;
}

Status NvmeQueuePair::submitAndWait(NvmeSqe& sqe, u32* outResult, u32 timeoutMs) noexcept {
    IrqLockGuard guard(m_lock);

    u16 cid = m_nextCid++;
    if (m_nextCid == 0) m_nextCid = 1;
    sqe.cid = cid;

    // Copy 64-byte SQE into Submission Queue ring slot
    volatile NvmeSqe* slot = &m_sqVirt[m_sqTail];
    const auto* srcWords   = reinterpret_cast<const u64*>(&sqe);
    auto* dstWords         = reinterpret_cast<volatile u64*>(slot);
    for (usize i = 0; i < 8; ++i) {
        dstWords[i] = srcWords[i];
    }

    m_sqTail = static_cast<u16>((m_sqTail + 1U) % m_depth);

    arch::memoryBarrier();
    *m_sqDoorbell = m_sqTail;

    // Wait for Completion Queue entry phase tag to match m_cqPhase
    volatile NvmeCqe* cqe = &m_cqVirt[m_cqHead];
    u32 waitedLoops = 0;
    u32 maxLoops    = timeoutMs * 25000U;

    while ((cqe->status & 0x01U) != m_cqPhase) {
        arch::pause();
        if (++waitedLoops > maxLoops) {
            return Status::Timeout;
        }
    }

    arch::memoryBarrier();

    u16 statusField = cqe->status >> 1; // Status Code + Status Code Type
    u32 resultVal   = cqe->result;

    // Advance CQ head and flip expected Phase Tag upon wrap-around
    if (++m_cqHead >= m_depth) {
        m_cqHead  = 0;
        m_cqPhase ^= 1U;
    }

    *m_cqDoorbell = m_cqHead;

    if (outResult) {
        *outResult = resultVal;
    }

    return (statusField == 0) ? Status::Ok : Status::IoError;
}

} // namespace cortez::drivers::storage
