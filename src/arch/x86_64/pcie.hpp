/**
 * @file pcie.hpp
 * @brief Multi-bus PCIe ECAM & Legacy PCI bus scanner with BAR decoding and MSI/MSI-X setup.
 */

#pragma once

#include "../../lib/types.hpp"
#include "../../lib/result.hpp"
#include "../../memory/i_memory.hpp"

namespace cortez::arch {

struct PciDeviceInfo {
    u16         segment;
    u8          bus;
    u8          device;
    u8          function;
    u16         vendorId;
    u16         deviceId;
    u8          classCode;
    u8          subclass;
    u8          progIf;
    u8          revisionId;
    u8          headerType;
    u8          irqLine;
    phys_addr_t bar0Phys;
    usize       bar0Size;
};

class PcieManager final {
public:
    static constexpr usize MAX_PCI_DEVICES = 64;

    static PcieManager& instance() noexcept;

    Status init(memory::IVirtualMapper* vmm) noexcept;

    u32  readConfig32(u8 bus, u8 dev, u8 func, u16 offset) const noexcept;
    u16  readConfig16(u8 bus, u8 dev, u8 func, u16 offset) const noexcept;
    u8   readConfig8(u8 bus, u8 dev, u8 func, u16 offset) const noexcept;
    void writeConfig32(u8 bus, u8 dev, u8 func, u16 offset, u32 value) noexcept;
    void writeConfig16(u8 bus, u8 dev, u8 func, u16 offset, u16 value) noexcept;
    void writeConfig8(u8 bus, u8 dev, u8 func, u16 offset, u8 value) noexcept;

    void enableBusMastering(const PciDeviceInfo& dev) noexcept;
    bool enableMsiOrMsix(const PciDeviceInfo& dev, u8 vector, u32 apicId) noexcept;

    const PciDeviceInfo* findByClass(u8 classCode, u8 subclass, u8 progIf, usize occurrenceIndex = 0) const noexcept;
    usize                getDeviceCount() const noexcept { return m_deviceCount; }
    const PciDeviceInfo& getDevice(usize idx) const noexcept { return m_devices[idx]; }

private:
    PcieManager() noexcept = default;

    void probeFunction(u8 bus, u8 dev, u8 func) noexcept;
    phys_addr_t decodeBar0(u8 bus, u8 dev, u8 func, usize* outSize) noexcept;

    memory::IVirtualMapper* m_vmm         = nullptr;
    virt_addr_t             m_ecamVirt    = 0;
    u8                      m_ecamStartBus = 0;
    u8                      m_ecamEndBus   = 255;
    PciDeviceInfo           m_devices[MAX_PCI_DEVICES]{};
    usize                   m_deviceCount = 0;
};

} // namespace cortez::arch
