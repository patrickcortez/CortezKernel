/**
 * @file cmdline.hpp
 * @brief Kernel command-line parser (e.g., root=/dev/nvme0n1p2 rootfstype=exfat rw).
 */

#pragma once

#include "../lib/types.hpp"

namespace cortez::boot {

class KernelCmdline final {
public:
    static KernelCmdline& instance() noexcept;

    void parse(const char* rawCmdline) noexcept;

    const char* getRootDevice() const noexcept { return m_rootDevice; }
    const char* getRootFsType() const noexcept { return m_rootFsType; }
    const char* getConsole() const noexcept { return m_console; }
    const char* getRaw() const noexcept { return m_raw; }
    bool        isReadWrite() const noexcept { return m_readWrite; }
    bool        isNoExt() const noexcept { return m_noExt; }

private:
    KernelCmdline() noexcept = default;

    char m_raw[256]        = "root=/dev/nvme0n1p2 rootfstype=exfat rw console=tty0";
    char m_rootDevice[64]  = "/dev/nvme0n1p2";
    char m_rootFsType[32]  = "auto";
    char m_console[32]     = "tty0";
    bool m_readWrite       = true;
    bool m_noExt           = false;
};

} // namespace cortez::boot
