/**
 * @file nvme_controller.hpp
 * @brief Physical NVMe Controller and Namespace Block Device driver for Laptops and VMs.
 */

#pragma once

#include "../i_device.hpp"
#include "nvme_queue.hpp"
#include "../../arch/x86_64/pcie.hpp"
#include "../../arch/x86_64/idt.hpp"

namespace cortez::drivers::storage {

class NvmeController;

/**
 * @brief Represents an active NVMe Namespace (/dev/nvme0n1) implementing IBlockDevice.
 */
class NvmeNamespace final : public IBlockDevice {
public:
    NvmeNamespace() noexcept = default;

    void init(NvmeController* ctrl, u32 nsid, u32 blockSize, u64 blockCount) noexcept;

    const char*    getName() const noexcept override { return "nvme0n1"; }
    StorageBusType getBusType() const noexcept override { return StorageBusType::NVMe; }
    u32            getBlockSize() const noexcept override { return m_blockSize; }
    u64            getBlockCount() const noexcept override { return m_blockCount; }

    Status readBlocks(u64 lba, u32 count, void* buffer) override;
    Status writeBlocks(u64 lba, u32 count, const void* buffer) override;
    Status flush() override;

    u32 getNsid() const noexcept { return m_nsid; }

private:
    NvmeController* m_ctrl       = nullptr;
    u32             m_nsid       = 1;
    u32             m_blockSize  = 512;
    u64             m_blockCount = 0;
};

/**
 * @brief Physical PCIe NVMe Controller driver managing Admin and I/O queues.
 */
class NvmeController final : public arch::IInterruptHandler {
public:
    static NvmeController& instance() noexcept;

    Status init(memory::IPhysicalAllocator* pmm,
                memory::IVirtualMapper* vmm,
                u64 hhdmOffset) noexcept;

    Status rwSingleBlock(u32 nsid, u64 lba, u32 blockSize, void* buffer, bool isWrite) noexcept;
    Status flushNamespace(u32 nsid) noexcept;

    void onInterrupt(arch::InterruptFrame* frame) noexcept override;

    bool                isReady() const noexcept { return m_ready; }
    const char*         getModel() const noexcept { return m_model; }
    const char*         getSerial() const noexcept { return m_serial; }
    const char*         getFirmware() const noexcept { return m_firmware; }
    u32                 getVersion() const noexcept { return m_version; }
    NvmeNamespace*      getNamespace() noexcept { return &m_namespace; }

private:
    NvmeController() noexcept = default;

    u32  readReg32(u32 offset) const noexcept;
    void writeReg32(u32 offset, u32 value) noexcept;
    u64  readReg64(u32 offset) const noexcept;
    void writeReg64(u32 offset, u64 value) noexcept;

    bool   waitControllerReady(bool expectedRdy, u32 timeoutMs) const noexcept;
    Status identifyController() noexcept;
    Status createIoQueues() noexcept;
    Status identifyNamespace(u32 nsid) noexcept;

    Spinlock                    m_dmaLock;
    memory::IPhysicalAllocator* m_pmm            = nullptr;
    u64                         m_hhdmOffset     = 0;
    volatile u8*                m_bar0Virt       = nullptr;
    u32                         m_doorbellStride = 4;
    u32                         m_version        = 0;
    bool                        m_ready          = false;

    NvmeQueuePair               m_adminQueue{};
    NvmeQueuePair               m_ioQueue{};
    NvmeNamespace               m_namespace{};

    phys_addr_t                 m_dmaPagePhys    = 0;
    u8*                         m_dmaPageVirt    = nullptr;

    char                        m_model[44]{};
    char                        m_serial[24]{};
    char                        m_firmware[12]{};
};

} // namespace cortez::drivers::storage
