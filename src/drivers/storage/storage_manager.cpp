/**
 * @file storage_manager.cpp
 * @brief Implementation of unified dynamic storage orchestrator.
 */

#include "storage_manager.hpp"
#include "nvme_controller.hpp"
#include "ahci_controller.hpp"
#include "gpt_partition.hpp"
#include "../usb/xhci_controller.hpp"
#include "../../arch/x86_64/pcie.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::storage {

StorageManager& StorageManager::instance() noexcept {
    static StorageManager s_mgr;
    return s_mgr;
}

Status StorageManager::probeAll(memory::IPhysicalAllocator* pmm,
                                memory::IVirtualMapper* vmm,
                                u64 hhdmOffset) noexcept {
    auto& pcie = arch::PcieManager::instance();
    m_nvmeControllers = 0;
    m_sataControllers = 0;
    m_usbControllers  = 0;

    for (usize i = 0; i < pcie.getDeviceCount(); ++i) {
        const auto& d = pcie.getDevice(i);
        if (d.classCode == 0x01 && d.subclass == 0x08 && d.progIf == 0x02) {
            ++m_nvmeControllers;
        } else if (d.classCode == 0x01 && d.subclass == 0x06 && d.progIf == 0x01) {
            ++m_sataControllers;
        } else if (d.classCode == 0x01 && d.subclass == 0x04 && d.bar5Phys != 0) {
            ++m_sataControllers;
        } else if (d.classCode == 0x0C && d.subclass == 0x03 && d.progIf == 0x30) {
            ++m_usbControllers;
        }
    }

    pr_info("storage", "PCIe storage scan: %zu NVMe, %zu SATA AHCI, %zu xHCI controller(s) detected",
            m_nvmeControllers, m_sataControllers, m_usbControllers);

    // 1. Probe NVMe if present
    if (m_nvmeControllers > 0) {
        auto& nvme = NvmeController::instance();
        if (nvme.init(pmm, vmm, hhdmOffset) == Status::Ok) {
            m_hasNvme = true;
            if (auto* ns = nvme.getNamespace()) {
                GptPartitionScanner::instance().scanDisk(ns);
            }
        }
    }

    // 2. Probe SATA AHCI if present
    if (m_sataControllers > 0) {
        auto& ahci = AhciController::instance();
        if (ahci.init(pmm, vmm, hhdmOffset) == Status::Ok) {
            m_hasSata = true;
        }
    }

    // 3. Probe USB xHCI if present
    if (m_usbControllers > 0) {
        if (drivers::usb::XhciManager::instance().init(pmm, vmm, hhdmOffset) == Status::Ok) {
            m_hasUsb = true;
        }
    }

    return Status::Ok;
}

IBlockDevice* StorageManager::resolveRootDevice(const char* preferredDevName) noexcept {
    static constexpr const char* ROOTFS_LABEL = "Cortez-RootFS";
    auto& reg = DeviceRegistry::instance();

    // Strategy 1: Find partition labelled "Cortez-RootFS" (UUID/label agnostic boot)
    IBlockDevice* rootBdev = nullptr;
    for (usize i = 0; i < reg.getBlockDeviceCount(); ++i) {
        IBlockDevice* dev = reg.getBlockDeviceAt(i);
        if (dev && str::compare(dev->getLabel(), ROOTFS_LABEL) == 0) {
            rootBdev = dev;
            break;
        }
    }

    if (rootBdev) {
        pr_ok("storage", "Dynamically selected boot storage device: /dev/%s [%s] (label='%s')",
              rootBdev->getName(), rootBdev->getBusTypeName(), rootBdev->getLabel());
        return rootBdev;
    }

    // Strategy 2: If preferredDevName provided (e.g. from cmdline root=...)
    if (preferredDevName && preferredDevName[0]) {
        const char* cleanName = preferredDevName;
        if (str::compareN(cleanName, "/dev/", 5) == 0) {
            cleanName += 5;
        }

        IBlockDevice* named = reg.findBlockDevice(cleanName);
        if (named) {
            pr_ok("storage", "Using explicitly requested boot device: /dev/%s [%s]",
                  named->getName(), named->getBusTypeName());
            return named;
        }

        // Strategy 3: Dynamic fallback if NVMe was requested (e.g. nvme0n1p2) but machine has SATA
        if (str::compareN(cleanName, "nvme", 4) == 0 && !m_hasNvme && m_hasSata) {
            for (usize i = 0; i < reg.getBlockDeviceCount(); ++i) {
                IBlockDevice* dev = reg.getBlockDeviceAt(i);
                if (dev && (str::compare(dev->getName(), "sda2") == 0 || str::compare(dev->getName(), "sda1") == 0)) {
                    pr_warn("storage", "Requested root='/dev/%s' but NVMe is not present on this machine", cleanName);
                    pr_ok("storage", "Dynamically falling back to detected SATA drive: /dev/%s [%s]",
                          dev->getName(), dev->getBusTypeName());
                    return dev;
                }
            }
        }
    }

    return nullptr;
}

} // namespace cortez::drivers::storage
