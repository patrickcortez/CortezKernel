/**
 * @file module_commands.cpp
 * @brief Implementation of insmod, rmmod, lsmod, run, and cc shell commands.
 */

#include "module_commands.hpp"
#include "../../module/module_manager.hpp"
#include "../../memory/module_area.hpp"
#include "../../proc/scheduler.hpp"
#include "../../fs/path_resolver.hpp"
#include "../../lib/printk.hpp"
#include "../../lib/string_utils.hpp"

namespace cortez::shell::commands {

namespace {

void resolveTaskPath(const char* inputPath, char* outResolved, usize maxLen) {
    const char* cwd = "/";
    if (const auto* t = proc::Scheduler::instance().getCurrentTask()) {
        cwd = t->getCwd();
    }
    fs::PathResolver::normalize(cwd, inputPath, outResolved, maxLen);
}

} // namespace

Status InsmodCommand::execute(const CommandArgs& args) {
    if (args.argc < 2) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }

    char fullPath[256]{};
    resolveTaskPath(args.argv[1], fullPath, sizeof(fullPath));
    return module::ModuleManager::instance().load(fullPath);
}

Status RmmodCommand::execute(const CommandArgs& args) {
    if (args.argc < 2) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }

    return module::ModuleManager::instance().unload(args.argv[1]);
}

Status LsmodCommand::execute(const CommandArgs& args) {
    (void)args;
    auto& mgr = module::ModuleManager::instance();

    kprintf("\033[1;36m%-4s %-14s %-18s %-8s %-6s %-6s %-6s %s\033[0m\n",
            "ID", "MODULE", "BASE ADDR", "SIZE", "CMDS", "DEVS", "IRQS", "DESCRIPTION");

    usize activeCount = 0;
    for (usize i = 0; i < mgr.getModuleCount(); ++i) {
        const auto* mod = mgr.getModuleAt(i);
        if (!mod) continue;
        ++activeCount;
        kprintf("%-4u %-14s 0x%016llx %-8zu %-6zu %-6zu %-6zu %s\n",
                mod->id,
                mod->name,
                reinterpret_cast<u64>(mod->codeBase),
                mod->codeSize,
                mod->commandCount,
                mod->charDevCount,
                mod->irqCount,
                mod->description);
    }

    kprintf("\nLoaded modules: %zu | ModuleArea used: %zu bytes\n",
            activeCount, memory::ModuleArea::instance().getUsedBytes());
    return Status::Ok;
}

Status RunCommand::execute(const CommandArgs& args) {
    if (args.argc < 2) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }

    char fullPath[256]{};
    resolveTaskPath(args.argv[1], fullPath, sizeof(fullPath));

    // Pass argv[1..] as argv[0..] to main(argc, argv)
    const char* progArgv[CommandArgs::MAX_ARGS]{};
    usize progArgc = args.argc - 1;
    for (usize i = 0; i < progArgc; ++i) {
        progArgv[i] = args.argv[i + 1];
    }

    i32 exitCode = 0;
    Status st = module::ModuleManager::instance().runProgram(fullPath, progArgc, progArgv, &exitCode);
    if (st == Status::Ok && exitCode != 0) {
        kprintf("\033[33m[run] exited with code %d\033[0m\n", exitCode);
    }
    return st;
}

Status CcCommand::execute(const CommandArgs& args) {
    if (args.argc < 2) {
        kprintf("Usage: %s\n", getUsage());
        return Status::InvalidArgument;
    }

    char fullPath[256]{};
    resolveTaskPath(args.argv[1], fullPath, sizeof(fullPath));
    return module::ModuleManager::instance().checkCompile(fullPath);
}

} // namespace cortez::shell::commands
