/**
 * @file fs_detector.hpp
 * @brief Filesystem auto-detection and metadata probing (FAT12/16/32, exFAT, ext2).
 */

#pragma once

#include "../drivers/i_device.hpp"

namespace cortez::fs {

enum class DetectedFsType : u8 {
    Unknown = 0,
    Fat12,
    Fat16,
    Fat32,
    ExFat,
    Ext2
};

struct FsProbeInfo {
    DetectedFsType type              = DetectedFsType::Unknown;
    u32            bytesPerSector    = 512;
    u32            sectorsPerCluster = 1;
    u64            totalSectors      = 0;
    char           volumeLabel[32]{};
    char           fsName[16]{};
};

class FsDetector final {
public:
    static FsProbeInfo probe(drivers::IBlockDevice* dev) noexcept;
    static const char* typeToString(DetectedFsType type) noexcept;
};

} // namespace cortez::fs
