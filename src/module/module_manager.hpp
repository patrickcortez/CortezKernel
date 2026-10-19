/**
 * @file module_manager.hpp
 * @brief Runtime C Kernel Module & Program Loader, Resource Tracker, and Fault Containment Manager.
 */

#pragma once

#include "kapi.hpp"
#include "module_adapters.hpp"
#include "../lib/types.hpp"
#include "../lib/result.hpp"
#include "../arch/x86_64/idt.hpp"
#include "../klibc/klibc.hpp"

namespace cortez::module {

struct ModuleSymbol {
    char        name[48];
    virt_addr_t addr;
};

struct ModulePageAlloc {
    phys_addr_t physAddr;
    usize       pageCount;
};

struct LoadedModule {
    bool               inUse;
    u32                id;
    char               name[32];
    char               description[80];
    char               sourcePath[128];
    void*              codeBase;
    usize              codeSize;
    int              (*initFn)(void);
    void             (*exitFn)(void);

    static constexpr usize MAX_MOD_SYMBOLS  = 32;
    static constexpr usize MAX_MOD_COMMANDS = 8;
    static constexpr usize MAX_MOD_CHARDEVS = 4;
    static constexpr usize MAX_MOD_IRQS     = 4;
    static constexpr usize MAX_MOD_PAGES    = 16;

    ModuleSymbol        symbols[MAX_MOD_SYMBOLS];
    usize               symbolCount;

    ModuleShellCommand* commands[MAX_MOD_COMMANDS];
    usize               commandCount;

    ModuleCharDevice*   charDevs[MAX_MOD_CHARDEVS];
    usize               charDevCount;

    ModuleIrqHandler*   irqHandlers[MAX_MOD_IRQS];
    usize               irqCount;

    ModulePageAlloc     pageAllocs[MAX_MOD_PAGES];
    usize               pageAllocCount;
};

class ModuleManager final {
public:
    static constexpr usize MAX_LOADED_MODULES = 32;

    static ModuleManager& instance() noexcept;

    Status load(const char* path) noexcept;
    Status unload(const char* name) noexcept;
    Status runProgram(const char* path, usize argc, const char** argv, i32* outExitCode = nullptr) noexcept;
    Status checkCompile(const char* path) noexcept;
    usize  autoload(const char* dirPath) noexcept;

    // Resource registration hooks called by kapi.cpp
    int    registerCommandForCurrent(const char* name, const char* desc, const char* usage, cortez_cmd_fn fn) noexcept;
    int    unregisterCommandByName(const char* name) noexcept;
    int    registerCharDevForCurrent(const char* name, const cortez_chardev_ops* ops, void* ctx) noexcept;
    int    unregisterCharDevByName(const char* name) noexcept;
    int    registerIrqForCurrent(u8 vector, cortez_irq_fn handler, void* ctx) noexcept;
    void   recordIsaIrqRoute(u8 isaIrq, u8 vector) noexcept;
    void   unregisterIrq(u8 vector) noexcept;
    u64    allocPagesForCurrent(usize count) noexcept;
    void   freePagesForCurrent(u64 physAddr, usize count) noexcept;

    u32    getActiveModuleId() const noexcept { return m_activeModuleId; }
    void   setActiveModuleId(u32 id) noexcept { m_activeModuleId = id; }

    // Fault containment & symbol resolution (Phase 6)
    bool   resolveAddress(virt_addr_t rip,
                          const char** outModName,
                          const char** outSymName,
                          u64* outOffset) const noexcept;
    bool   handleException(arch::InterruptFrame* frame, const char* exceptionName) noexcept;

    // Execute a callback under a fault-recovery guard
    bool   enterRecoveryGuard(cortez_jmp_buf** outBuf) noexcept;
    void   leaveRecoveryGuard() noexcept;

    usize               getModuleCount() const noexcept { return MAX_LOADED_MODULES; }
    const LoadedModule* getModuleAt(usize idx) const noexcept {
        if (idx >= MAX_LOADED_MODULES) return nullptr;
        return m_modules[idx].inUse ? &m_modules[idx] : nullptr;
    }

private:
    ModuleManager() noexcept = default;

    LoadedModule* findById(u32 id) noexcept;
    LoadedModule* findByName(const char* name) noexcept;
    void          cleanupModuleResources(LoadedModule& mod) noexcept;
    static void   extractBaseName(const char* path, char* outName, usize maxLen) noexcept;

    LoadedModule   m_modules[MAX_LOADED_MODULES]{};
    u32            m_nextModuleId   = 1;
    u32            m_activeModuleId = 0;

    bool           m_recoveryActive = false;
    cortez_jmp_buf m_recoveryEnv{};
};

} // namespace cortez::module
