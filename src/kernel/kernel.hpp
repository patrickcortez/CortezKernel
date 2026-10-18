/**
 * @file kernel.hpp
 * @brief Top-level Cortez Kernel orchestrator (start_kernel & kernel_init).
 */

#pragma once

#include "../lib/types.hpp"

extern "C" void start_kernel();

namespace cortez::kernel {

class Kernel final {
public:
    static Kernel& instance() noexcept;

    void start() noexcept;

    static void kernelInitTask();
    static void kworkerTask();

private:
    Kernel() noexcept = default;

    void setupArch() noexcept;
    void mmInit() noexcept;
    void initIrqAndTimers() noexcept;
    void doBasicSetup() noexcept;
    void mountBlockRoot() noexcept;
};

} // namespace cortez::kernel
