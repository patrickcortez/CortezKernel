/**
 * @file os_installer.hpp
 * @brief Autonomous OS Installer: discovers unallocated disk space on internal drives,
 *        partitions GPT, formats RootFS and ESP, installs kernel/system files, and
 *        writes the Limine UEFI bootloader configuration.
 */

#pragma once

#include "../i_device.hpp"
#include "../../lib/types.hpp"
#include "../../lib/result.hpp"

namespace cortez::drivers::storage {

struct FreeSpaceSpan {
    u64 startLba     = 0;
    u64 endLba       = 0;
    u64 sectorCount  = 0;
    u64 sizeMiB      = 0;
};

struct DiskInstallInfo {
    IBlockDevice* disk               = nullptr;
    char          name[32]{};
    u64           totalSectors       = 0;
    u32           blockSize          = 512;
    u64           totalSizeMiB       = 0;
    bool          isGpt              = false;
    bool          hasEsp             = false;
    char          espPartName[32]{};
    u32           existingPartCount  = 0;
    FreeSpaceSpan largestFree{};
    bool          hasEnoughSpace     = false; // At least 128 MiB unallocated
};

class OsInstaller final {
public:
    static constexpr usize MAX_TARGET_DISKS = 8;
    static constexpr u64   MIN_INSTALL_SECTORS = 262144; // 128 MiB in 512B sectors

    static OsInstaller& instance() noexcept;

    /**
     * @brief Discovers all internal physical disks (excluding the live boot disk)
     *        and analyzes each disk's partition table and unallocated space.
     */
    Status scanCandidateDisks(DiskInstallInfo* outDisks,
                              usize maxDisks,
                              usize* outCount) noexcept;

    /**
     * @brief Analyzes a specific physical disk for existing partitions and unallocated gaps.
     */
    Status findUnallocatedSpace(IBlockDevice* disk, DiskInstallInfo* outInfo) noexcept;

    /**
     * @brief Performs full end-to-end installation onto the specified disk's unallocated space:
     *        1. Partitions the unallocated space (ESP + Cortez-RootFS) in GPT
     *        2. Formats RootFS as exFAT and ESP as FAT32
     *        3. Copies kernel, system files, and modules from live boot media
     *        4. Configures Limine UEFI bootloader and boot():/boot/kernel.elf
     */
    Status installToDisk(IBlockDevice* targetDisk) noexcept;

    /**
     * @brief High-level shell helper to install by disk name (e.g. "nvme0n1", "/dev/sda").
     */
    Status installByName(const char* diskName) noexcept;

private:
    OsInstaller() noexcept = default;

    static u32 calculateCrc32(const void* data, usize length) noexcept;
    static void generateGuid(u8 outGuid[16]) noexcept;
    static void asciiToUtf16(u16 outName[36], const char* ascii) noexcept;
};

} // namespace cortez::drivers::storage
