/**
 * @file sys_commands.cpp
 * @brief Implementation of System, Hardware, Memory, and Process Shell Commands.
 */

#include "sys_commands.hpp"
#include "../shell.hpp"
#include "../line_editor.hpp"
#include "../text_editor.hpp"
#include "../../drivers/i_device.hpp"
#include "../../drivers/video/terminal_driver.hpp"
#include "../../drivers/storage/nvme_controller.hpp"
#include "../../drivers/storage/ahci_controller.hpp"
#include "../../drivers/storage/storage_manager.hpp"
#include "../../drivers/storage/os_installer.hpp"
#include "../../drivers/usb/xhci_controller.hpp"
#include "../../drivers/input/input_manager.hpp"
#include "../../arch/x86_64/pcie.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../arch/x86_64/io_ports.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../memory/pmm_bitmap.hpp"
#include "../../memory/heap_allocator.hpp"
#include "../../proc/scheduler.hpp"
#include "../../fs/vfs_manager.hpp"
#include "../../fs/fs_detector.hpp"
#include "../../klibc/klibc.hpp"
#include "../../lib/printk.hpp"

namespace cortez::shell::commands {

Status HelpCommand::execute(const CommandArgs&) {
    kprintf("\033[36mCortez Kernel Interactive Shell — Built-in Commands:\033[0m\n");
    usize count = Shell::instance().getCommandCount();
    for (usize i = 0; i < count; ++i) {
        const IShellCommand* cmd = Shell::instance().getCommandAt(i);
        if (!cmd) continue;
        kprintf("  \033[32m%-24s\033[0m %s\n", cmd->getUsage(), cmd->getDescription());
    }
    kprintf("  \033[32m%-24s\033[0m %s\n", "echo <text> > <file>", "Write/redirect text directly to an NVMe file");
    return Status::Ok;
}

Status ClearCommand::execute(const CommandArgs&) {
    drivers::video::TerminalDriver::instance().clear();
    return Status::Ok;
}

Status DmesgCommand::execute(const CommandArgs&) {
    const char* buf = klog::Logger::instance().getBuffer();
    usize len       = klog::Logger::instance().getBufferLength();
    klog::Logger::instance().writeRaw(buf, len);
    return Status::Ok;
}

Status LsblkCommand::execute(const CommandArgs&) {
    auto& reg = drivers::DeviceRegistry::instance();
    usize count = reg.getBlockDeviceCount();

    kprintf("  \033[90m%-12s %-6s %10s %12s %-10s  %s\033[0m\n",
            "NAME", "TYPE", "SECT-SIZE", "SIZE", "FS", "LABEL");

    for (usize i = 0; i < count; ++i) {
        drivers::IBlockDevice* bdev = reg.getBlockDeviceAt(i);
        if (!bdev) continue;
        u64 bytes = bdev->getBlockCount() * bdev->getBlockSize();
        u64 sizeMiB = bytes / (1024ULL * 1024ULL);
        char sizeStr[16]{};
        if (sizeMiB >= 1024) {
            klibc_snprintf(sizeStr, sizeof(sizeStr), "%llu.%llu GiB", sizeMiB / 1024ULL, (sizeMiB % 1024ULL) * 10ULL / 1024ULL);
        } else {
            klibc_snprintf(sizeStr, sizeof(sizeStr), "%llu MiB", sizeMiB);
        }
        auto probe = fs::FsDetector::probe(bdev);
        const char* fsStr = (probe.type != fs::DetectedFsType::Unknown) ? fs::FsDetector::typeToString(probe.type) : "-";
        const char* lbl = (probe.volumeLabel[0] != '\0') ? probe.volumeLabel : bdev->getLabel();
        kprintf("  /dev/%-7s %-6s %8u B %12s %-10s  %s\n",
                bdev->getName(),
                bdev->getBusTypeName(),
                bdev->getBlockSize(),
                sizeStr,
                fsStr,
                (lbl && lbl[0]) ? lbl : "-");
    }
    return Status::Ok;
}

Status LspciCommand::execute(const CommandArgs&) {
    auto& pcie = arch::PcieManager::instance();
    usize count = pcie.getDeviceCount();

    kprintf("  \033[90m%-10s %-11s %-10s %s\033[0m\n",
            "BDF", "VID:DID", "CLASS", "BAR0 (PHYS)");

    for (usize i = 0; i < count; ++i) {
        const auto& d = pcie.getDevice(i);
        kprintf("  %02x:%02x.%u    [%04x:%04x] %02x:%02x.%02x   0x%016llx\n",
                d.bus, d.device, d.function,
                d.vendorId, d.deviceId,
                d.classCode, d.subclass, d.progIf,
                d.bar0Phys);
    }
    return Status::Ok;
}

Status NvmeInfoCommand::execute(const CommandArgs&) {
    auto& nvme = drivers::storage::NvmeController::instance();
    if (!nvme.isReady()) {
        kprintf("\033[31mNVMe Controller is not initialized.\033[0m\n");
        return Status::DeviceNotFound;
    }

    auto* ns = nvme.getNamespace();
    u32 ver  = nvme.getVersion();
    kprintf("  \033[36mNVMe Controller Status:\033[0m\n");
    kprintf("    Model Number     : %s\n", nvme.getModel());
    kprintf("    Serial Number    : %s\n", nvme.getSerial());
    kprintf("    Firmware Rev     : %s\n", nvme.getFirmware());
    kprintf("    NVMe Spec Version: %u.%u.%u\n",
            (ver >> 16) & 0xFFFFU, (ver >> 8) & 0xFFU, ver & 0xFFU);
    if (ns) {
        u64 totalMiB = (ns->getBlockCount() * ns->getBlockSize()) / (1024ULL * 1024ULL);
        kprintf("    Active Namespace : /dev/%s (NSID %u)\n", ns->getName(), ns->getNsid());
        kprintf("    LBA Block Size   : %u bytes\n", ns->getBlockSize());
        kprintf("    Total Capacity   : %llu blocks (%llu MiB)\n", ns->getBlockCount(), totalMiB);
    }
    return Status::Ok;
}

Status AhciInfoCommand::execute(const CommandArgs&) {
    auto& ahci = drivers::storage::AhciController::instance();
    if (!ahci.isReady()) {
        kprintf("\033[33mNo active SATA AHCI Host Controller detected.\033[0m\n");
        return Status::DeviceNotFound;
    }

    u32 ver = ahci.getVersion();
    usize diskCount = ahci.getDiskCount();

    kprintf("  \033[36mSATA AHCI Controller Status:\033[0m\n");
    kprintf("    AHCI Spec Version : %u.%u\n", (ver >> 16) & 0xFFFFU, (ver >> 8) & 0xFFU);
    kprintf("    Attached Disks    : %zu\n", diskCount);

    if (diskCount == 0) {
        kprintf("    \033[90m(no SATA disks attached or detected)\033[0m\n");
        return Status::Ok;
    }

    for (usize i = 0; i < diskCount; ++i) {
        auto* disk = ahci.getDisk(i);
        if (!disk) continue;
        auto* port = disk->getPort();
        u64 sizeMiB = (disk->getBlockCount() * disk->getBlockSize()) / (1024ULL * 1024ULL);
        kprintf("\n    \033[32m/dev/%-4s\033[0m Port %u [%s]\n",
                disk->getName(),
                port ? port->getPortIndex() : 0,
                port ? port->getSpeedString() : "Unknown");
        kprintf("      Model Number   : %s\n", disk->getModel());
        kprintf("      Serial Number  : %s\n", disk->getSerial());
        kprintf("      Firmware Rev   : %s\n", disk->getFirmware());
        kprintf("      Sector Size    : %u bytes\n", disk->getBlockSize());
        kprintf("      Total Capacity : %llu sectors (%llu MiB)\n", disk->getBlockCount(), sizeMiB);
    }
    return Status::Ok;
}

Status StorageInfoCommand::execute(const CommandArgs&) {
    auto& mgr = drivers::storage::StorageManager::instance();
    auto& reg = drivers::DeviceRegistry::instance();

    kprintf("  \033[36mStorage Subsystem Inventory:\033[0m\n");
    kprintf("    PCIe Storage Controllers : %zu NVMe, %zu SATA AHCI, %zu xHCI USB\n",
            mgr.getNvmeControllerCount(),
            mgr.getSataControllerCount(),
            mgr.getUsbControllerCount());
    kprintf("    Active Drivers           : %s%s%s\n",
            mgr.hasNvme() ? "[NVMe] " : "",
            mgr.hasSata() ? "[SATA AHCI] " : "",
            mgr.hasUsb()  ? "[USB MSC] "  : "(none)");

    usize count = reg.getBlockDeviceCount();
    if (count == 0) {
        kprintf("    \033[90m(no block storage devices registered)\033[0m\n");
        return Status::Ok;
    }

    kprintf("\n  \033[36mRegistered Block Devices (%zu):\033[0m\n", count);
    for (usize i = 0; i < count; ++i) {
        auto* bdev = reg.getBlockDeviceAt(i);
        if (!bdev) continue;
        u64 sizeMiB = (bdev->getBlockCount() * bdev->getBlockSize()) / (1024ULL * 1024ULL);
        const char* lbl = bdev->getLabel();
        kprintf("    /dev/%-8s \033[33m[%-4s]\033[0m %8llu MiB  %s\n",
                bdev->getName(),
                bdev->getBusTypeName(),
                sizeMiB,
                (lbl && lbl[0]) ? lbl : "");
    }

    return Status::Ok;
}

Status LsusbCommand::execute(const CommandArgs&) {
    auto& usbMgr = drivers::usb::XhciManager::instance();
    usize ctrlCount = usbMgr.getControllerCount();
    if (ctrlCount == 0) {
        kprintf("\033[33mNo active xHCI USB Host Controllers detected.\033[0m\n");
        return Status::Ok;
    }

    kprintf("  \033[36mxHCI USB Host Controllers (%zu):\033[0m\n", ctrlCount);
    for (usize i = 0; i < ctrlCount; ++i) {
        const auto* ctrl = usbMgr.getController(i);
        if (!ctrl) continue;
        const auto& pci = ctrl->getPciInfo();
        u16 ver = ctrl->getHciVersion();
        kprintf("    [usb-bus %zu] PCIe %02x:%02x.%u [%04x:%04x] xHCI %x.%02x (%u ports, %u slots)\n",
                i, pci.bus, pci.device, pci.function,
                pci.vendorId, pci.deviceId,
                (ver >> 8) & 0xFFU, ver & 0xFFU,
                ctrl->getMaxPorts(), ctrl->getMaxSlots());
    }

    // 2. Physical Root Hub Ports & Dynamic Device Classification
    auto& portMgr = drivers::usb::UsbPortManager::instance();
    kprintf("\n  \033[36mPhysical USB Ports & Connected Devices:\033[0m\n");
    for (usize c = 0; c < ctrlCount; ++c) {
        const auto* ctrl = usbMgr.getController(c);
        if (!ctrl) continue;
        kprintf("  \033[1;37m[usb-bus %zu]\033[0m", c);
        usize protoCnt = ctrl->getProtocolCount();
        if (protoCnt > 0) {
            kprintf(" (Protocols:");
            for (usize p = 0; p < protoCnt; ++p) {
                const auto* pr = ctrl->getProtocol(p);
                if (pr) {
                    kprintf(" %s ports %u-%u;",
                            (pr->majorRev == 3) ? "USB 3.x" : "USB 2.0",
                            pr->portOffset, pr->portOffset + pr->portCount - 1);
                }
            }
            kprintf(")");
        }
        kprintf("\n");

        u8 maxPorts = ctrl->getMaxPorts();
        for (u8 port = 1; port <= maxPorts; ++port) {
            u8 maj = 0, min = 0;
            if (!ctrl->isPortValid(port, &maj, &min)) {
                continue;
            }
            auto* p = portMgr.findPortByBus(static_cast<u8>(c), port);
            if (!p) continue;

            const char* protoStr = p->getProtocolString();
            if (p->isConnected()) {
                const char* vName = p->getVendorName()[0] ? p->getVendorName() : "----";
                const char* pName = p->getProductName()[0] ? p->getProductName() : "----";
                kprintf("    Port %-2u [%-7s]: \033[32mConnected\033[0m    %-18s Slot %-2u [%s:%s] %s\n",
                        port,
                        protoStr,
                        p->getSpeedString(),
                        p->getSlotId(),
                        vName,
                        pName,
                        p->getAttachedTypeString());
            } else {
                kprintf("    Port %-2u [%-7s]: \033[90mDisconnected\033[0m (empty)\n",
                        port,
                        protoStr);
            }
        }
    }

    // 3. Active USB Keyboards
    usize kbdCount = usbMgr.getKeyboardCount();
    kprintf("\n  \033[36mUSB Keyboards (%zu):\033[0m\n", kbdCount);
    if (kbdCount == 0) {
        kprintf("    \033[90m(no USB keyboards attached)\033[0m\n");
    } else {
        auto* mainKbd = drivers::input::InputManager::instance().getMainKeyboard();
        for (usize i = 0; i < drivers::usb::XhciManager::MAX_USB_KBD_DEVICES; ++i) {
            auto* kbd = usbMgr.getKeyboardDevice(i);
            if (!kbd || !kbd->isActive()) continue;
            bool isMain = (kbd == mainKbd);
            kprintf("    /%-10s Port %-2u Slot %-2u \033[%sm[%s]\033[0m %s\n",
                    kbd->getName(), kbd->getPortNum(), kbd->getSlotId(),
                    isMain ? "32" : "90",
                    isMain ? "MAIN" : "SECONDARY",
                    isMain ? "(Active console stdin)" : "(Standby, switch with 'chkbd')");
        }
    }

    // 4. USB Mass Storage Block Devices
    usize mscCount = usbMgr.getMscDeviceCount();
    kprintf("\n  \033[36mUSB Mass Storage Block Devices (%zu):\033[0m\n", mscCount);
    if (mscCount == 0) {
        kprintf("    \033[90m(no USB Mass Storage drives attached)\033[0m\n");
    } else {
        for (usize i = 0; i < drivers::usb::XhciManager::MAX_USB_MSC_DEVICES; ++i) {
            const auto* dev = usbMgr.getMscDeviceRaw(i);
            if (!dev || !dev->isActive() || !dev->isReady()) continue;
            u64 sizeMiB = (dev->getBlockCount() * dev->getBlockSize()) / (1024ULL * 1024ULL);
            kprintf("    /dev/%-6s Port %-2u Slot %-2u [%04x:%04x] %-18s '%s %s' (%llu MiB)\n",
                    dev->getName(),
                    dev->getPortNumber(),
                    dev->getSlotId(),
                    dev->getUsbVendorId(),
                    dev->getUsbProductId(),
                    drivers::usb::XhciManager::speedToString(dev->getSpeedId()),
                    dev->getVendorString(),
                    dev->getProductString(),
                    sizeMiB);
        }
    }
    return Status::Ok;
}

Status PsCommand::execute(const CommandArgs&) {
    auto& sched = proc::Scheduler::instance();
    usize count = sched.getTaskCount();

    kprintf("  \033[90m%-6s %-16s %-16s %-10s %s\033[0m\n",
            "PID", "NAME", "STATE", "TICKS", "CWD");

    for (usize i = 0; i < count; ++i) {
        const proc::Task* t = sched.getTaskAt(i);
        if (!t) continue;
        kprintf("  %-6u \033[36m%-16s\033[0m %-16s %-10llu %s\n",
                t->getPid(),
                t->getName(),
                proc::taskStateToString(t->getState()),
                t->getTicks(),
                t->getCwd());
    }
    return Status::Ok;
}

Status FreeCommand::execute(const CommandArgs&) {
    auto& pmm  = memory::BitmapPmm::instance();
    auto& heap = memory::KernelHeap::instance();

    kprintf("  \033[90m%-18s %14s %14s %14s\033[0m\n",
            "MEMORY POOL", "TOTAL (KiB)", "USED (KiB)", "FREE (KiB)");

    kprintf("  %-18s %14llu %14llu %14llu\n",
            "Physical RAM (PMM)",
            pmm.getTotalMemoryBytes() / 1024ULL,
            pmm.getUsedMemoryBytes() / 1024ULL,
            pmm.getFreeMemoryBytes() / 1024ULL);

    usize heapTotal = heap.getTotalBytes();
    usize heapUsed  = heap.getUsedBytes();
    kprintf("  %-18s %14zu %14zu %14zu\n",
            "Kernel Heap",
            heapTotal / 1024,
            heapUsed / 1024,
            (heapTotal - heapUsed) / 1024);

    return Status::Ok;
}

Status UnameCommand::execute(const CommandArgs&) {
    u64 uptimeSec = arch::ApicController::instance().getUptimeMs() / 1000ULL;
    kprintf("Cortez Kernel 1.0.0-x86_64 (SMP/UEFI Monolithic C++20, APIC=%s, Uptime=%llus)\n",
            arch::ApicController::instance().isX2Apic() ? "x2APIC" : "xAPIC",
            uptimeSec);
    return Status::Ok;
}

Status RebootCommand::execute(const CommandArgs&) {
    kprintf("Syncing root filesystem before reboot...\n");
    (void)fs::VfsManager::instance().sync();
    kprintf("Filesystem synced. Triggering hardware reset...\n");

    arch::cli();

    // 1. Intel PCH / AMD FCH / QEMU q35 PCI Reset Control Register (Port 0xCF9)
    u8 cf9 = arch::inb(0xCF9) & ~0x0EU;
    arch::outb(0xCF9, cf9 | 0x02U); // Assert SYS_RST (warm/system reset mode)
    arch::ioWait();
    arch::outb(0xCF9, cf9 | 0x06U); // Pulse RST_CPU | SYS_RST
    for (u32 i = 0; i < 100000; ++i) arch::pause();
    arch::outb(0xCF9, cf9 | 0x0EU); // Full cold reset (FULL_RST | RST_CPU | SYS_RST)
    for (u32 i = 0; i < 100000; ++i) arch::pause();

    // 2. Legacy 8042 Keyboard Controller / Laptop EC CPU Reset line (Port 0x64 <- 0xFE)
    for (u32 i = 0; i < 16384; ++i) {
        if ((arch::inb(0x64) & 0x02U) == 0) break;
    }
    arch::outb(0x64, 0xFE);
    for (u32 i = 0; i < 100000; ++i) arch::pause();

    // 3. System Control Port A Fast Reset (Port 0x92 bit 0)
    u8 port92 = arch::inb(0x92);
    arch::outb(0x92, port92 & ~0x01U);
    arch::ioWait();
    arch::outb(0x92, port92 | 0x01U);
    for (u32 i = 0; i < 100000; ++i) arch::pause();

    // 4. Architectural CPU Triple Fault fallback (guaranteed reset on all x86_64 CPUs)
    arch::IdtDescriptor nullIdt{ 0, 0 };
    asm volatile("lidt (%0); int3" :: "r"(&nullIdt) : "memory");

    arch::haltForever();
}

Status HistoryCommand::execute(const CommandArgs&) {
    auto& le    = LineEditor::instance();
    usize count = le.getHistoryCount();
    if (count == 0) {
        kprintf("  \033[90m(history is empty)\033[0m\n");
        return Status::Ok;
    }
    for (usize i = 0; i < count; ++i) {
        const char* entry = le.getHistoryAt(i);
        if (entry) {
            kprintf("  \033[90m%3zu\033[0m  %s\n", i + 1, entry);
        }
    }
    return Status::Ok;
}

namespace {
Status launchNanoEditor(const CommandArgs& args) noexcept {
    const proc::Task* t   = proc::Scheduler::instance().getCurrentTask();
    const char*       cwd = t ? t->getCwd() : "/";

    const char* filePath = nullptr;
    usize initLine       = 0;
    usize initCol        = 0;

    for (usize i = 1; i < args.argc; ++i) {
        const char* arg = args.argv[i];
        if (!arg || !arg[0]) continue;
        if (arg[0] == '+' && arg[1] >= '0' && arg[1] <= '9') {
            char* endPtr = nullptr;
            long ln = klibc_strtol(arg + 1, &endPtr, 10);
            if (ln > 0) initLine = static_cast<usize>(ln);
            if (endPtr && (*endPtr == ',' || *endPtr == ':')) {
                long col = klibc_strtol(endPtr + 1, nullptr, 10);
                if (col > 0) initCol = static_cast<usize>(col);
            }
        } else {
            filePath = arg;
        }
    }

    return TextEditor::openAndEdit(cwd, filePath, initLine, initCol);
}
} // namespace

Status NanoCommand::execute(const CommandArgs& args) {
    return launchNanoEditor(args);
}

Status EditCommand::execute(const CommandArgs& args) {
    return launchNanoEditor(args);
}

Status ChkbdCommand::execute(const CommandArgs& args) {
    auto& inMgr = drivers::input::InputManager::instance();
    auto* mainKbd = inMgr.getMainKeyboard();

    if (args.argc <= 1) {
        kprintf("  \033[36mConsole Main Keyboard:\033[0m \033[32m%s\033[0m\n",
                mainKbd ? mainKbd->getName() : "none");
        usize count = inMgr.getDeviceCount();
        kprintf("  \033[36mRegistered Keyboards (%zu):\033[0m\n", count);
        for (usize i = 0; i < count; ++i) {
            auto* dev = inMgr.getDeviceAt(i);
            if (!dev) continue;
            bool isMain = (dev == mainKbd);
            const char* typeDesc = "Input Device";
            if (str::compare(dev->getName(), "kbd0") == 0) {
                typeDesc = "PS/2 Keyboard";
            } else if (str::compareN(dev->getName(), "ukbd", 4) == 0) {
                typeDesc = "USB Keyboard";
            }
            kprintf("    [%zu] %-8s (%s) %s\n",
                    i, dev->getName(), typeDesc,
                    isMain ? "\033[32m[MAIN KEYBOARD]\033[0m" : "\033[90m[SECONDARY]\033[0m");
        }
        kprintf("  Usage: chkbd <device_name> (e.g. chkbd ukbd0, chkbd kbd0)\n");
        return Status::Ok;
    }

    const char* targetName = args.argv[1];
    Status st = inMgr.setMainKeyboard(targetName);
    if (st == Status::Ok) {
        kprintf("\033[32mSuccessfully switched MAIN keyboard to '%s'.\033[0m\n", targetName);
    } else {
        kprintf("\033[31mError:\033[0m Keyboard device '%s' not found.\n", targetName);
        kprintf("Available keyboards: ");
        usize count = inMgr.getDeviceCount();
        for (usize i = 0; i < count; ++i) {
            auto* dev = inMgr.getDeviceAt(i);
            if (dev) kprintf("%s%s", dev->getName(), (i + 1 < count) ? ", " : "\n");
        }
    }
    return Status::Ok;
}

Status InstallCommand::execute(const CommandArgs& args) {
    auto& inst = drivers::storage::OsInstaller::instance();

    if (args.argc >= 2) {
        const char* targetName = args.argv[1];
        kprintf("\033[36mInitiating Cortez OS installation onto target disk '%s'...\033[0m\n", targetName);
        return inst.installByName(targetName);
    }

    drivers::storage::DiskInstallInfo disks[drivers::storage::OsInstaller::MAX_TARGET_DISKS]{};
    usize count = 0;
    Status st = inst.scanCandidateDisks(disks, drivers::storage::OsInstaller::MAX_TARGET_DISKS, &count);
    if (st != Status::Ok || count == 0) {
        kprintf("\033[33mNo candidate physical internal disks found for installation.\033[0m\n");
        kprintf("Ensure an internal NVMe SSD or SATA hard drive is connected (not the live boot USB).\n");
        return Status::NotFound;
    }

    kprintf("\n\033[36m=================================================================\033[0m\n");
    kprintf("\033[36m           Cortez Operating System — Disk Installer              \033[0m\n");
    kprintf("\033[36m=================================================================\033[0m\n");
    kprintf("Discovered Candidate Disks (%zu):\n\n", count);

    int suitableIndex = -1;
    usize suitableCount = 0;

    for (usize i = 0; i < count; ++i) {
        const auto& d = disks[i];
        kprintf("  [%zu] \033[1m/dev/%-10s\033[0m (%llu MiB) [%s] - %s\n",
                i + 1, d.name, d.totalSizeMiB, d.disk ? d.disk->getBusTypeName() : "Unknown",
                d.hasEnoughSpace ? "\033[32mEligible (Free Space Found)\033[0m" : "\033[33mInsufficient Free Space\033[0m");
        kprintf("       Existing Partitions: %u | EFI System Partition: %s\n",
                d.existingPartCount,
                d.hasEsp ? d.espPartName : "None (will create 64 MiB ESP)");
        kprintf("       Largest Unallocated Space: \033[32m%llu MiB\033[0m (LBA %llu..%llu)\n\n",
                d.largestFree.sizeMiB, d.largestFree.startLba, d.largestFree.endLba);

        if (d.hasEnoughSpace) {
            suitableIndex = static_cast<int>(i);
            ++suitableCount;
        }
    }

    if (suitableCount == 0) {
        kprintf("\033[33mNone of the candidate disks have at least 128 MiB of unallocated space.\033[0m\n");
        kprintf("Please shrink an existing partition in Windows or Linux to free up unallocated space,\n");
        kprintf("then re-run 'install'.\n");
        return Status::NoSpace;
    }

    if (suitableCount == 1 && suitableIndex >= 0) {
        const auto& chosen = disks[suitableIndex];
        kprintf("\033[32mAuto-selecting candidate disk /dev/%s for installation...\033[0m\n\n", chosen.name);
        return inst.installToDisk(chosen.disk);
    }

    kprintf("\033[36mMultiple disks with unallocated space detected.\033[0m\n");
    kprintf("Specify the target disk to install to:\n");
    kprintf("  \033[1minstall /dev/<disk_name>\033[0m  (e.g. install /dev/%s)\n\n",
            disks[suitableIndex].name);

    return Status::Ok;
}

} // namespace cortez::shell::commands
