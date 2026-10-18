/**
 * @file acpi.cpp
 * @brief Implementation of ACPI table discovery (MADT, MCFG, FADT).
 */

#include "acpi.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::arch {

AcpiManager& AcpiManager::instance() noexcept {
    static AcpiManager s_acpi;
    return s_acpi;
}

Status AcpiManager::init(phys_addr_t rsdpPhys, u64 hhdmOffset) noexcept {
    if (rsdpPhys == 0) {
        pr_warn("acpi", "No RSDP physical address provided; using x86_64 defaults");
        return Status::NotFound;
    }

    m_hhdmOffset = hhdmOffset;
    const auto* rsdp = reinterpret_cast<const AcpiRsdp*>(rsdpPhys + m_hhdmOffset);

    if (str::compareN(rsdp->signature, "RSD PTR ", 8) != 0) {
        pr_err("acpi", "Invalid RSDP signature");
        return Status::CorruptedFs;
    }

    bool useXsdt = (rsdp->revision >= 2 && rsdp->xsdtAddress != 0);
    phys_addr_t rootTablePhys = useXsdt ? rsdp->xsdtAddress : static_cast<phys_addr_t>(rsdp->rsdtAddress);
    const auto* rootSdt = reinterpret_cast<const AcpiSdtHeader*>(rootTablePhys + m_hhdmOffset);

    usize entryBytes = rootSdt->length - sizeof(AcpiSdtHeader);
    usize entryCount = useXsdt ? (entryBytes / 8) : (entryBytes / 4);
    const u8* entriesPtr = reinterpret_cast<const u8*>(rootSdt) + sizeof(AcpiSdtHeader);

    for (usize i = 0; i < entryCount; ++i) {
        phys_addr_t tablePhys = 0;
        if (useXsdt) {
            tablePhys = *reinterpret_cast<const u64*>(entriesPtr + i * 8);
        } else {
            tablePhys = *reinterpret_cast<const u32*>(entriesPtr + i * 4);
        }
        if (tablePhys == 0) continue;

        const auto* hdr = reinterpret_cast<const AcpiSdtHeader*>(tablePhys + m_hhdmOffset);
        if (str::compareN(hdr->signature, "APIC", 4) == 0) {
            parseMadt(hdr);
        } else if (str::compareN(hdr->signature, "MCFG", 4) == 0) {
            parseMcfg(hdr);
        } else if (str::compareN(hdr->signature, "FACP", 4) == 0) {
            parseFadt(hdr);
        }
    }

    pr_ok("acpi", "Parsed ACPI %s: LAPIC=0x%llx, IOAPIC=0x%llx, MCFG segments=%zu",
          useXsdt ? "XSDT (v2.0+)" : "RSDT (v1.0)",
          m_lapicPhys, m_ioApicPhys, m_mcfgCount);
    return Status::Ok;
}

void AcpiManager::parseMadt(const AcpiSdtHeader* madt) noexcept {
    const u8* base = reinterpret_cast<const u8*>(madt);
    u32 lapicAddr32 = *reinterpret_cast<const u32*>(base + sizeof(AcpiSdtHeader));
    if (lapicAddr32 != 0) {
        m_lapicPhys = lapicAddr32;
    }

    usize offset = sizeof(AcpiSdtHeader) + 8; // Skip 32-bit Local APIC addr + 32-bit flags
    while (offset + 2 <= madt->length) {
        u8 entryType = base[offset];
        u8 entryLen  = base[offset + 1];
        if (entryLen < 2 || offset + entryLen > madt->length) break;

        if (entryType == 1 && entryLen >= 12) {
            // I/O APIC entry
            u32 ioApicAddr = *reinterpret_cast<const u32*>(base + offset + 4);
            u32 gsiBase    = *reinterpret_cast<const u32*>(base + offset + 8);
            if (gsiBase == 0) {
                m_ioApicPhys    = ioApicAddr;
                m_ioApicGsiBase = gsiBase;
            }
        } else if (entryType == 2 && entryLen >= 10) {
            // Interrupt Source Override (ISO)
            if (m_isoCount < MAX_ISO_OVERRIDES) {
                m_iso[m_isoCount].irqSource = base[offset + 3];
                m_iso[m_isoCount].gsi       = *reinterpret_cast<const u32*>(base + offset + 4);
                m_iso[m_isoCount].flags     = *reinterpret_cast<const u16*>(base + offset + 8);
                ++m_isoCount;
            }
        } else if (entryType == 5 && entryLen >= 12) {
            // 64-bit Local APIC Address Override
            m_lapicPhys = *reinterpret_cast<const u64*>(base + offset + 4);
        }

        offset += entryLen;
    }
}

void AcpiManager::parseMcfg(const AcpiSdtHeader* mcfg) noexcept {
    const u8* base = reinterpret_cast<const u8*>(mcfg);
    usize offset   = sizeof(AcpiSdtHeader) + 8; // 8-byte reserved field after SDT header

    while (offset + sizeof(AcpiMcfgAllocation) <= mcfg->length && m_mcfgCount < MAX_MCFG_SEGMENTS) {
        const auto* alloc = reinterpret_cast<const AcpiMcfgAllocation*>(base + offset);
        m_mcfg[m_mcfgCount++] = *alloc;
        offset += sizeof(AcpiMcfgAllocation);
    }
}

void AcpiManager::parseFadt(const AcpiSdtHeader* fadt) noexcept {
    if (fadt->revision >= 2 && fadt->length >= 112) {
        const u8* base = reinterpret_cast<const u8*>(fadt);
        u16 bootArchFlags = *reinterpret_cast<const u16*>(base + 109);
        // Bit 1 (0x0002) = 8042 controller present in ACPI 2.0+ FADT
        if (bootArchFlags != 0) {
            m_has8042 = (bootArchFlags & 0x0002U) != 0;
        }
    }
}

u32 AcpiManager::resolveIrqToGsi(u8 isaIrq, u16* outFlags) const noexcept {
    for (usize i = 0; i < m_isoCount; ++i) {
        if (m_iso[i].irqSource == isaIrq) {
            if (outFlags) *outFlags = m_iso[i].flags;
            return m_iso[i].gsi;
        }
    }
    if (outFlags) *outFlags = 0;
    return static_cast<u32>(isaIrq);
}

} // namespace cortez::arch
