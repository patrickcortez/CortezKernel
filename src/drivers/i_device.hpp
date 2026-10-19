/**
 * @file i_device.hpp
 * @brief Core SOLID interfaces for Hardware Devices, Block Devices, and Character Devices.
 */

#pragma once

#include "../lib/types.hpp"
#include "../lib/result.hpp"

namespace cortez::drivers {

enum class DeviceType : u8 {
    Character = 1,
    Block     = 2,
    Controller = 3
};

/**
 * @brief Base interface for all hardware devices in the Cortez Kernel.
 */
class IDevice {
public:
    virtual ~IDevice() = default;
    virtual const char* getName() const noexcept = 0;
    virtual DeviceType  getDeviceType() const noexcept = 0;
};

/**
 * @brief Interface for character-stream I/O devices (e.g., Terminal, Keyboard).
 */
class ICharDevice : public IDevice {
public:
    ~ICharDevice() override = default;

    DeviceType getDeviceType() const noexcept override {
        return DeviceType::Character;
    }

    virtual Result<usize> read(void* buffer, usize count) = 0;
    virtual Result<usize> write(const void* buffer, usize count) = 0;
    virtual bool          hasData() const noexcept = 0;
};

enum class StorageBusType : u8 {
    Unknown = 0,
    NVMe    = 1,
    SATA    = 2,
    USB     = 3,
    RAM     = 4,
};

/**
 * @brief Interface for sector/block-addressable storage devices (NVMe namespaces, GPT partitions).
 */
class IBlockDevice : public IDevice {
public:
    ~IBlockDevice() override = default;

    DeviceType getDeviceType() const noexcept override {
        return DeviceType::Block;
    }

    virtual u32            getBlockSize() const noexcept = 0;
    virtual u64            getBlockCount() const noexcept = 0;
    virtual Status         readBlocks(u64 lba, u32 count, void* buffer) = 0;
    virtual Status         writeBlocks(u64 lba, u32 count, const void* buffer) = 0;
    virtual Status         flush() = 0;

    /// GPT partition name (empty for whole disks / unlabelled devices).
    virtual const char*    getLabel() const noexcept { return ""; }

    /// Underlying storage hardware bus or interface
    virtual StorageBusType getBusType() const noexcept { return StorageBusType::Unknown; }
    virtual const char*    getBusTypeName() const noexcept {
        switch (getBusType()) {
            case StorageBusType::NVMe: return "NVMe";
            case StorageBusType::SATA: return "SATA";
            case StorageBusType::USB:  return "USB";
            case StorageBusType::RAM:  return "RAM";
            default:                   return "Unknown";
        }
    }
};

/**
 * @brief Registry of active character and block devices (Linux bdev/cdev table equivalent).
 */
class DeviceRegistry final {
public:
    static constexpr usize MAX_BLOCK_DEVICES = 32;
    static constexpr usize MAX_CHAR_DEVICES  = 32;

    static DeviceRegistry& instance() noexcept;

    Status        registerBlockDevice(IBlockDevice* dev) noexcept;
    Status        unregisterBlockDevice(IBlockDevice* dev) noexcept;
    Status        registerCharDevice(ICharDevice* dev) noexcept;
    Status        unregisterCharDevice(ICharDevice* dev) noexcept;

    IBlockDevice* findBlockDevice(const char* name) const noexcept;
    ICharDevice*  findCharDevice(const char* name) const noexcept;

    usize         getBlockDeviceCount() const noexcept { return m_blockCount; }
    IBlockDevice* getBlockDeviceAt(usize idx) const noexcept {
        return (idx < m_blockCount) ? m_blockDevices[idx] : nullptr;
    }

    usize         getCharDeviceCount() const noexcept { return m_charCount; }
    ICharDevice*  getCharDeviceAt(usize idx) const noexcept {
        return (idx < m_charCount) ? m_charDevices[idx] : nullptr;
    }

private:
    DeviceRegistry() noexcept = default;

    IBlockDevice* m_blockDevices[MAX_BLOCK_DEVICES]{};
    usize         m_blockCount = 0;

    ICharDevice*  m_charDevices[MAX_CHAR_DEVICES]{};
    usize         m_charCount  = 0;
};

} // namespace cortez::drivers
