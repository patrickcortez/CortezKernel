/**
 * @file ahci_port.hpp
 * @brief AHCI Port driver managing PHY link, DMA rings, and synchronous ATA command execution.
 */

#pragma once

#include "ahci_regs.hpp"
#include "../../lib/spinlock.hpp"
#include "../../lib/result.hpp"
#include "../../memory/i_memory.hpp"

namespace cortez::drivers::storage {

class AhciPort final {
public:
    AhciPort() noexcept = default;
    ~AhciPort() = default;

    AhciPort(const AhciPort&) = delete;
    AhciPort& operator=(const AhciPort&) = delete;

    Status init(u32 portIndex,
                volatile u8* abarVirt,
                bool staggeredSpinup,
                memory::IPhysicalAllocator* pmm,
                u64 hhdmOffset) noexcept;

    Status executeCommand(u8 command,
                          u64 lba,
                          u16 count,
                          void* buffer,
                          bool isWrite) noexcept;

    Status flush() noexcept;

    bool isDriveConnected() const noexcept { return m_connected; }
    bool isSataDisk() const noexcept { return m_isSataDisk; }
    bool isAtapi() const noexcept { return m_isAtapi; }
    u32  getPortIndex() const noexcept { return m_portIndex; }
    u32  getSignature() const noexcept { return m_signature; }
    u8   getSataGen() const noexcept { return m_sataGen; }
    const char* getSpeedString() const noexcept;

    u32  readReg(u32 offset) const noexcept;
    void writeReg(u32 offset, u32 val) noexcept;

private:
    bool stopEngine() noexcept;
    bool startEngine() noexcept;
    bool resetPhy() noexcept;

    Spinlock                    m_lock;
    u32                         m_portIndex   = 0;
    volatile u8*                m_portMmio    = nullptr;
    memory::IPhysicalAllocator* m_pmm         = nullptr;
    u64                         m_hhdmOffset  = 0;

    phys_addr_t                 m_dmaPagePhys = 0;
    u8*                         m_dmaPageVirt = nullptr;

    phys_addr_t                 m_bouncePhys  = 0;
    u8*                         m_bounceVirt  = nullptr;
    static constexpr usize      BOUNCE_SIZE   = 65536; // 64 KiB bounce buffer (128 sectors)

    u32                         m_signature   = 0;
    u8                          m_sataGen     = 0;
    bool                        m_connected   = false;
    bool                        m_isSataDisk  = false;
    bool                        m_isAtapi     = false;
};

} // namespace cortez::drivers::storage
