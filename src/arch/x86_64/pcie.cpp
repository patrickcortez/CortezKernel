/**
 * @file pcie.cpp
 * @brief Implementation of multi-bus PCIe scanner, 64-bit BAR decoder, and MSI/MSI-X setup.
 */

#include "pcie.hpp"
#include "acpi.hpp"
#include "io_ports.hpp"
#include "../../lib/printk.hpp"

namespace cortez::arch {

namespace {
constexpr u16 PCI_CONFIG_ADDRESS = 0xCF8;
constexpr u16 PCI_CONFIG_DATA    = 0xCFC;

struct CORTEZ_PACKED MsixTableEntry {
    u32 msgAddrLow;
    u32 msgAddrHigh;
    u32 msgData;
    u32 vectorControl;
};
} // namespace

PcieManager& PcieManager::instance() noexcept {
    static PcieManager s_pcie;
    return s_pcie;
}

u32 PcieManager::readConfig32(u8 bus, u8 dev, u8 func, u16 offset) const noexcept {
    if (m_ecamVirt != 0 && bus >= m_ecamStartBus && bus <= m_ecamEndBus && offset < 4096) {
        usize ecamOffset = (static_cast<usize>(bus - m_ecamStartBus) << 20) |
                           (static_cast<usize>(dev & 0x1F) << 15) |
                           (static_cast<usize>(func & 0x07) << 12) |
                           (static_cast<usize>(offset & 0xFFFC));
        return *reinterpret_cast<volatile const u32*>(m_ecamVirt + ecamOffset);
    }

    u32 addr = (1U << 31) |
               (static_cast<u32>(bus) << 16) |
               (static_cast<u32>(dev & 0x1F) << 11) |
               (static_cast<u32>(func & 0x07) << 8) |
               (static_cast<u32>(offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, addr);
    return inl(PCI_CONFIG_DATA);
}

u16 PcieManager::readConfig16(u8 bus, u8 dev, u8 func, u16 offset) const noexcept {
    u32 val32 = readConfig32(bus, dev, func, offset & ~0x03U);
    return static_cast<u16>((val32 >> ((offset & 2U) * 8U)) & 0xFFFFU);
}

u8 PcieManager::readConfig8(u8 bus, u8 dev, u8 func, u16 offset) const noexcept {
    u32 val32 = readConfig32(bus, dev, func, offset & ~0x03U);
    return static_cast<u8>((val32 >> ((offset & 3U) * 8U)) & 0xFFU);
}

void PcieManager::writeConfig32(u8 bus, u8 dev, u8 func, u16 offset, u32 value) noexcept {
    if (m_ecamVirt != 0 && bus >= m_ecamStartBus && bus <= m_ecamEndBus && offset < 4096) {
        usize ecamOffset = (static_cast<usize>(bus - m_ecamStartBus) << 20) |
                           (static_cast<usize>(dev & 0x1F) << 15) |
                           (static_cast<usize>(func & 0x07) << 12) |
                           (static_cast<usize>(offset & 0xFFFC));
        *reinterpret_cast<volatile u32*>(m_ecamVirt + ecamOffset) = value;
        return;
    }

    u32 addr = (1U << 31) |
               (static_cast<u32>(bus) << 16) |
               (static_cast<u32>(dev & 0x1F) << 11) |
               (static_cast<u32>(func & 0x07) << 8) |
               (static_cast<u32>(offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, value);
}

void PcieManager::writeConfig16(u8 bus, u8 dev, u8 func, u16 offset, u16 value) noexcept {
    u16 aligned = offset & ~0x03U;
    u32 shift   = (offset & 2U) * 8U;
    u32 oldVal  = readConfig32(bus, dev, func, aligned);
    u32 newVal  = (oldVal & ~(0xFFFFU << shift)) | (static_cast<u32>(value) << shift);
    writeConfig32(bus, dev, func, aligned, newVal);
}

void PcieManager::writeConfig8(u8 bus, u8 dev, u8 func, u16 offset, u8 value) noexcept {
    u16 aligned = offset & ~0x03U;
    u32 shift   = (offset & 3U) * 8U;
    u32 oldVal  = readConfig32(bus, dev, func, aligned);
    u32 newVal  = (oldVal & ~(0xFFU << shift)) | (static_cast<u32>(value) << shift);
    writeConfig32(bus, dev, func, aligned, newVal);
}

phys_addr_t PcieManager::decodeBar0(u8 bus, u8 dev, u8 func, usize* outSize) noexcept {
    u32 bar0 = readConfig32(bus, dev, func, 0x10);
    if ((bar0 & 0x01U) != 0) {
        if (outSize) *outSize = 0;
        return 0; // I/O port BAR, not MMIO
    }

    bool is64Bit = ((bar0 & 0x06U) == 0x04U);
    u32  bar1    = is64Bit ? readConfig32(bus, dev, func, 0x14) : 0;

    phys_addr_t physBase = static_cast<phys_addr_t>(bar0 & 0xFFFFFFF0U) |
                           (static_cast<phys_addr_t>(bar1) << 32);

    if (outSize) {
        *outSize = 131072; // Default 128 KiB MMIO window covering NVMe & xHCI registers/doorbells
    }
    return physBase;
}

void PcieManager::probeFunction(u8 bus, u8 dev, u8 func) noexcept {
    u32 idReg    = readConfig32(bus, dev, func, 0x00);
    u16 vendorId = static_cast<u16>(idReg & 0xFFFFU);
    if (vendorId == 0xFFFFU || vendorId == 0x0000U) return;

    u16 deviceId   = static_cast<u16>((idReg >> 16) & 0xFFFFU);
    u32 classReg   = readConfig32(bus, dev, func, 0x08);
    u8  revisionId = static_cast<u8>(classReg & 0xFFU);
    u8  progIf     = static_cast<u8>((classReg >> 8) & 0xFFU);
    u8  subclass   = static_cast<u8>((classReg >> 16) & 0xFFU);
    u8  classCode  = static_cast<u8>((classReg >> 24) & 0xFFU);
    u8  headerType = readConfig8(bus, dev, func, 0x0E) & 0x7FU;
    u8  irqLine    = readConfig8(bus, dev, func, 0x3C);

    if (m_deviceCount >= MAX_PCI_DEVICES) return;

    usize bar0Size = 0;
    phys_addr_t bar0Phys = (headerType == 0x00) ? decodeBar0(bus, dev, func, &bar0Size) : 0;

    PciDeviceInfo& info = m_devices[m_deviceCount++];
    info.segment    = 0;
    info.bus        = bus;
    info.device     = dev;
    info.function   = func;
    info.vendorId   = vendorId;
    info.deviceId   = deviceId;
    info.classCode  = classCode;
    info.subclass   = subclass;
    info.progIf     = progIf;
    info.revisionId = revisionId;
    info.headerType = headerType;
    info.irqLine    = irqLine;
    info.bar0Phys   = bar0Phys;
    info.bar0Size   = bar0Size;
}

Status PcieManager::init(memory::IVirtualMapper* vmm) noexcept {
    m_vmm         = vmm;
    m_deviceCount = 0;
    m_ecamVirt    = 0;

    // Scan all 256 PCIe buses across 32 devices and 8 functions
    for (u32 bus = 0; bus < 256; ++bus) {
        for (u8 dev = 0; dev < 32; ++dev) {
            u16 vendor0 = readConfig16(static_cast<u8>(bus), dev, 0, 0x00);
            if (vendor0 == 0xFFFFU || vendor0 == 0x0000U) continue;

            probeFunction(static_cast<u8>(bus), dev, 0);

            u8 headerType = readConfig8(static_cast<u8>(bus), dev, 0, 0x0E);
            if (headerType & 0x80U) {
                for (u8 func = 1; func < 8; ++func) {
                    probeFunction(static_cast<u8>(bus), dev, func);
                }
            }
        }
    }

    pr_ok("pcie", "Enumerated %zu PCIe functions across buses 0..255", m_deviceCount);
    return Status::Ok;
}

void PcieManager::enableBusMastering(const PciDeviceInfo& dev) noexcept {
    u16 cmd = readConfig16(dev.bus, dev.device, dev.function, 0x04);
    // Bit 1 = Memory Space Enable, Bit 2 = Bus Master Enable
    cmd |= (1U << 1) | (1U << 2);
    writeConfig16(dev.bus, dev.device, dev.function, 0x04, cmd);
}

bool PcieManager::enableMsiOrMsix(const PciDeviceInfo& dev, u8 vector, u32 apicId) noexcept {
    u16 status = readConfig16(dev.bus, dev.device, dev.function, 0x06);
    if (!(status & (1U << 4))) {
        return false; // No Capabilities list
    }

    u32 msgAddr = 0xFEE00000U | ((apicId & 0xFFU) << 12);
    u32 msgData = vector;

    u8 capOffset = readConfig8(dev.bus, dev.device, dev.function, 0x34) & 0xFCU;
    u8 msiOffset  = 0;
    u8 msixOffset = 0;

    for (u32 guard = 0; capOffset != 0 && guard < 48; ++guard) {
        u8 capId   = readConfig8(dev.bus, dev.device, dev.function, capOffset);
        u8 nextCap = readConfig8(dev.bus, dev.device, dev.function, capOffset + 1) & 0xFCU;

        if (capId == 0x05U) msiOffset  = capOffset;
        if (capId == 0x11U) msixOffset = capOffset;
        capOffset = nextCap;
    }

    // Prefer MSI (Capability 0x05) when available for single-queue low-overhead delivery
    if (msiOffset != 0) {
        u16 msgCtrl = readConfig16(dev.bus, dev.device, dev.function, msiOffset + 2);
        bool is64Bit = (msgCtrl & (1U << 7)) != 0;

        writeConfig32(dev.bus, dev.device, dev.function, msiOffset + 4, msgAddr);
        if (is64Bit) {
            writeConfig32(dev.bus, dev.device, dev.function, msiOffset + 8, 0);
            writeConfig16(dev.bus, dev.device, dev.function, msiOffset + 12, static_cast<u16>(msgData));
        } else {
            writeConfig16(dev.bus, dev.device, dev.function, msiOffset + 8, static_cast<u16>(msgData));
        }

        // Single message (MME = 0) + MSI Enable (bit 0)
        msgCtrl &= ~(0x07U << 4);
        msgCtrl |= (1U << 0);
        writeConfig16(dev.bus, dev.device, dev.function, msiOffset + 2, msgCtrl);

        // Disable Legacy INTx in PCI Command register (bit 10)
        u16 cmd = readConfig16(dev.bus, dev.device, dev.function, 0x04);
        writeConfig16(dev.bus, dev.device, dev.function, 0x04, cmd | (1U << 10));
        return true;
    }

    // Fallback to MSI-X (Capability 0x11) for NVMe controllers that only expose MSI-X
    if (msixOffset != 0 && m_vmm != nullptr) {
        u16 msgCtrl  = readConfig16(dev.bus, dev.device, dev.function, msixOffset + 2);
        u32 tableReg = readConfig32(dev.bus, dev.device, dev.function, msixOffset + 4);
        u8  bir      = static_cast<u8>(tableReg & 0x07U);
        u32 offset   = tableReg & ~0x07U;

        u16 barRegOffset = static_cast<u16>(0x10U + bir * 4U);
        u32 barLow       = readConfig32(dev.bus, dev.device, dev.function, barRegOffset);
        bool is64        = ((barLow & 0x06U) == 0x04U);
        u32 barHigh      = is64 ? readConfig32(dev.bus, dev.device, dev.function, barRegOffset + 4) : 0;

        phys_addr_t barPhys = static_cast<phys_addr_t>(barLow & 0xFFFFFFF0U) |
                              (static_cast<phys_addr_t>(barHigh) << 32);
        if (barPhys != 0) {
            auto mapRes = m_vmm->mapMmio(barPhys + offset, PAGE_SIZE_4K);
            if (mapRes.isOk()) {
                auto* entry = reinterpret_cast<volatile MsixTableEntry*>(mapRes.value());
                entry[0].msgAddrLow    = msgAddr;
                entry[0].msgAddrHigh   = 0;
                entry[0].msgData       = msgData;
                entry[0].vectorControl = 0; // Unmasked

                // Enable MSI-X (bit 15), clear Function Mask (bit 14)
                msgCtrl = static_cast<u16>((msgCtrl | (1U << 15)) & ~(1U << 14));
                writeConfig16(dev.bus, dev.device, dev.function, msixOffset + 2, msgCtrl);

                u16 cmd = readConfig16(dev.bus, dev.device, dev.function, 0x04);
                writeConfig16(dev.bus, dev.device, dev.function, 0x04, cmd | (1U << 10));
                return true;
            }
        }
    }

    return false;
}

const PciDeviceInfo* PcieManager::findByClass(u8 classCode, u8 subclass, u8 progIf, usize occurrenceIndex) const noexcept {
    usize matched = 0;
    for (usize i = 0; i < m_deviceCount; ++i) {
        if (m_devices[i].classCode == classCode &&
            m_devices[i].subclass  == subclass &&
            m_devices[i].progIf    == progIf) {
            if (matched == occurrenceIndex) {
                return &m_devices[i];
            }
            ++matched;
        }
    }
    return nullptr;
}

} // namespace cortez::arch
