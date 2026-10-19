/**
 * @file kernel.cpp
 * @brief Implementation of the Linux-style Cortez Kernel boot sequence (start_kernel & kernel_init).
 */

#include "kernel.hpp"
#include "../boot/boot_info.hpp"
#include "../boot/cmdline.hpp"
#include "../arch/x86_64/cpu.hpp"
#include "../arch/x86_64/gdt.hpp"
#include "../arch/x86_64/idt.hpp"
#include "../arch/x86_64/acpi.hpp"
#include "../arch/x86_64/apic.hpp"
#include "../arch/x86_64/pcie.hpp"
#include "../memory/pmm_bitmap.hpp"
#include "../memory/vmm_pager.hpp"
#include "../memory/heap_allocator.hpp"
#include "../memory/module_area.hpp"
#include "../drivers/i_device.hpp"
#include "../drivers/video/terminal_driver.hpp"
#include "../drivers/input/ps2_keyboard.hpp"
#include "../drivers/storage/nvme_controller.hpp"
#include "../drivers/storage/gpt_partition.hpp"
#include "../drivers/usb/xhci_controller.hpp"
#include "../fs/vfs_manager.hpp"
#include "../proc/scheduler.hpp"
#include "../shell/shell.hpp"
#include "../module/module_manager.hpp"
#include "../lib/printk.hpp"
#include "../lib/string_utils.hpp"

extern "C" void start_kernel() {
    cortez::kernel::Kernel::instance().start();
}

namespace cortez::kernel {

Kernel& Kernel::instance() noexcept {
    static Kernel s_kernel;
    return s_kernel;
}

void Kernel::setupArch() noexcept {
    auto& boot = boot::BootInfo::instance();
    if (boot.hasFramebuffer()) {
        auto& term = drivers::video::TerminalDriver::instance();
        if (term.init(boot.getFramebuffer()) == Status::Ok) {
            drivers::DeviceRegistry::instance().registerCharDevice(&term);
            klog::Logger::instance().setConsoleDevice(&term);
        }
    }

    pr_info("boot", "Cortez Kernel v1.0.0-x86_64 (C++20 Monolithic UEFI Kernel)");
    pr_info("boot", "Kernel command line: %s", boot::KernelCmdline::instance().getRaw());

    arch::GdtManager::instance().init();
    arch::IdtManager::instance().init();
    arch::enableFpuSse();
    pr_ok("cpu", "Enabled x87 FPU and SSE/SSE2 instructions (CR0.EM=0, CR4.OSFXSR=1)");
}

void Kernel::mmInit() noexcept {
    auto& boot = boot::BootInfo::instance();
    u64 hhdm   = boot.getHhdmOffset();

    if (memory::BitmapPmm::instance().init(boot.getMemmap(), hhdm) != Status::Ok) {
        pr_err("PANIC", "Failed to initialize Physical Memory Manager (PMM)");
        arch::haltForever();
    }

    if (memory::PageTableVmm::instance().init(&memory::BitmapPmm::instance(), hhdm) != Status::Ok) {
        pr_err("PANIC", "Failed to initialize 4-Level Virtual Memory Manager (VMM)");
        arch::haltForever();
    }

    if (memory::KernelHeap::instance().init(&memory::BitmapPmm::instance(), hhdm) != Status::Ok) {
        pr_err("PANIC", "Failed to initialize Kernel Heap Allocator");
        arch::haltForever();
    }

    if (memory::ModuleArea::instance().init(&memory::BitmapPmm::instance(), &memory::PageTableVmm::instance()) != Status::Ok) {
        pr_err("PANIC", "Failed to initialize Executable Module Area");
        arch::haltForever();
    }
}

void Kernel::initIrqAndTimers() noexcept {
    auto& boot = boot::BootInfo::instance();
    arch::AcpiManager::instance().init(boot.getRsdpPhysAddr(), boot.getHhdmOffset());

    if (arch::ApicController::instance().init(&memory::PageTableVmm::instance()) != Status::Ok) {
        pr_err("PANIC", "Failed to initialize Local APIC / I/O APIC");
        arch::haltForever();
    }

    auto& kbd = drivers::input::Ps2KeyboardDriver::instance();
    if (kbd.init() == Status::Ok) {
        drivers::DeviceRegistry::instance().registerCharDevice(&kbd);
    }
}

void Kernel::doBasicSetup() noexcept {
    auto& boot = boot::BootInfo::instance();
    auto& pmm  = memory::BitmapPmm::instance();
    auto& vmm  = memory::PageTableVmm::instance();

    arch::PcieManager::instance().init(&vmm);

    auto& nvme = drivers::storage::NvmeController::instance();
    if (nvme.init(&pmm, &vmm, boot.getHhdmOffset()) == Status::Ok) {
        if (auto* ns = nvme.getNamespace()) {
            drivers::storage::GptPartitionScanner::instance().scanDisk(ns);
        }
    }

    drivers::usb::XhciManager::instance().init(&pmm, &vmm, boot.getHhdmOffset());
}

void Kernel::mountBlockRoot() noexcept {
    // Only ever mount a partition that the Cortez image builder created (GPT name "Cortez-RootFS").
    // On real hardware /dev/nvme0n1p2 is typically the host OS (e.g. Ubuntu's ext4 root), so we must
    // never fall back to a device purely by name — and never suggest formatting it.
    static constexpr const char* ROOTFS_LABEL = "Cortez-RootFS";

    const char* rootDevName = boot::KernelCmdline::instance().getRootDevice();
    auto&       reg         = drivers::DeviceRegistry::instance();

    drivers::IBlockDevice* rootBdev = nullptr;
    for (usize i = 0; i < reg.getBlockDeviceCount(); ++i) {
        drivers::IBlockDevice* dev = reg.getBlockDeviceAt(i);
        if (dev && str::compare(dev->getLabel(), ROOTFS_LABEL) == 0) {
            rootBdev = dev;
            break;
        }
    }

    if (!rootBdev) {
        drivers::IBlockDevice* named = reg.findBlockDevice(rootDevName);
        if (named) {
            pr_warn("vfs", "VFS: /dev/%s (label='%s') is not a '%s' partition; refusing to mount it",
                    named->getName(), named->getLabel(), ROOTFS_LABEL);
        }
        pr_err("vfs", "VFS: No '%s' partition found (root='%s'); continuing without a root filesystem",
               ROOTFS_LABEL, rootDevName);
        return;
    }

    Status st = fs::VfsManager::instance().mountBlockRoot(rootBdev);
    if (st != Status::Ok) {
        pr_warn("vfs", "VFS: Failed to mount /dev/%s (%s); run 'mkfs.exfat %s' in shell",
                rootBdev->getName(), statusToString(st), rootBdev->getName());
        return;
    }

    // Bind standard console streams (fd 0=stdin, fd 1=stdout, fd 2=stderr)
    fs::VfsManager::instance().getKernelFdTable().bindStdStreams(
        &drivers::input::Ps2KeyboardDriver::instance(),
        &drivers::video::TerminalDriver::instance(),
        &drivers::video::TerminalDriver::instance());

    // Persist early kernel boot log to /var/log/dmesg.log on the physical root disk
    (void)fs::VfsManager::instance().mkdir("/", "/var");
    (void)fs::VfsManager::instance().mkdir("/", "/var/log");
    const char* klogBuf = klog::Logger::instance().getBuffer();
    usize       klogLen = klog::Logger::instance().getBufferLength();
    if (klogLen > 0) {
        if (fs::VfsManager::instance().writeFileAll("/", "/var/log/dmesg.log", klogBuf, klogLen) == Status::Ok) {
            pr_ok("vfs", "Persisted kernel boot log (%zu bytes) to /var/log/dmesg.log on /dev/%s",
                  klogLen, rootBdev->getName());
        }
    }
}

void Kernel::kernelInitTask() {
    pr_info("init", "PID 1 (kernel_init) starting device probe and rootfs mount...");
    Kernel::instance().doBasicSetup();
    Kernel::instance().mountBlockRoot();

    shell::Shell::instance().init();

    if (fs::VfsManager::instance().isRootMounted()) {
        if (boot::KernelCmdline::instance().isNoExt()) {
            pr_warn("ext", "Boot flag 'noext' detected; skipping /ext C module autoload");
        } else {
            pr_info("ext", "Autoloading C kernel extensions from /ext/drivers and /ext/commands...");
            usize drvCount = module::ModuleManager::instance().autoload("/ext/drivers");
            usize cmdCount = module::ModuleManager::instance().autoload("/ext/commands");
            pr_ok("ext", "Autoloaded %zu C driver(s) and %zu C command module(s)", drvCount, cmdCount);
        }
    }

    shell::Shell::instance().runInteractiveLoop();
}

void Kernel::kworkerTask() {
    for (;;) {
        proc::Scheduler::instance().yield();
    }
}

void Kernel::start() noexcept {
    klog::Logger::instance().initSerial();

    if (boot::BootInfo::instance().init() != Status::Ok) {
        arch::haltForever();
    }
    boot::KernelCmdline::instance().parse(boot::BootInfo::instance().getCmdline());

    setupArch();
    mmInit();
    initIrqAndTimers();

    if (proc::Scheduler::instance().init(&Kernel::kernelInitTask, &Kernel::kworkerTask) != Status::Ok) {
        pr_err("PANIC", "Failed to initialize task scheduler");
        arch::haltForever();
    }

    arch::sti();

    // PID 0 (swapper/0) idle loop
    for (;;) {
        proc::Scheduler::instance().yield();
        arch::hlt();
    }
}

} // namespace cortez::kernel
