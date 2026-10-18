/**
 * @file cmdline.cpp
 * @brief Implementation of KernelCmdline argument parser.
 */

#include "cmdline.hpp"
#include "../lib/string_utils.hpp"

namespace cortez::boot {

KernelCmdline& KernelCmdline::instance() noexcept {
    static KernelCmdline s_cmdline;
    return s_cmdline;
}

void KernelCmdline::parse(const char* rawCmdline) noexcept {
    if (!rawCmdline || !rawCmdline[0]) {
        return;
    }

    str::copy(m_raw, rawCmdline, sizeof(m_raw));

    const char* p = m_raw;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;

        char token[96];
        usize len = 0;
        while (*p && *p != ' ' && *p != '\t' && len + 1 < sizeof(token)) {
            token[len++] = *p++;
        }
        token[len] = '\0';

        if (str::compareN(token, "root=", 5) == 0) {
            str::copy(m_rootDevice, token + 5, sizeof(m_rootDevice));
        } else if (str::compareN(token, "rootfstype=", 11) == 0) {
            str::copy(m_rootFsType, token + 11, sizeof(m_rootFsType));
        } else if (str::compareN(token, "console=", 8) == 0) {
            str::copy(m_console, token + 8, sizeof(m_console));
        } else if (str::compare(token, "ro") == 0) {
            m_readWrite = false;
        } else if (str::compare(token, "rw") == 0) {
            m_readWrite = true;
        }
    }
}

} // namespace cortez::boot
