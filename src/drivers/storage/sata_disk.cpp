/**
 * @file sata_disk.cpp
 * @brief Implementation of the SATA Disk Block Device driver.
 */

#include "sata_disk.hpp"
#include "../../lib/string_utils.hpp"
#include "../../lib/printk.hpp"

namespace cortez::drivers::storage {

namespace {

void copyAtaString(char* dst, const u16* srcWords, usize numWords, usize maxDst) noexcept {
    usize outIdx = 0;
    for (usize i = 0; i < numWords && outIdx + 2 < maxDst; ++i) {
        u16 w = srcWords[i];
        dst[outIdx++] = static_cast<char>((w >> 8) & 0xFF);
        dst[outIdx++] = static_cast<char>(w & 0xFF);
    }
    // Trim trailing spaces and nulls
    while (outIdx > 0 && (dst[outIdx - 1] == ' ' || dst[outIdx - 1] == '\0')) {
        --outIdx;
    }
    dst[outIdx] = '\0';
}

} // namespace

Status SataDisk::identify() noexcept {
    CORTEZ_ALIGNED(16) u16 idData[256]{};

    Status st = m_port->executeCommand(ATA_CMD_IDENTIFY, 0, 0, idData, false);
    if (st != Status::Ok) {
        pr_err("sata", "Port %u: ATA IDENTIFY command failed (%s)", m_port->getPortIndex(), statusToString(st));
        return st;
    }

    // Words 10..19: Serial number (20 ASCII chars)
    copyAtaString(m_serial, &idData[10], 10, sizeof(m_serial));

    // Words 23..26: Firmware revision (8 ASCII chars)
    copyAtaString(m_firmware, &idData[23], 4, sizeof(m_firmware));

    // Words 27..46: Model number (40 ASCII chars)
    copyAtaString(m_model, &idData[27], 20, sizeof(m_model));

    // Word 83: Command set supported (bit 10 = 1: 48-bit LBA supported)
    m_lba48 = (idData[83] & (1U << 10)) != 0;

    if (m_lba48) {
        // Words 100..103: Total user addressable logical sectors (48-bit)
        m_blockCount = static_cast<u64>(idData[100]) |
                      (static_cast<u64>(idData[101]) << 16) |
                      (static_cast<u64>(idData[102]) << 32) |
                      (static_cast<u64>(idData[103]) << 48);
    } else {
        // Words 60..61: Total user addressable logical sectors (28-bit)
        m_blockCount = static_cast<u64>(idData[60]) |
                      (static_cast<u64>(idData[61]) << 16);
    }

    // Word 106: Physical / logical sector size
    if ((idData[106] & 0xC000U) == 0x4000U && (idData[106] & (1U << 12))) {
        // Logical sector is larger than 256 words (512 bytes)
        u32 wordsPerSector = static_cast<u32>(idData[117]) | (static_cast<u32>(idData[118]) << 16);
        m_blockSize = (wordsPerSector > 0) ? (wordsPerSector * 2U) : 512U;
    } else {
        m_blockSize = 512U;
    }

    return Status::Ok;
}

Status SataDisk::init(AhciPort* port, u32 diskIndex) noexcept {
    if (!port) return Status::InvalidArgument;
    m_port      = port;
    m_diskIndex = diskIndex;

    // Generate disk name: sda, sdb, sdc, etc.
    m_name[0] = 's';
    m_name[1] = 'd';
    if (diskIndex < 26) {
        m_name[2] = static_cast<char>('a' + diskIndex);
        m_name[3] = '\0';
    } else {
        m_name[2] = static_cast<char>('a' + (diskIndex / 26) - 1);
        m_name[3] = static_cast<char>('a' + (diskIndex % 26));
        m_name[4] = '\0';
    }

    Status st = identify();
    if (st != Status::Ok) {
        return st;
    }

    u64 sizeMiB = (m_blockCount * m_blockSize) / (1024ULL * 1024ULL);
    pr_ok("sata", "Port %u: Attached /dev/%s '%s' FW='%s' SN='%s' — %llu blocks x %u B (%llu MiB) [%s]",
          m_port->getPortIndex(), m_name, m_model, m_firmware, m_serial,
          m_blockCount, m_blockSize, sizeMiB, m_port->getSpeedString());

    return Status::Ok;
}

Status SataDisk::readBlocks(u64 lba, u32 count, void* buffer) {
    if (!m_port || !buffer) return Status::InvalidArgument;
    if (count == 0) return Status::Ok;
    if (lba + count > m_blockCount) return Status::InvalidArgument;

    // Max 128 sectors per DMA bounce chunk (64 KiB)
    constexpr u32 MAX_SECTORS_PER_CHUNK = 128;
    auto* dst = static_cast<u8*>(buffer);
    u32 done = 0;

    while (done < count) {
        u32 chunk = count - done;
        if (chunk > MAX_SECTORS_PER_CHUNK) chunk = MAX_SECTORS_PER_CHUNK;

        u8 cmd = m_lba48 ? ATA_CMD_READ_DMA_EXT : ATA_CMD_READ_DMA;
        Status st = m_port->executeCommand(cmd, lba + done, static_cast<u16>(chunk),
                                           dst + static_cast<usize>(done) * m_blockSize, false);
        if (st != Status::Ok) return st;
        done += chunk;
    }

    return Status::Ok;
}

Status SataDisk::writeBlocks(u64 lba, u32 count, const void* buffer) {
    if (!m_port || !buffer) return Status::InvalidArgument;
    if (count == 0) return Status::Ok;
    if (lba + count > m_blockCount) return Status::InvalidArgument;

    // Max 128 sectors per DMA bounce chunk (64 KiB)
    constexpr u32 MAX_SECTORS_PER_CHUNK = 128;
    const auto* src = static_cast<const u8*>(buffer);
    u32 done = 0;

    while (done < count) {
        u32 chunk = count - done;
        if (chunk > MAX_SECTORS_PER_CHUNK) chunk = MAX_SECTORS_PER_CHUNK;

        u8 cmd = m_lba48 ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_WRITE_DMA;
        Status st = m_port->executeCommand(cmd, lba + done, static_cast<u16>(chunk),
                                           const_cast<u8*>(src + static_cast<usize>(done) * m_blockSize), true);
        if (st != Status::Ok) return st;
        done += chunk;
    }

    return Status::Ok;
}

Status SataDisk::flush() {
    if (!m_port) return Status::DeviceNotFound;
    return m_port->flush();
}

} // namespace cortez::drivers::storage
