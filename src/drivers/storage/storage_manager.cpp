/**
 * @file storage_manager.cpp
 * @brief Implementation of unified dynamic storage orchestrator.
 */

#include "storage_manager.hpp"
#include "nvme_controller.hpp"
#include "ahci_controller.hpp"
#include "gpt_partition.hpp"
#include "../usb/xhci_controller.hpp"
#include "../../fs/vfs_manager.hpp"
#include "../../fs/exfat/exfat_fs.hpp"
#include "../../fs/fat/fat32_fs.hpp"
#include "../../fs/fs_detector.hpp"
#include "mbr_partition.hpp"
#include "../../arch/x86_64/pcie.hpp"
#include "../../klibc/klibc.hpp"
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

    pr_dbg("storage", "PCIe storage scan: %zu NVMe, %zu SATA AHCI, %zu xHCI controller(s) detected",
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

        // Strategy 4: Dynamic fallback to USB drive if NVMe/SATA is not present on this machine
        if (m_hasUsb) {
            for (usize i = 0; i < reg.getBlockDeviceCount(); ++i) {
                IBlockDevice* dev = reg.getBlockDeviceAt(i);
                if (dev && (str::compare(dev->getName(), "usb0p2") == 0 ||
                            str::compare(dev->getName(), "usb0p1") == 0 ||
                            str::compare(dev->getName(), "usb0") == 0)) {
                    pr_warn("storage", "Requested root='/dev/%s' but NVMe/SATA is not present on this machine", cleanName);
                    pr_ok("storage", "Dynamically falling back to detected USB drive: /dev/%s [%s]",
                          dev->getName(), dev->getBusTypeName());
                    return dev;
                }
            }
        }
    }

    // Strategy 5: Generic USB boot fallback if no root was found
    if (m_hasUsb) {
        for (usize i = 0; i < reg.getBlockDeviceCount(); ++i) {
            IBlockDevice* dev = reg.getBlockDeviceAt(i);
            if (dev && (str::compare(dev->getName(), "usb0p2") == 0 ||
                        str::compare(dev->getName(), "usb0p1") == 0 ||
                        str::compare(dev->getName(), "usb0") == 0)) {
                pr_ok("storage", "Dynamically selected USB boot storage: /dev/%s [%s]",
                      dev->getName(), dev->getBusTypeName());
                return dev;
            }
        }
    }

    return nullptr;
}

static fs::IFileSystem* createAndMountFs(drivers::IBlockDevice* dev) {
    if (!dev) return nullptr;
    auto probe = fs::FsDetector::probe(dev);
    pr_dbg("storage", "/dev/%s detected filesystem: %s (label='%s')",
            dev->getName(), fs::FsDetector::typeToString(probe.type), probe.volumeLabel);

    if (probe.type == fs::DetectedFsType::Fat32 ||
        probe.type == fs::DetectedFsType::Fat16 ||
        probe.type == fs::DetectedFsType::Fat12) {
        auto* fatFs = new fs::fat::Fat32FileSystem();
        if (fatFs && fatFs->mount(dev) == Status::Ok) {
            return fatFs;
        }
        delete fatFs;
    } else if (probe.type == fs::DetectedFsType::ExFat) {
        auto* exFs = new fs::exfat::ExFatFileSystem();
        if (exFs && exFs->mount(dev) == Status::Ok) {
            return exFs;
        }
        delete exFs;
    }

    // Dynamic fallback: attempt FAT32 then exFAT
    auto* fatFs = new fs::fat::Fat32FileSystem();
    if (fatFs && fatFs->mount(dev) == Status::Ok) {
        return fatFs;
    }
    delete fatFs;

    auto* exFs = new fs::exfat::ExFatFileSystem();
    if (exFs && exFs->mount(dev) == Status::Ok) {
        return exFs;
    }
    delete exFs;

    return nullptr;
}

void StorageManager::onUsbStorageConnected(IBlockDevice* dev, const char*) noexcept {
    if (!dev) return;

    if (!fs::VfsManager::instance().isRootMounted()) {
        for (usize i = 0; i < m_pendingUsbCount; ++i) {
            if (m_pendingUsbDevices[i] == dev) return;
        }
        if (m_pendingUsbCount < MAX_PENDING_USB_DEVICES) {
            m_pendingUsbDevices[m_pendingUsbCount++] = dev;
        }
        return;
    }

    // Do not mount secondary /usb if this device or its partition is the root filesystem
    if (auto* rootDev = fs::VfsManager::instance().getRootBlockDevice()) {
        if (rootDev == dev) return;
        char mscPartPrefix[32]{};
        klibc_snprintf(mscPartPrefix, sizeof(mscPartPrefix), "%sp", dev->getName());
        if (str::compareN(rootDev->getName(), mscPartPrefix, str::length(mscPartPrefix)) == 0) {
            return;
        }
    }

    // Do not mount if device or its partition is already mounted
    for (usize mIdx = 0; mIdx < fs::VfsManager::instance().getMountCount(); ++mIdx) {
        const auto* m = fs::VfsManager::instance().getMountAt(mIdx);
        if (!m) continue;
        if (m->bdev == dev) return;
        char prefix[32]{};
        klibc_snprintf(prefix, sizeof(prefix), "%sp", dev->getName());
        if (m->bdev && str::compareN(m->bdev->getName(), prefix, str::length(prefix)) == 0) {
            return;
        }
    }

    auto allocateMountPoint = [](char* outPath, usize maxLen) {
        bool isUsbMounted = false;
        for (usize i = 0; i < fs::VfsManager::instance().getMountCount(); ++i) {
            const auto* m = fs::VfsManager::instance().getMountAt(i);
            if (m && str::compare(m->path, "/usb") == 0) {
                isUsbMounted = true;
                break;
            }
        }
        if (!isUsbMounted) {
            str::copy(outPath, "/usb", maxLen);
            (void)fs::VfsManager::instance().mkdir("/", "/usb");
            return;
        }
        for (u32 idx = 1; idx <= 16; ++idx) {
            char candidate[32]{};
            klibc_snprintf(candidate, sizeof(candidate), "/usb%u", idx);
            bool used = false;
            for (usize i = 0; i < fs::VfsManager::instance().getMountCount(); ++i) {
                const auto* m = fs::VfsManager::instance().getMountAt(i);
                if (m && str::compare(m->path, candidate) == 0) {
                    used = true;
                    break;
                }
            }
            if (!used) {
                str::copy(outPath, candidate, maxLen);
                (void)fs::VfsManager::instance().mkdir("/", candidate);
                return;
            }
        }
        str::copy(outPath, "/usb", maxLen);
    };

    // 1. Try mounting partitions 1..4 first if partition devices exist
    bool mountedAnyPartition = false;
    for (u32 part = 1; part <= 4; ++part) {
        char partName[32]{};
        klibc_snprintf(partName, sizeof(partName), "%sp%u", dev->getName(), part);
        if (auto* pDev = DeviceRegistry::instance().findBlockDevice(partName)) {
            if (auto* pFs = createAndMountFs(pDev)) {
                char mp[64]{};
                allocateMountPoint(mp, sizeof(mp));
                (void)fs::VfsManager::instance().mount(mp, pFs, pDev);
                pr_ok("usb", "Mounted USB storage /dev/%s on '%s'", pDev->getName(), mp);
                mountedAnyPartition = true;
            }
        }
    }

    if (mountedAnyPartition) return;

    // 2. Try mounting unpartitioned / superfloppy directly on dev
    if (auto* fs = createAndMountFs(dev)) {
        char mp[64]{};
        allocateMountPoint(mp, sizeof(mp));
        (void)fs::VfsManager::instance().mount(mp, fs, dev);
        pr_ok("usb", "Mounted USB storage /dev/%s on '%s'", dev->getName(), mp);
        return;
    }

    // 3. Attached device could not be mounted
    pr_warn("usb", "Attached USB storage (/dev/%s) has no supported filesystem", dev->getName());
}

void StorageManager::onUsbStorageDisconnected(IBlockDevice* dev) noexcept {
    if (!dev) return;

    // Never unmount or disconnect the live root filesystem
    auto* rootDev = fs::VfsManager::instance().getRootBlockDevice();
    if (rootDev == dev) return;
    char rootPrefix[32]{};
    klibc_snprintf(rootPrefix, sizeof(rootPrefix), "%sp", dev->getName());
    if (rootDev && str::compareN(rootDev->getName(), rootPrefix, str::length(rootPrefix)) == 0) {
        return;
    }

    // 1. Unmount all mounts matching this disk or any of its partitions (never unmount "/")
    bool unmountedAny = false;
    do {
        unmountedAny = false;
        for (usize i = 0; i < fs::VfsManager::instance().getMountCount(); ++i) {
            const auto* m = fs::VfsManager::instance().getMountAt(i);
            if (!m) continue;
            if (str::compare(m->path, "/") == 0) continue;

            bool matches = false;
            if (m->bdev == dev) {
                matches = true;
            } else {
                char prefix[32]{};
                klibc_snprintf(prefix, sizeof(prefix), "%sp", dev->getName());
                if (m->bdev && str::compareN(m->bdev->getName(), prefix, str::length(prefix)) == 0) {
                    matches = true;
                }
            }

            if (matches) {
                char mpath[64]{};
                str::copy(mpath, m->path, sizeof(mpath));
                char devName[32]{};
                if (m->bdev) {
                    str::copy(devName, m->bdev->getName(), sizeof(devName));
                } else {
                    str::copy(devName, dev->getName(), sizeof(devName));
                }
                auto* targetFs = m->fs;
                (void)fs::VfsManager::instance().unmount(mpath);
                delete targetFs;
                pr_info("usb", "Unmounted USB storage /dev/%s from '%s'", devName, mpath);
                unmountedAny = true;
                break; // Restart scan since mount list shifted
            }
        }
    } while (unmountedAny);

    // 2. Unregister and purge partitions from DeviceRegistry
    MbrPartitionScanner::instance().unregisterDiskPartitions(dev);
    GptPartitionScanner::instance().unregisterDiskPartitions(dev);

    // 3. Unregister the raw block device itself
    DeviceRegistry::instance().unregisterBlockDevice(dev);
}

void StorageManager::mountAllUsbStorages() noexcept {
    // 1. Drain pending early-boot USB storage devices
    for (usize i = 0; i < m_pendingUsbCount; ++i) {
        if (auto* dev = m_pendingUsbDevices[i]) {
            char cleanName[32]{};
            klibc_snprintf(cleanName, sizeof(cleanName), "USB_Drive_%zu", i);
            onUsbStorageConnected(dev, cleanName);
        }
    }
    m_pendingUsbCount = 0;

    // 2. Scan all attached MSC devices in XhciManager
    for (usize i = 0; i < drivers::usb::XhciManager::MAX_USB_MSC_DEVICES; ++i) {
        auto* msc = drivers::usb::XhciManager::instance().getMscDeviceRaw(i);
        if (!msc || !msc->isActive() || !msc->isReady()) continue;

        bool isRoot = false;
        if (auto* rootDev = fs::VfsManager::instance().getRootBlockDevice()) {
            if (rootDev == msc) isRoot = true;
            char mscPartPrefix[32]{};
            klibc_snprintf(mscPartPrefix, sizeof(mscPartPrefix), "%sp", msc->getName());
            if (str::compareN(rootDev->getName(), mscPartPrefix, str::length(mscPartPrefix)) == 0) {
                isRoot = true;
            }
        }
        if (isRoot) continue;

        // Check if device or any of its partitions are already mounted
        bool alreadyMounted = false;
        for (usize mIdx = 0; mIdx < fs::VfsManager::instance().getMountCount(); ++mIdx) {
            const auto* m = fs::VfsManager::instance().getMountAt(mIdx);
            if (!m) continue;
            if (m->bdev == msc) {
                alreadyMounted = true;
                break;
            }
            char prefix[32]{};
            klibc_snprintf(prefix, sizeof(prefix), "%sp", msc->getName());
            if (m->bdev && str::compareN(m->bdev->getName(), prefix, str::length(prefix)) == 0) {
                alreadyMounted = true;
                break;
            }
        }
        if (alreadyMounted) continue;

        const char* prodName = msc->getProductString();
        char cleanName[32]{};
        if (prodName && prodName[0] != '\0') {
            klibc_snprintf(cleanName, sizeof(cleanName), "%s", prodName);
        } else {
            klibc_snprintf(cleanName, sizeof(cleanName), "USB_Drive_%zu", i);
        }
        onUsbStorageConnected(msc, cleanName);
    }
}

} // namespace cortez::drivers::storage
