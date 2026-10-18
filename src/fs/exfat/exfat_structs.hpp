/**
 * @file exfat_structs.hpp
 * @brief Microsoft exFAT on-disk structures (Boot Sector, FAT, Bitmap, and 32B Directory Entries).
 */

#pragma once

#include "../../lib/types.hpp"

namespace cortez::fs::exfat {

// exFAT FAT entry constants
constexpr u32 EXFAT_CLUSTER_FREE  = 0x00000000U;
constexpr u32 EXFAT_CLUSTER_FIRST = 2U;
constexpr u32 EXFAT_CLUSTER_BAD   = 0xFFFFFFF7U;
constexpr u32 EXFAT_CLUSTER_EOC   = 0xFFFFFFFFU;

// exFAT Directory Entry Types
constexpr u8 EXFAT_ENTRY_EOD        = 0x00U; // End of Directory
constexpr u8 EXFAT_ENTRY_BITMAP     = 0x81U; // Allocation Bitmap
constexpr u8 EXFAT_ENTRY_UPCASE     = 0x82U; // Up-case Table
constexpr u8 EXFAT_ENTRY_LABEL      = 0x83U; // Volume Label
constexpr u8 EXFAT_ENTRY_FILE       = 0x85U; // File / Directory Primary Entry
constexpr u8 EXFAT_ENTRY_STREAM     = 0xC0U; // Stream Extension Secondary Entry
constexpr u8 EXFAT_ENTRY_FILENAME   = 0xC1U; // File Name Secondary Entry

// File Attributes (ExFatFileEntry::fileAttributes)
constexpr u16 EXFAT_ATTR_READ_ONLY  = 0x0001U;
constexpr u16 EXFAT_ATTR_HIDDEN     = 0x0002U;
constexpr u16 EXFAT_ATTR_SYSTEM     = 0x0004U;
constexpr u16 EXFAT_ATTR_DIRECTORY  = 0x0010U;
constexpr u16 EXFAT_ATTR_ARCHIVE    = 0x0020U;

// GeneralSecondaryFlags (ExFatStreamEntry::flags)
constexpr u8 EXFAT_FLAG_ALLOC_POSSIBLE = 0x01U;
constexpr u8 EXFAT_FLAG_NO_FAT_CHAIN   = 0x02U;

/**
 * @brief 512-byte exFAT Main Boot Sector (LBA 0 of Partition).
 */
struct CORTEZ_PACKED ExFatBootSector {
    u8  jumpBoot[3];
    char fsName[8]; // "EXFAT   "
    u8  mustBeZero[53];
    u64 partitionOffset;
    u64 volumeLength;
    u32 fatOffset;
    u32 fatLength;
    u32 clusterHeapOffset;
    u32 clusterCount;
    u32 rootDirFirstCluster;
    u32 volumeSerialNumber;
    u16 fsRevision;
    u16 volumeFlags;
    u8  bytesPerSectorShift;
    u8  sectorsPerClusterShift;
    u8  numberOfFats;
    u8  driveSelect;
    u8  percentInUse;
    u8  reserved[7];
    u8  bootCode[390];
    u16 bootSignature; // 0xAA55
};
static_assert(sizeof(ExFatBootSector) == 512, "ExFatBootSector must be 512 bytes");

/**
 * @brief Generic 32-byte exFAT Directory Entry.
 */
struct CORTEZ_PACKED ExFatDirEntryRaw {
    u8 entryType;
    u8 payload[31];
};
static_assert(sizeof(ExFatDirEntryRaw) == 32, "ExFatDirEntryRaw must be 32 bytes");

/**
 * @brief 0x81 Allocation Bitmap Directory Entry.
 */
struct CORTEZ_PACKED ExFatBitmapEntry {
    u8  entryType; // 0x81
    u8  bitmapFlags;
    u8  reserved[18];
    u32 firstCluster;
    u64 dataLength;
};
static_assert(sizeof(ExFatBitmapEntry) == 32, "ExFatBitmapEntry must be 32 bytes");

/**
 * @brief 0x83 Volume Label Directory Entry.
 */
struct CORTEZ_PACKED ExFatLabelEntry {
    u8  entryType; // 0x83
    u8  charCount;
    u16 volumeLabel[11];
    u8  reserved[8];
};
static_assert(sizeof(ExFatLabelEntry) == 32, "ExFatLabelEntry must be 32 bytes");

/**
 * @brief 0x85 File / Directory Entry.
 */
struct CORTEZ_PACKED ExFatFileEntry {
    u8  entryType;      // 0x85
    u8  secondaryCount; // Number of secondary entries (Stream + FileName entries)
    u16 setChecksum;
    u16 fileAttributes;
    u16 reserved1;
    u32 createTimestamp;
    u32 lastModifiedTimestamp;
    u32 lastAccessedTimestamp;
    u8  create10msIncrement;
    u8  lastModified10msIncrement;
    u8  createUtcOffset;
    u8  lastModifiedUtcOffset;
    u8  lastAccessedUtcOffset;
    u8  reserved2[7];
};
static_assert(sizeof(ExFatFileEntry) == 32, "ExFatFileEntry must be 32 bytes");

/**
 * @brief 0xC0 Stream Extension Directory Entry.
 */
struct CORTEZ_PACKED ExFatStreamEntry {
    u8  entryType; // 0xC0
    u8  flags;     // Bit 0: AllocationPossible, Bit 1: NoFatChain
    u8  reserved1;
    u8  nameLength;
    u16 nameHash;
    u16 reserved2;
    u64 validDataLength;
    u32 reserved3;
    u32 firstCluster;
    u64 dataLength;
};
static_assert(sizeof(ExFatStreamEntry) == 32, "ExFatStreamEntry must be 32 bytes");

/**
 * @brief 0xC1 File Name Directory Entry (holds up to 15 UTF-16LE characters).
 */
struct CORTEZ_PACKED ExFatNameEntry {
    u8  entryType; // 0xC1
    u8  flags;
    u16 fileName[15];
};
static_assert(sizeof(ExFatNameEntry) == 32, "ExFatNameEntry must be 32 bytes");

/**
 * @brief Computes the 16-bit exFAT NameHash over an upper-cased ASCII/UTF-16 filename.
 */
inline u16 computeNameHash(const u16* upperName, usize charCount) noexcept {
    u16 hash = 0;
    const auto* bytes = reinterpret_cast<const u8*>(upperName);
    usize byteLen = charCount * 2;
    for (usize i = 0; i < byteLen; ++i) {
        hash = static_cast<u16>(((hash << 15) | (hash >> 1)) + bytes[i]);
    }
    return hash;
}

/**
 * @brief Computes the 16-bit exFAT EntrySetChecksum over a File + Stream + Name entry set.
 */
inline u16 computeEntrySetChecksum(const void* entries, usize entryCount) noexcept {
    u16 sum = 0;
    const auto* bytes = static_cast<const u8*>(entries);
    usize totalBytes  = entryCount * 32;
    for (usize i = 0; i < totalBytes; ++i) {
        if (i == 2 || i == 3) continue; // Skip SetChecksum field itself
        sum = static_cast<u16>(((sum << 15) | (sum >> 1)) + bytes[i]);
    }
    return sum;
}

} // namespace cortez::fs::exfat
