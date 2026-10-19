/**
 * @file storage_manager.hpp
 * @brief Unified dynamic storage orchestrator detecting NVMe, SATA, and USB mass storage.
 */

#pragma once

#include "../i_device.hpp"
#include "../../memory/i_memory.hpp"

namespace cortez::drivers::storage {

class StorageManager final {
public:
    static StorageManager& instance() noexcept;

    Status probeAll(memory::IPhysicalAllocator* pmm,
                    memory::IVirtualMapper* vmm,
                    u64 hhdmOffset) noexcept;

    IBlockDevice* resolveRootDevice(const char* preferredDevName) noexcept;

    bool  hasNvme() const noexcept { return m_hasNvme; }
    bool  hasSata() const noexcept { return m_hasSata; }
    bool  hasUsb() const noexcept  { return m_hasUsb; }

    usize getNvmeControllerCount() const noexcept { return m_nvmeControllers; }
    usize getSataControllerCount() const noexcept { return m_sataControllers; }
    usize getUsbControllerCount() const noexcept  { return m_usbControllers; }

private:
    StorageManager() noexcept = default;

    usize m_nvmeControllers = 0;
    usize m_sataControllers = 0;
    usize m_usbControllers  = 0;

    bool  m_hasNvme         = false;
    bool  m_hasSata         = false;
    bool  m_hasUsb          = false;
};

} // namespace cortez::drivers::storage
