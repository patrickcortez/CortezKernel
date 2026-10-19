/**
 * @file module_manager.cpp
 * @brief Implementation of the Runtime C Kernel Module & Program Loader, Resource Tracker, and Fault Containment.
 */

#include "module_manager.hpp"
#include "../memory/module_area.hpp"
#include "../memory/pmm_bitmap.hpp"
#include "../memory/heap_allocator.hpp"
#include "../shell/shell.hpp"
#include "../drivers/i_device.hpp"
#include "../arch/x86_64/idt.hpp"
#include "../arch/x86_64/apic.hpp"
#include "../arch/x86_64/cpu.hpp"
#include "../proc/scheduler.hpp"
#include "../fs/vfs_manager.hpp"
#include "../lib/printk.hpp"
#include "../lib/string_utils.hpp"
#include "../../third_party/tinycc/libtcc.h"

namespace cortez::module {

namespace {

void tccErrorCallback(void* opaque, const char* msg) {
    const char* srcPath = static_cast<const char*>(opaque);
    pr_err("tcc", "[%s] %s", srcPath ? srcPath : "cc", msg ? msg : "compilation error");
}

} // namespace

Status ModuleShellCommand::execute(const shell::CommandArgs& args) {
    if (!m_fn) return Status::InvalidArgument;

    auto& mgr = ModuleManager::instance();
    u32 prevMod = mgr.getActiveModuleId();
    mgr.setActiveModuleId(m_ownerModuleId);

    cortez_jmp_buf* jmpEnv = nullptr;
    bool guarded = mgr.enterRecoveryGuard(&jmpEnv);
    int ret = 0;

    if (guarded && jmpEnv) {
        if (cortez_setjmp(*jmpEnv) == 0) {
            ret = m_fn(static_cast<int>(args.argc), const_cast<const char**>(args.argv));
        } else {
            kprintf("\033[1;31m[FAULT] Command '%s' aborted after CPU exception; recovered to shell.\033[0m\n",
                    m_name);
            ret = -1;
        }
        mgr.leaveRecoveryGuard();
    } else {
        ret = m_fn(static_cast<int>(args.argc), const_cast<const char**>(args.argv));
    }

    mgr.setActiveModuleId(prevMod);
    (void)ret;
    return Status::Ok;
}

ModuleManager& ModuleManager::instance() noexcept {
    static ModuleManager s_mgr;
    return s_mgr;
}

bool ModuleManager::enterRecoveryGuard(cortez_jmp_buf** outBuf) noexcept {
    if (m_recoveryActive) {
        return false;
    }
    m_recoveryActive = true;
    if (outBuf) {
        *outBuf = &m_recoveryEnv;
    }
    return true;
}

void ModuleManager::leaveRecoveryGuard() noexcept {
    m_recoveryActive = false;
}

void ModuleManager::extractBaseName(const char* path, char* outName, usize maxLen) noexcept {
    if (!path || maxLen == 0) return;
    const char* slash = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') slash = p + 1;
    }
    usize i = 0;
    while (slash[i] && slash[i] != '.' && i + 1 < maxLen) {
        outName[i] = slash[i];
        ++i;
    }
    outName[i] = '\0';
    if (outName[0] == '\0') {
        str::copy(outName, "mod", maxLen);
    }
}

LoadedModule* ModuleManager::findById(u32 id) noexcept {
    if (id == 0) return nullptr;
    for (usize i = 0; i < MAX_LOADED_MODULES; ++i) {
        if (m_modules[i].inUse && m_modules[i].id == id) {
            return &m_modules[i];
        }
    }
    return nullptr;
}

LoadedModule* ModuleManager::findByName(const char* name) noexcept {
    if (!name) return nullptr;
    for (usize i = 0; i < MAX_LOADED_MODULES; ++i) {
        if (m_modules[i].inUse && str::compare(m_modules[i].name, name) == 0) {
            return &m_modules[i];
        }
    }
    return nullptr;
}

int ModuleManager::registerCommandForCurrent(const char* name,
                                             const char* desc,
                                             const char* usage,
                                             cortez_cmd_fn fn) noexcept {
    if (!name || !fn) return -1;
    LoadedModule* mod = findById(m_activeModuleId);
    if (mod && mod->commandCount >= LoadedModule::MAX_MOD_COMMANDS) {
        return -1;
    }

    auto* cmd = new ModuleShellCommand(m_activeModuleId, name, desc, usage, fn);
    if (!cmd) return -1;

    if (shell::Shell::instance().registerCommand(cmd) != Status::Ok) {
        delete cmd;
        return -1;
    }

    if (mod) {
        mod->commands[mod->commandCount++] = cmd;
    }
    return 0;
}

int ModuleManager::unregisterCommandByName(const char* name) noexcept {
    if (!name) return -1;
    for (usize m = 0; m < MAX_LOADED_MODULES; ++m) {
        if (!m_modules[m].inUse) continue;
        auto& mod = m_modules[m];
        for (usize i = 0; i < mod.commandCount; ++i) {
            if (mod.commands[i] && str::compare(mod.commands[i]->getName(), name) == 0) {
                shell::Shell::instance().unregisterCommand(mod.commands[i]);
                delete mod.commands[i];
                for (usize j = i; j + 1 < mod.commandCount; ++j) {
                    mod.commands[j] = mod.commands[j + 1];
                }
                mod.commands[--mod.commandCount] = nullptr;
                return 0;
            }
        }
    }
    return (shell::Shell::instance().unregisterCommandByName(name) == Status::Ok) ? 0 : -1;
}

int ModuleManager::registerCharDevForCurrent(const char* name,
                                             const cortez_chardev_ops* ops,
                                             void* ctx) noexcept {
    if (!name || !ops) return -1;
    LoadedModule* mod = findById(m_activeModuleId);
    if (mod && mod->charDevCount >= LoadedModule::MAX_MOD_CHARDEVS) {
        return -1;
    }

    auto* dev = new ModuleCharDevice(m_activeModuleId, name, ops, ctx);
    if (!dev) return -1;

    if (drivers::DeviceRegistry::instance().registerCharDevice(dev) != Status::Ok) {
        delete dev;
        return -1;
    }

    if (mod) {
        mod->charDevs[mod->charDevCount++] = dev;
    }
    return 0;
}

int ModuleManager::unregisterCharDevByName(const char* name) noexcept {
    if (!name) return -1;
    for (usize m = 0; m < MAX_LOADED_MODULES; ++m) {
        if (!m_modules[m].inUse) continue;
        auto& mod = m_modules[m];
        for (usize i = 0; i < mod.charDevCount; ++i) {
            if (mod.charDevs[i] && str::compare(mod.charDevs[i]->getName(), name) == 0) {
                drivers::DeviceRegistry::instance().unregisterCharDevice(mod.charDevs[i]);
                delete mod.charDevs[i];
                for (usize j = i; j + 1 < mod.charDevCount; ++j) {
                    mod.charDevs[j] = mod.charDevs[j + 1];
                }
                mod.charDevs[--mod.charDevCount] = nullptr;
                return 0;
            }
        }
    }
    return -1;
}

int ModuleManager::registerIrqForCurrent(u8 vector, cortez_irq_fn handler, void* ctx) noexcept {
    if (!handler || vector < 32) return -1;
    LoadedModule* mod = findById(m_activeModuleId);
    if (mod && mod->irqCount >= LoadedModule::MAX_MOD_IRQS) {
        return -1;
    }

    auto* wrapper = new ModuleIrqHandler(m_activeModuleId, vector, handler, ctx);
    if (!wrapper) return -1;

    arch::IdtManager::instance().registerHandler(vector, wrapper);
    if (mod) {
        mod->irqHandlers[mod->irqCount++] = wrapper;
    }
    return 0;
}

void ModuleManager::recordIsaIrqRoute(u8 isaIrq, u8 vector) noexcept {
    for (usize m = 0; m < MAX_LOADED_MODULES; ++m) {
        if (!m_modules[m].inUse) continue;
        auto& mod = m_modules[m];
        for (usize i = 0; i < mod.irqCount; ++i) {
            if (mod.irqHandlers[i] && mod.irqHandlers[i]->getVector() == vector) {
                mod.irqHandlers[i]->setIsaIrq(static_cast<i32>(isaIrq));
                return;
            }
        }
    }
}

void ModuleManager::unregisterIrq(u8 vector) noexcept {
    for (usize m = 0; m < MAX_LOADED_MODULES; ++m) {
        if (!m_modules[m].inUse) continue;
        auto& mod = m_modules[m];
        for (usize i = 0; i < mod.irqCount; ++i) {
            if (mod.irqHandlers[i] && mod.irqHandlers[i]->getVector() == vector) {
                if (mod.irqHandlers[i]->getIsaIrq() >= 0) {
                    arch::ApicController::instance().maskIsaIrq(
                        static_cast<u8>(mod.irqHandlers[i]->getIsaIrq()));
                }
                arch::IdtManager::instance().unregisterHandler(vector);
                arch::IdtManager::instance().freeVector(vector);
                delete mod.irqHandlers[i];
                for (usize j = i; j + 1 < mod.irqCount; ++j) {
                    mod.irqHandlers[j] = mod.irqHandlers[j + 1];
                }
                mod.irqHandlers[--mod.irqCount] = nullptr;
                return;
            }
        }
    }
    arch::IdtManager::instance().unregisterHandler(vector);
    arch::IdtManager::instance().freeVector(vector);
}

u64 ModuleManager::allocPagesForCurrent(usize count) noexcept {
    if (count == 0) return 0;
    auto res = memory::BitmapPmm::instance().allocPages(count);
    if (res.isError()) return 0;
    phys_addr_t phys = res.value();

    LoadedModule* mod = findById(m_activeModuleId);
    if (mod && mod->pageAllocCount < LoadedModule::MAX_MOD_PAGES) {
        mod->pageAllocs[mod->pageAllocCount++] = { phys, count };
    }
    return phys;
}

void ModuleManager::freePagesForCurrent(u64 physAddr, usize count) noexcept {
    if (physAddr == 0 || count == 0) return;
    for (usize m = 0; m < MAX_LOADED_MODULES; ++m) {
        if (!m_modules[m].inUse) continue;
        auto& mod = m_modules[m];
        for (usize i = 0; i < mod.pageAllocCount; ++i) {
            if (mod.pageAllocs[i].physAddr == physAddr) {
                usize actualCount = mod.pageAllocs[i].pageCount;
                for (usize j = i; j + 1 < mod.pageAllocCount; ++j) {
                    mod.pageAllocs[j] = mod.pageAllocs[j + 1];
                }
                --mod.pageAllocCount;
                (void)memory::BitmapPmm::instance().freePages(physAddr, actualCount);
                return;
            }
        }
    }
    (void)memory::BitmapPmm::instance().freePages(physAddr, count);
}

void ModuleManager::cleanupModuleResources(LoadedModule& mod) noexcept {
    // 1. Kill any background kernel tasks spawned by this module
    (void)proc::Scheduler::instance().killModuleTasks(mod.id);

    // 2. Unregister & mask any IRQs still owned by this module
    for (usize i = 0; i < mod.irqCount; ++i) {
        if (mod.irqHandlers[i]) {
            if (mod.irqHandlers[i]->getIsaIrq() >= 0) {
                arch::ApicController::instance().maskIsaIrq(
                    static_cast<u8>(mod.irqHandlers[i]->getIsaIrq()));
            }
            u8 vec = mod.irqHandlers[i]->getVector();
            arch::IdtManager::instance().unregisterHandler(vec);
            arch::IdtManager::instance().freeVector(vec);
            delete mod.irqHandlers[i];
            mod.irqHandlers[i] = nullptr;
        }
    }
    mod.irqCount = 0;

    // 3. Unregister any character devices still owned by this module
    for (usize i = 0; i < mod.charDevCount; ++i) {
        if (mod.charDevs[i]) {
            drivers::DeviceRegistry::instance().unregisterCharDevice(mod.charDevs[i]);
            delete mod.charDevs[i];
            mod.charDevs[i] = nullptr;
        }
    }
    mod.charDevCount = 0;

    // 4. Unregister any shell commands still owned by this module
    for (usize i = 0; i < mod.commandCount; ++i) {
        if (mod.commands[i]) {
            shell::Shell::instance().unregisterCommand(mod.commands[i]);
            delete mod.commands[i];
            mod.commands[i] = nullptr;
        }
    }
    mod.commandCount = 0;

    // 5. Free any physical page allocations still owned by this module
    for (usize i = 0; i < mod.pageAllocCount; ++i) {
        if (mod.pageAllocs[i].physAddr != 0) {
            (void)memory::BitmapPmm::instance().freePages(
                mod.pageAllocs[i].physAddr, mod.pageAllocs[i].pageCount);
            mod.pageAllocs[i] = { 0, 0 };
        }
    }
    mod.pageAllocCount = 0;
}

Status ModuleManager::load(const char* path) noexcept {
    if (!path || path[0] == '\0') return Status::InvalidArgument;

    LoadedModule* slot = nullptr;
    for (usize i = 0; i < MAX_LOADED_MODULES; ++i) {
        if (!m_modules[i].inUse) {
            slot = &m_modules[i];
            break;
        }
    }
    if (!slot) {
        pr_err("mod", "Module table full (max %zu modules)", MAX_LOADED_MODULES);
        return Status::OutOfMemory;
    }

    TCCState* s = tcc_new();
    if (!s) {
        return Status::OutOfMemory;
    }

    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_set_error_func(s, const_cast<char*>(path), &tccErrorCallback);
    tcc_add_include_path(s, "/ext/include");
    tcc_add_sysinclude_path(s, "/ext/include");
    registerKernelSymbols(s);

    if (tcc_add_file(s, path) < 0) {
        tcc_delete(s);
        return Status::InvalidArgument;
    }

    int codeSize = tcc_relocate(s, nullptr);
    if (codeSize <= 0) {
        tcc_delete(s);
        return Status::InvalidArgument;
    }

    void* codeMem = memory::ModuleArea::instance().allocate(static_cast<usize>(codeSize));
    if (!codeMem) {
        pr_err("mod", "Failed to allocate %d bytes in ModuleArea for %s", codeSize, path);
        tcc_delete(s);
        return Status::OutOfMemory;
    }

    if (tcc_relocate(s, codeMem) < 0) {
        (void)memory::ModuleArea::instance().free(codeMem, static_cast<usize>(codeSize));
        tcc_delete(s);
        return Status::InvalidArgument;
    }

    // Extract module metadata and entry points
    auto initFn = reinterpret_cast<int (*)(void)>(tcc_get_symbol(s, "module_init"));
    auto exitFn = reinterpret_cast<void (*)(void)>(tcc_get_symbol(s, "module_exit"));
    if (!initFn) {
        pr_err("mod", "%s: missing required 'int module_init(void)' symbol", path);
        (void)memory::ModuleArea::instance().free(codeMem, static_cast<usize>(codeSize));
        tcc_delete(s);
        return Status::NotFound;
    }

    char modName[32]{};
    extractBaseName(path, modName, sizeof(modName));
    if (auto* customName = reinterpret_cast<const char*>(tcc_get_symbol(s, "__cortez_mod_name"))) {
        if (customName[0] != '\0') {
            str::copy(modName, customName, sizeof(modName));
        }
    }

    char modDesc[80]{};
    str::copy(modDesc, "Runtime C Kernel Module", sizeof(modDesc));
    if (auto* customDesc = reinterpret_cast<const char*>(tcc_get_symbol(s, "__cortez_mod_desc"))) {
        if (customDesc[0] != '\0') {
            str::copy(modDesc, customDesc, sizeof(modDesc));
        }
    }

    if (findByName(modName) != nullptr) {
        pr_warn("mod", "Module '%s' is already loaded", modName);
        (void)memory::ModuleArea::instance().free(codeMem, static_cast<usize>(codeSize));
        tcc_delete(s);
        return Status::AlreadyExists;
    }

    // Populate module slot and copy exported symbol table before freeing TCCState
    memset(slot, 0, sizeof(LoadedModule));
    slot->inUse      = true;
    slot->id         = m_nextModuleId++;
    slot->codeBase   = codeMem;
    slot->codeSize   = static_cast<usize>(codeSize);
    slot->initFn     = initFn;
    slot->exitFn     = exitFn;
    str::copy(slot->name, modName, sizeof(slot->name));
    str::copy(slot->description, modDesc, sizeof(slot->description));
    str::copy(slot->sourcePath, path, sizeof(slot->sourcePath));

    size_t symCount = tcc_get_exported_symbol_count(s);
    for (size_t i = 0; i < symCount && slot->symbolCount < LoadedModule::MAX_MOD_SYMBOLS; ++i) {
        const char* sname = tcc_get_exported_symbol_name(s, i);
        void*       saddr = tcc_get_exported_symbol_addr(s, i);
        if (sname && saddr) {
            auto& entry = slot->symbols[slot->symbolCount++];
            str::copy(entry.name, sname, sizeof(entry.name));
            entry.addr = reinterpret_cast<virt_addr_t>(saddr);
        }
    }

    tcc_delete(s);

    // Execute module_init() under active module context & fault recovery guard
    u32 prevMod = m_activeModuleId;
    m_activeModuleId = slot->id;

    cortez_jmp_buf* jmpEnv = nullptr;
    bool guarded = enterRecoveryGuard(&jmpEnv);
    int initRet = -1;

    if (guarded && jmpEnv) {
        if (cortez_setjmp(*jmpEnv) == 0) {
            initRet = slot->initFn();
        } else {
            pr_err("mod", "Module '%s' faulted during module_init(); rolling back", slot->name);
            initRet = -1;
        }
        leaveRecoveryGuard();
    } else {
        initRet = slot->initFn();
    }

    m_activeModuleId = prevMod;

    if (initRet != 0) {
        pr_err("mod", "Module '%s' module_init() returned %d; unloading", slot->name, initRet);
        cleanupModuleResources(*slot);
        (void)memory::ModuleArea::instance().free(slot->codeBase, slot->codeSize);
        slot->inUse = false;
        return Status::IoError;
    }

    pr_ok("mod", "Loaded C module '%s' (%zu bytes at 0x%llx) — %s",
          slot->name, slot->codeSize, reinterpret_cast<u64>(slot->codeBase), slot->description);
    return Status::Ok;
}

Status ModuleManager::unload(const char* name) noexcept {
    LoadedModule* mod = findByName(name);
    if (!mod) {
        return Status::NotFound;
    }

    if (mod->exitFn) {
        u32 prevMod = m_activeModuleId;
        m_activeModuleId = mod->id;

        cortez_jmp_buf* jmpEnv = nullptr;
        bool guarded = enterRecoveryGuard(&jmpEnv);
        if (guarded && jmpEnv) {
            if (cortez_setjmp(*jmpEnv) == 0) {
                mod->exitFn();
            } else {
                pr_warn("mod", "Module '%s' faulted during module_exit(); continuing cleanup", mod->name);
            }
            leaveRecoveryGuard();
        } else {
            mod->exitFn();
        }

        m_activeModuleId = prevMod;
    }

    cleanupModuleResources(*mod);
    (void)memory::ModuleArea::instance().free(mod->codeBase, mod->codeSize);

    pr_ok("mod", "Unloaded C module '%s'", mod->name);
    memset(mod, 0, sizeof(LoadedModule));
    return Status::Ok;
}

Status ModuleManager::runProgram(const char* path, usize argc, const char** argv, i32* outExitCode) noexcept {
    if (!path || path[0] == '\0') return Status::InvalidArgument;

    LoadedModule* tempSlot = nullptr;
    for (usize i = 0; i < MAX_LOADED_MODULES; ++i) {
        if (!m_modules[i].inUse) {
            tempSlot = &m_modules[i];
            break;
        }
    }
    if (!tempSlot) {
        return Status::OutOfMemory;
    }

    TCCState* s = tcc_new();
    if (!s) return Status::OutOfMemory;

    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_set_error_func(s, const_cast<char*>(path), &tccErrorCallback);
    tcc_add_include_path(s, "/ext/include");
    tcc_add_sysinclude_path(s, "/ext/include");
    registerKernelSymbols(s);

    if (tcc_add_file(s, path) < 0) {
        tcc_delete(s);
        return Status::InvalidArgument;
    }

    int codeSize = tcc_relocate(s, nullptr);
    if (codeSize <= 0) {
        tcc_delete(s);
        return Status::InvalidArgument;
    }

    void* codeMem = memory::ModuleArea::instance().allocate(static_cast<usize>(codeSize));
    if (!codeMem) {
        tcc_delete(s);
        return Status::OutOfMemory;
    }

    if (tcc_relocate(s, codeMem) < 0) {
        (void)memory::ModuleArea::instance().free(codeMem, static_cast<usize>(codeSize));
        tcc_delete(s);
        return Status::InvalidArgument;
    }

    auto mainFn = reinterpret_cast<int (*)(int, const char**)>(tcc_get_symbol(s, "main"));
    if (!mainFn) {
        pr_err("run", "%s: missing 'int main(int argc, const char** argv)' entry point", path);
        (void)memory::ModuleArea::instance().free(codeMem, static_cast<usize>(codeSize));
        tcc_delete(s);
        return Status::NotFound;
    }

    memset(tempSlot, 0, sizeof(LoadedModule));
    tempSlot->inUse    = true;
    tempSlot->id       = m_nextModuleId++;
    tempSlot->codeBase = codeMem;
    tempSlot->codeSize = static_cast<usize>(codeSize);
    extractBaseName(path, tempSlot->name, sizeof(tempSlot->name));
    str::copy(tempSlot->sourcePath, path, sizeof(tempSlot->sourcePath));

    size_t symCount = tcc_get_exported_symbol_count(s);
    for (size_t i = 0; i < symCount && tempSlot->symbolCount < LoadedModule::MAX_MOD_SYMBOLS; ++i) {
        const char* sname = tcc_get_exported_symbol_name(s, i);
        void*       saddr = tcc_get_exported_symbol_addr(s, i);
        if (sname && saddr) {
            auto& entry = tempSlot->symbols[tempSlot->symbolCount++];
            str::copy(entry.name, sname, sizeof(entry.name));
            entry.addr = reinterpret_cast<virt_addr_t>(saddr);
        }
    }

    tcc_delete(s);

    u32 prevMod = m_activeModuleId;
    m_activeModuleId = tempSlot->id;

    cortez_jmp_buf* jmpEnv = nullptr;
    bool guarded = enterRecoveryGuard(&jmpEnv);
    i32 exitCode = -1;

    if (guarded && jmpEnv) {
        if (cortez_setjmp(*jmpEnv) == 0) {
            exitCode = mainFn(static_cast<int>(argc), argv);
        } else {
            kprintf("\033[1;31m[FAULT] Program '%s' terminated after CPU exception; recovered to shell.\033[0m\n",
                    path);
            exitCode = -1;
        }
        leaveRecoveryGuard();
    } else {
        exitCode = mainFn(static_cast<int>(argc), argv);
    }

    m_activeModuleId = prevMod;

    cleanupModuleResources(*tempSlot);
    (void)memory::ModuleArea::instance().free(tempSlot->codeBase, tempSlot->codeSize);
    memset(tempSlot, 0, sizeof(LoadedModule));

    if (outExitCode) {
        *outExitCode = exitCode;
    }
    return Status::Ok;
}

Status ModuleManager::checkCompile(const char* path) noexcept {
    if (!path || path[0] == '\0') return Status::InvalidArgument;

    TCCState* s = tcc_new();
    if (!s) return Status::OutOfMemory;

    tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc_set_error_func(s, const_cast<char*>(path), &tccErrorCallback);
    tcc_add_include_path(s, "/ext/include");
    tcc_add_sysinclude_path(s, "/ext/include");
    registerKernelSymbols(s);

    if (tcc_add_file(s, path) < 0) {
        tcc_delete(s);
        return Status::InvalidArgument;
    }

    int codeSize = tcc_relocate(s, nullptr);
    if (codeSize <= 0) {
        tcc_delete(s);
        return Status::InvalidArgument;
    }

    size_t symCount = tcc_get_exported_symbol_count(s);
    kprintf("\033[1;32m[cc]\033[0m %s compiled cleanly (%d bytes of x86_64 code, %zu symbols):\n",
            path, codeSize, symCount);
    for (size_t i = 0; i < symCount; ++i) {
        const char* sname = tcc_get_exported_symbol_name(s, i);
        if (sname) {
            kprintf("  - %s\n", sname);
        }
    }

    tcc_delete(s);
    return Status::Ok;
}

usize ModuleManager::autoload(const char* dirPath) noexcept {
    if (!dirPath) return 0;

    const char* cwd = "/";
    usize loadedCount = 0;

    for (usize idx = 0;; ++idx) {
        fs::DirEntry entry{};
        if (fs::VfsManager::instance().readDir(cwd, dirPath, idx, &entry) != Status::Ok) {
            break;
        }
        if (entry.type != fs::NodeType::File) {
            continue;
        }
        usize nameLen = str::length(entry.name);
        if (nameLen < 3 || entry.name[nameLen - 2] != '.' || entry.name[nameLen - 1] != 'c') {
            continue;
        }

        char fullPath[160]{};
        str::copy(fullPath, dirPath, sizeof(fullPath));
        usize dlen = str::length(fullPath);
        if (dlen > 0 && fullPath[dlen - 1] != '/' && dlen + 1 < sizeof(fullPath)) {
            fullPath[dlen++] = '/';
            fullPath[dlen] = '\0';
        }
        str::concat(fullPath, entry.name, sizeof(fullPath));

        if (load(fullPath) == Status::Ok) {
            ++loadedCount;
        }
    }

    return loadedCount;
}

bool ModuleManager::resolveAddress(virt_addr_t rip,
                                   const char** outModName,
                                   const char** outSymName,
                                   u64* outOffset) const noexcept {
    for (usize i = 0; i < MAX_LOADED_MODULES; ++i) {
        if (!m_modules[i].inUse) continue;
        const auto& mod = m_modules[i];
        virt_addr_t base = reinterpret_cast<virt_addr_t>(mod.codeBase);
        if (rip >= base && rip < base + mod.codeSize) {
            if (outModName) *outModName = mod.name;
            const char* bestSym = "<module_base>";
            virt_addr_t bestAddr = base;
            for (usize s = 0; s < mod.symbolCount; ++s) {
                if (mod.symbols[s].addr <= rip && mod.symbols[s].addr >= bestAddr) {
                    bestAddr = mod.symbols[s].addr;
                    bestSym  = mod.symbols[s].name;
                }
            }
            if (outSymName) *outSymName = bestSym;
            if (outOffset)  *outOffset  = rip - bestAddr;
            return true;
        }
    }
    return false;
}

bool ModuleManager::handleException(arch::InterruptFrame* frame, const char* exceptionName) noexcept {
    if (!frame) return false;

    const char* modName = nullptr;
    const char* symName = nullptr;
    u64         symOff  = 0;
    bool inModule = resolveAddress(frame->rip, &modName, &symName, &symOff);

    if (inModule) {
        pr_err("FAULT", "Exception (%s) in module '%s' (%s+0x%llx) at RIP=0x%016llx CR2=0x%016llx",
               exceptionName ? exceptionName : "CPU Exception",
               modName, symName, symOff, frame->rip, arch::readCr2());
    }

    if (m_recoveryActive) {
        if (!inModule) {
            pr_err("FAULT", "Exception (%s) during module call at RIP=0x%016llx CR2=0x%016llx",
                   exceptionName ? exceptionName : "CPU Exception",
                   frame->rip, arch::readCr2());
        }
        // Rewrite InterruptFrame to return via iretq directly to the cortez_setjmp recovery point
        frame->rbx = m_recoveryEnv[0];
        frame->rbp = m_recoveryEnv[1];
        frame->r12 = m_recoveryEnv[2];
        frame->r13 = m_recoveryEnv[3];
        frame->r14 = m_recoveryEnv[4];
        frame->r15 = m_recoveryEnv[5];
        frame->rsp = m_recoveryEnv[6];
        frame->rip = m_recoveryEnv[7];
        frame->rax = 1;
        return true;
    }

    return false;
}

} // namespace cortez::module
