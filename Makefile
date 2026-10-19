# =============================================================================
# Cortez Kernel — Build System (Clang++ C++20 Freestanding + NASM + LLD)
# =============================================================================

# Clear GNU Make built-in defaults for CXX and LD if unchanged
ifeq ($(origin CXX),default)
    undefine CXX
endif
ifeq ($(origin LD),default)
    undefine LD
endif

# Detect Operating System and configure toolchain paths
ifeq ($(OS),Windows_NT)
    # Windows Host (defaulting to MSYS2 UCRT64 toolchain if present, or PATH)
    MSYS2_BIN ?= C:/msys64/ucrt64/bin
    ifneq ($(wildcard $(MSYS2_BIN)/clang++.exe),)
        CXX    ?= $(MSYS2_BIN)/clang++.exe
        NASM   ?= $(MSYS2_BIN)/nasm.exe
        LD     ?= $(MSYS2_BIN)/ld.lld.exe
        PYTHON ?= $(MSYS2_BIN)/python.exe
    else
        CXX    ?= clang++
        NASM   ?= nasm
        LD     ?= ld.lld
        PYTHON ?= python
    endif
    QEMU       ?= qemu-system-x86_64
    OVMF       ?= OVMF.fd
else
    # Linux / POSIX Host
    CXX    ?= clang++
    NASM   ?= nasm
    LD     ?= $(if $(shell command -v ld.lld 2>/dev/null),ld.lld,ld)
    PYTHON ?= $(if $(shell command -v python3 2>/dev/null),python3,python)
    QEMU   ?= qemu-system-x86_64
    OVMF   ?= $(firstword $(wildcard OVMF.fd /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/qemu/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/edk2/ovmf/OVMF_CODE.fd) OVMF.fd)
endif

CXXFLAGS := --target=x86_64-unknown-none-elf \
            -std=c++20 \
            -O2 \
            -ffreestanding \
            -fno-exceptions \
            -fno-rtti \
            -fno-stack-protector \
            -fno-pic \
            -fno-pie \
            -mno-red-zone \
            -mno-80387 \
            -mno-mmx \
            -mno-sse \
            -mno-sse2 \
            -mcmodel=kernel \
            -Wall \
            -Wextra \
            -Werror \
            -MMD -MP

ASFLAGS  := -f elf64
LDFLAGS  := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T linker.ld

BUILD_DIR   := build
KERNEL_ELF  := $(BUILD_DIR)/kernel.elf
DISK_IMAGE  := $(BUILD_DIR)/cortez-kernel.img

ASM_SRCS := \
    src/crt/crt0.asm \
    src/arch/x86_64/gdt_flush.asm \
    src/arch/x86_64/isr_stubs.asm \
    src/proc/context_switch.asm \
    src/klibc/setjmp.asm

CPP_SRCS := \
    src/crt/icxxabi.cpp \
    src/crt/new_delete.cpp \
    src/lib/string_utils.cpp \
    src/lib/printk.cpp \
    src/klibc/klibc.cpp \
    src/boot/boot_info.cpp \
    src/boot/cmdline.cpp \
    src/arch/x86_64/gdt.cpp \
    src/arch/x86_64/idt.cpp \
    src/arch/x86_64/acpi.cpp \
    src/arch/x86_64/apic.cpp \
    src/arch/x86_64/pcie.cpp \
    src/memory/pmm_bitmap.cpp \
    src/memory/vmm_pager.cpp \
    src/memory/heap_allocator.cpp \
    src/memory/module_area.cpp \
    src/drivers/device_registry.cpp \
    src/drivers/video/gop_framebuffer.cpp \
    src/drivers/video/terminal_driver.cpp \
    src/drivers/input/ps2_keyboard.cpp \
    src/drivers/storage/nvme_queue.cpp \
    src/drivers/storage/nvme_controller.cpp \
    src/drivers/storage/gpt_partition.cpp \
    src/drivers/usb/xhci_rings.cpp \
    src/drivers/usb/xhci_controller.cpp \
    src/drivers/usb/usb_msc_device.cpp \
    src/fs/path_resolver.cpp \
    src/fs/fd_table.cpp \
    src/fs/exfat/exfat_fs.cpp \
    src/fs/vfs_manager.cpp \
    src/proc/scheduler.cpp \
    src/module/kapi.cpp \
    src/module/module_manager.cpp \
    src/module/elf_loader.cpp \
    src/shell/line_editor.cpp \
    src/shell/text_editor.cpp \
    src/shell/commands/fs_commands.cpp \
    src/shell/commands/sys_commands.cpp \
    src/shell/commands/module_commands.cpp \
    src/shell/shell.cpp \
    src/kernel/kernel.cpp

TCC_SRCS := \
    third_party/tinycc/libtcc.cpp

ASM_OBJS := $(patsubst src/%.asm,$(BUILD_DIR)/%.asm.o,$(ASM_SRCS))
CPP_OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.cpp.o,$(CPP_SRCS))
TCC_OBJS := $(patsubst third_party/%.cpp,$(BUILD_DIR)/third_party/%.cpp.o,$(TCC_SRCS))
ALL_OBJS := $(ASM_OBJS) $(CPP_OBJS) $(TCC_OBJS)

.PHONY: all image run run-usb clean

all: image

$(BUILD_DIR)/%.asm.o: src/%.asm
	@$(PYTHON) -c "import os; os.makedirs(os.path.dirname('$@'), exist_ok=True)"
	$(NASM) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/%.cpp.o: src/%.cpp
	@$(PYTHON) -c "import os; os.makedirs(os.path.dirname('$@'), exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/third_party/%.cpp.o: third_party/%.cpp
	@$(PYTHON) -c "import os; os.makedirs(os.path.dirname('$@'), exist_ok=True)"
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(KERNEL_ELF): $(ALL_OBJS) linker.ld
	$(LD) $(LDFLAGS) $(ALL_OBJS) -o $(KERNEL_ELF)

image: $(KERNEL_ELF) limine.conf tools/mkimage.py
	$(PYTHON) tools/mkimage.py $(KERNEL_ELF) limine.conf $(DISK_IMAGE)

run: image
	$(QEMU) -machine q35 -m 512M -bios $(OVMF) \
	    -drive file=$(DISK_IMAGE),if=none,id=nvm,format=raw \
	    -device pcie-root-port,id=rp1,slot=0,chassis=1 \
	    -device nvme,bus=rp1,drive=nvm,serial=CORTEZ2026 \
	    -serial stdio

run-usb: image
	$(QEMU) -machine q35 -m 512M -bios $(OVMF) \
	    -device qemu-xhci,id=xhci \
	    -drive file=$(DISK_IMAGE),if=none,id=usbdisk,format=raw \
	    -device usb-storage,bus=xhci.0,drive=usbdisk,serial=CORTEZUSB2026 \
	    -serial stdio

clean:
	@$(PYTHON) -c "import shutil; shutil.rmtree('$(BUILD_DIR)', ignore_errors=True)"

# Auto-generated header dependencies (from -MMD -MP)
-include $(CPP_OBJS:.o=.d) $(TCC_OBJS:.o=.d)
