/**
 * @file sys_commands.cpp
 * @brief Implementation of System, Hardware, Memory, and Process Shell Commands.
 */

#include "sys_commands.hpp"
#include "../shell.hpp"
#include "../../drivers/i_device.hpp"
#include "../../drivers/video/terminal_driver.hpp"
#include "../../drivers/storage/nvme_controller.hpp"
#include "../../drivers/usb/xhci_controller.hpp"
#include "../../arch/x86_64/pcie.hpp"
#include "../../arch/x86_64/apic.hpp"
#include "../../arch/x86_64/io_ports.hpp"
#include "../../arch/x86_64/cpu.hpp"
#include "../../memory/pmm_bitmap.hpp"
#include "../../memory/heap_allocator.hpp"
#include "../../proc/scheduler.hpp"
#include "../../fs/vfs_manager.hpp"
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

    kprintf("  \033[90m%-14s %10s %14s %12s  %s\033[0m\n",
            "NAME", "SECT-SIZE", "SECTORS", "SIZE (MiB)", "LABEL");

    for (usize i = 0; i < count; ++i) {
        drivers::IBlockDevice* bdev = reg.getBlockDeviceAt(i);
        if (!bdev) continue;
        u64 bytes = bdev->getBlockCount() * bdev->getBlockSize();
        const char* lbl = bdev->getLabel();
        kprintf("  /dev/%-9s %8u B %14llu %9llu MiB  %s\n",
                bdev->getName(),
                bdev->getBlockSize(),
                bdev->getBlockCount(),
                bytes / (1024ULL * 1024ULL),
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

    usize mscCount = usbMgr.getMscDeviceCount();
    if (mscCount == 0) {
        kprintf("  \033[90m(no USB Mass Storage drives attached)\033[0m\n");
        return Status::Ok;
    }

    kprintf("\n  \033[36mUSB Mass Storage Block Devices (%zu):\033[0m\n", mscCount);
    for (usize i = 0; i < mscCount; ++i) {
        const auto* dev = usbMgr.getMscDevice(i);
        if (!dev || !dev->isReady()) continue;
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

} // namespace cortez::shell::commands
