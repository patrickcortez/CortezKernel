/**
 * @file fat_structs.hpp
 * @brief On-disk structures for File Allocation Table (FAT12/FAT16/FAT32) filesystems.
 */

#pragma once

#include "../../lib/types.hpp"

namespace cortez::fs::fat {

static constexpr u32 FAT32_CLUSTER_FIRST   = 2U;
static constexpr u32 FAT32_CLUSTER_FREE    = 0x00000000U;
static constexpr u32 FAT32_CLUSTER_BAD     = 0x0FFFFFF7U;
static constexpr u32 FAT32_CLUSTER_EOC_MIN = 0x0FFFFFF8U;
static constexpr u32 FAT32_CLUSTER_EOC     = 0x0FFFFFFFU;

static constexpr u8 FAT_ATTR_READ_ONLY = 0x01U;
static constexpr u8 FAT_ATTR_HIDDEN    = 0x02U;
static constexpr u8 FAT_ATTR_SYSTEM    = 0x04U;
static constexpr u8 FAT_ATTR_VOLUME_ID = 0x08U;
static constexpr u8 FAT_ATTR_DIRECTORY = 0x10U;
static constexpr u8 FAT_ATTR_ARCHIVE   = 0x20U;
static constexpr u8 FAT_ATTR_LFN       = 0x0FU;

static constexpr u8 FAT_ENTRY_FREE_OR_DELETED = 0xE5U;
static constexpr u8 FAT_ENTRY_LAST_FREE       = 0x00U;

struct CORTEZ_PACKED FatBootSector {
    u8  jumpBoot[3];
    char oemName[8];
    u16 bytesPerSector;
    u8  sectorsPerCluster;
    u16 reservedSectors;
    u8  numFats;
    u16 rootEntryCount;
    u16 totalSectors16;
    u8  mediaType;
    u16 fatSize16;
    u16 sectorsPerTrack;
    u16 numHeads;
    u32 hiddenSectors;
    u32 totalSectors32;

    // Extended FAT32 BPB
    u32 fatSize32;
    u16 extFlags;
    u16 fsVersion;
    u32 rootDirFirstCluster;
    u16 fsInfoSector;
    u16 backupBootSector;
    u8  reserved[12];
    u8  driveNumber;
    u8  reserved1;
    u8  bootSig;
    u32 volumeId;
    char volumeLabel[11];
    char fsType[8];
};

struct CORTEZ_PACKED FatDirEntryRaw {
    char name[11]; // 8.3 Short File Name (e.g., "FOO     TXT")
    u8   attr;
    u8   ntRes;
    u8   createTimeTenth;
    u16  createTime;
    u16  createDate;
    u16  lastAccessDate;
    u16  firstClusterHigh;
    u16  writeTime;
    u16  writeDate;
    u16  firstClusterLow;
    u32  fileSize;
};

struct CORTEZ_PACKED FatLfnEntryRaw {
    u8  order;
    u16 name1[5];
    u8  attr; // 0x0F
    u8  type; // 0x00
    u8  checksum;
    u16 name2[6];
    u16 firstClusterZero;
    u16 name3[2];
};

} // namespace cortez::fs::fat
