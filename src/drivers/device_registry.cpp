/**
 * @file device_registry.cpp
 * @brief Implementation of DeviceRegistry for block and character devices.
 */

#include "i_device.hpp"
#include "../lib/string_utils.hpp"

namespace cortez::drivers {

DeviceRegistry& DeviceRegistry::instance() noexcept {
    static DeviceRegistry s_instance;
    return s_instance;
}

Status DeviceRegistry::registerBlockDevice(IBlockDevice* dev) noexcept {
    if (!dev || !dev->getName()) return Status::InvalidArgument;
    if (m_blockCount >= MAX_BLOCK_DEVICES) return Status::NoSpace;
    for (usize i = 0; i < m_blockCount; ++i) {
        if (str::compare(m_blockDevices[i]->getName(), dev->getName()) == 0) {
            return Status::AlreadyExists;
        }
    }
    m_blockDevices[m_blockCount++] = dev;
    return Status::Ok;
}

Status DeviceRegistry::registerCharDevice(ICharDevice* dev) noexcept {
    if (!dev || !dev->getName()) return Status::InvalidArgument;
    if (m_charCount >= MAX_CHAR_DEVICES) return Status::NoSpace;
    for (usize i = 0; i < m_charCount; ++i) {
        if (str::compare(m_charDevices[i]->getName(), dev->getName()) == 0) {
            return Status::AlreadyExists;
        }
    }
    m_charDevices[m_charCount++] = dev;
    return Status::Ok;
}

IBlockDevice* DeviceRegistry::findBlockDevice(const char* name) const noexcept {
    if (!name) return nullptr;
    if (str::compareN(name, "/dev/", 5) == 0) {
        name += 5;
    }
    for (usize i = 0; i < m_blockCount; ++i) {
        if (str::compare(m_blockDevices[i]->getName(), name) == 0) {
            return m_blockDevices[i];
        }
    }
    return nullptr;
}

ICharDevice* DeviceRegistry::findCharDevice(const char* name) const noexcept {
    if (!name) return nullptr;
    if (str::compareN(name, "/dev/", 5) == 0) {
        name += 5;
    }
    for (usize i = 0; i < m_charCount; ++i) {
        if (str::compare(m_charDevices[i]->getName(), name) == 0) {
            return m_charDevices[i];
        }
    }
    return nullptr;
}

} // namespace cortez::drivers
