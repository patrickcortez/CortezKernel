/**
 * @file module_adapters.hpp
 * @brief C-to-C++ SOLID adapter classes bridging C function pointers to IShellCommand, ICharDevice, and IInterruptHandler.
 */

#pragma once

#include "kapi.hpp"
#include "../shell/i_shell_command.hpp"
#include "../drivers/i_device.hpp"
#include "../arch/x86_64/idt.hpp"
#include "../arch/x86_64/cpu.hpp"
#include "../lib/string_utils.hpp"

namespace cortez::module {

/**
 * @brief Wraps a C module shell command callback into an IShellCommand object.
 */
class ModuleShellCommand final : public shell::IShellCommand {
public:
    ModuleShellCommand(u32 ownerModuleId,
                       const char* name,
                       const char* desc,
                       const char* usage,
                       cortez_cmd_fn fn) noexcept
        : m_ownerModuleId(ownerModuleId), m_fn(fn) {
        str::copy(m_name, name ? name : "", sizeof(m_name));
        str::copy(m_desc, desc ? desc : "", sizeof(m_desc));
        str::copy(m_usage, usage ? usage : "", sizeof(m_usage));
    }

    ~ModuleShellCommand() override = default;

    const char* getName() const noexcept override { return m_name; }
    const char* getDescription() const noexcept override { return m_desc; }
    const char* getUsage() const noexcept override { return m_usage; }

    Status execute(const shell::CommandArgs& args) override;

    u32           getOwnerModuleId() const noexcept { return m_ownerModuleId; }
    cortez_cmd_fn getCallback() const noexcept { return m_fn; }

private:
    u32           m_ownerModuleId = 0;
    char          m_name[32]{};
    char          m_desc[96]{};
    char          m_usage[64]{};
    cortez_cmd_fn m_fn            = nullptr;
};

/**
 * @brief Wraps a C module character device ops struct into an ICharDevice object.
 */
class ModuleCharDevice final : public drivers::ICharDevice {
public:
    ModuleCharDevice(u32 ownerModuleId,
                     const char* name,
                     const cortez_chardev_ops* ops,
                     void* ctx) noexcept
        : m_ownerModuleId(ownerModuleId), m_ctx(ctx) {
        str::copy(m_name, name ? name : "", sizeof(m_name));
        if (ops) {
            m_ops = *ops;
        }
    }

    ~ModuleCharDevice() override = default;

    const char* getName() const noexcept override { return m_name; }

    Result<usize> read(void* buffer, usize count) override {
        if (!m_ops.read) {
            return Status::NotSupported;
        }
        i64 ret = m_ops.read(m_ctx, buffer, count);
        if (ret < 0) {
            return Status::IoError;
        }
        return static_cast<usize>(ret);
    }

    Result<usize> write(const void* buffer, usize count) override {
        if (!m_ops.write) {
            return Status::NotSupported;
        }
        i64 ret = m_ops.write(m_ctx, buffer, count);
        if (ret < 0) {
            return Status::IoError;
        }
        return static_cast<usize>(ret);
    }

    bool hasData() const noexcept override {
        if (!m_ops.has_data) {
            return true;
        }
        return m_ops.has_data(m_ctx);
    }

    u32 getOwnerModuleId() const noexcept { return m_ownerModuleId; }

private:
    u32                m_ownerModuleId = 0;
    char               m_name[32]{};
    cortez_chardev_ops m_ops{};
    void*              m_ctx           = nullptr;
};

/**
 * @brief Wraps a C module IRQ callback into an IInterruptHandler object, saving/restoring FPU/SSE state.
 */
class ModuleIrqHandler final : public arch::IInterruptHandler {
public:
    ModuleIrqHandler(u32 ownerModuleId, u8 vector, cortez_irq_fn fn, void* ctx) noexcept
        : m_ownerModuleId(ownerModuleId), m_vector(vector), m_fn(fn), m_ctx(ctx) {}

    ~ModuleIrqHandler() override = default;

    void onInterrupt(arch::InterruptFrame* frame) noexcept override {
        (void)frame;
        if (m_fn) {
            CORTEZ_ALIGNED(16) u8 fpuSave[512];
            arch::fxsave(fpuSave);
            m_fn(m_vector, m_ctx);
            arch::fxrstor(fpuSave);
        }
    }

    u32  getOwnerModuleId() const noexcept { return m_ownerModuleId; }
    u8   getVector() const noexcept { return m_vector; }
    void setIsaIrq(i32 isaIrq) noexcept { m_isaIrq = isaIrq; }
    i32  getIsaIrq() const noexcept { return m_isaIrq; }

private:
    u32           m_ownerModuleId = 0;
    u8            m_vector        = 0;
    i32           m_isaIrq        = -1;
    cortez_irq_fn m_fn            = nullptr;
    void*         m_ctx           = nullptr;
};

} // namespace cortez::module
