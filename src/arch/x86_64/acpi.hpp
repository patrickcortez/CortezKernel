/**
 * @file acpi.hpp
 * @brief ACPI table parser (RSDP -> RSDT/XSDT -> MADT & MCFG) for laptop APIC and PCIe discovery.
 */

#pragma once

#include "../../lib/types.hpp"
#include "../../lib/result.hpp"

namespace cortez::arch {

struct CORTEZ_PACKED AcpiRsdp {
    char signature[8];
    u8   checksum;
    char oemId[6];
    u8   revision;
    u32  rsdtAddress;
    // ACPI 2.0+ extension
    u32  length;
    u64  xsdtAddress;
    u8   extendedChecksum;
    u8   reserved[3];
};

struct CORTEZ_PACKED AcpiSdtHeader {
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oemId[6];
    char oemTableId[8];
    u32  oemRevision;
    u32  creatorId;
    u32  creatorRevision;
};

struct CORTEZ_PACKED AcpiMcfgAllocation {
    u64 baseAddress;
    u16 pciSegmentGroup;
    u8  startBusNumber;
    u8  endBusNumber;
    u32 reserved;
};

struct IsoOverride {
    u8  irqSource;
    u32 gsi;
    u16 flags;
};

class AcpiManager final {
public:
    static constexpr usize MAX_ISO_OVERRIDES = 16;
    static constexpr usize MAX_MCFG_SEGMENTS = 8;

    static AcpiManager& instance() noexcept;

    Status init(phys_addr_t rsdpPhys, u64 hhdmOffset) noexcept;

    phys_addr_t getLocalApicPhys() const noexcept { return m_lapicPhys; }
    phys_addr_t getIoApicPhys() const noexcept { return m_ioApicPhys; }
    u32         getIoApicGsiBase() const noexcept { return m_ioApicGsiBase; }
    u32         resolveIrqToGsi(u8 isaIrq, u16* outFlags = nullptr) const noexcept;

    usize                     getMcfgCount() const noexcept { return m_mcfgCount; }
    const AcpiMcfgAllocation& getMcfg(usize idx) const noexcept { return m_mcfg[idx]; }
    bool                      has8042Controller() const noexcept { return m_has8042; }

private:
    AcpiManager() noexcept = default;

    void parseMadt(const AcpiSdtHeader* madt) noexcept;
    void parseMcfg(const AcpiSdtHeader* mcfg) noexcept;
    void parseFadt(const AcpiSdtHeader* fadt) noexcept;

    u64                m_hhdmOffset     = 0;
    phys_addr_t        m_lapicPhys      = 0xFEE00000ULL;
    phys_addr_t        m_ioApicPhys     = 0xFEC00000ULL;
    u32                m_ioApicGsiBase  = 0;
    IsoOverride        m_iso[MAX_ISO_OVERRIDES]{};
    usize              m_isoCount       = 0;
    AcpiMcfgAllocation m_mcfg[MAX_MCFG_SEGMENTS]{};
    usize              m_mcfgCount      = 0;
    bool               m_has8042        = true;
};

} // namespace cortez::arch
