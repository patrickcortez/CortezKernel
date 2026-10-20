/**
 * @file shell.cpp
 * @brief Implementation of the interactive Cortez Kernel Shell.
 */

#include "shell.hpp"
#include "line_editor.hpp"
#include "commands/fs_commands.hpp"
#include "commands/sys_commands.hpp"
#include "commands/module_commands.hpp"
#include "../drivers/input/ps2_keyboard.hpp"
#include "../drivers/video/terminal_driver.hpp"
#include "../proc/scheduler.hpp"
#include "../fs/vfs_manager.hpp"
#include "../fs/path_resolver.hpp"
#include "../module/module_manager.hpp"
#include "../lib/string_utils.hpp"
#include "../lib/printk.hpp"

namespace cortez::shell {

namespace {
commands::HelpCommand     s_cmdHelp;
commands::LsCommand       s_cmdLs;
commands::CdCommand       s_cmdCd;
commands::PwdCommand      s_cmdPwd;
commands::CatCommand      s_cmdCat;
commands::WriteCommand    s_cmdWrite;
commands::AppendCommand   s_cmdAppend;
commands::TouchCommand    s_cmdTouch;
commands::MkdirCommand    s_cmdMkdir;
commands::RmCommand       s_cmdRm;
commands::StatCommand     s_cmdStat;
commands::DfCommand       s_cmdDf;
commands::SyncCommand     s_cmdSync;
commands::MkfsCommand     s_cmdMkfs;
commands::MountCommand    s_cmdMount;
commands::UmountCommand   s_cmdUmount;
commands::DmesgCommand    s_cmdDmesg;
commands::LsblkCommand    s_cmdLsblk;
commands::LspciCommand    s_cmdLspci;
commands::LsusbCommand    s_cmdLsusb;
commands::NvmeInfoCommand s_cmdNvme;
commands::AhciInfoCommand s_cmdAhci;
commands::StorageInfoCommand s_cmdStorage;
commands::PsCommand       s_cmdPs;
commands::FreeCommand     s_cmdFree;
commands::UnameCommand    s_cmdUname;
commands::ClearCommand    s_cmdClear;
commands::RebootCommand   s_cmdReboot;
commands::HistoryCommand  s_cmdHistory;
commands::NanoCommand     s_cmdNano;
commands::EditCommand     s_cmdEdit;
commands::ChkbdCommand    s_cmdChkbd;
commands::InstallCommand  s_cmdInstall;
commands::InsmodCommand   s_cmdInsmod;
commands::RmmodCommand    s_cmdRmmod;
commands::LsmodCommand    s_cmdLsmod;
commands::RunCommand      s_cmdRun;
commands::CcCommand       s_cmdCc;
} // namespace

Shell& Shell::instance() noexcept {
    static Shell s_shell;
    return s_shell;
}

Status Shell::registerCommand(IShellCommand* cmd) noexcept {
    if (!cmd || !cmd->getName() || !cmd->getName()[0]) return Status::InvalidArgument;
    for (usize i = 0; i < m_cmdCount; ++i) {
        if (m_commands[i] && str::compare(m_commands[i]->getName(), cmd->getName()) == 0) {
            return Status::AlreadyExists;
        }
    }
    if (m_cmdCount >= MAX_COMMANDS) return Status::NoSpace;
    m_commands[m_cmdCount++] = cmd;
    return Status::Ok;
}

Status Shell::unregisterCommand(IShellCommand* cmd) noexcept {
    if (!cmd) return Status::InvalidArgument;
    for (usize i = 0; i < m_cmdCount; ++i) {
        if (m_commands[i] == cmd) {
            for (usize j = i; j + 1 < m_cmdCount; ++j) {
                m_commands[j] = m_commands[j + 1];
            }
            m_commands[--m_cmdCount] = nullptr;
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

Status Shell::unregisterCommandByName(const char* name) noexcept {
    if (!name || !name[0]) return Status::InvalidArgument;
    for (usize i = 0; i < m_cmdCount; ++i) {
        if (m_commands[i] && str::compare(m_commands[i]->getName(), name) == 0) {
            for (usize j = i; j + 1 < m_cmdCount; ++j) {
                m_commands[j] = m_commands[j + 1];
            }
            m_commands[--m_cmdCount] = nullptr;
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

void Shell::init() noexcept {
    m_cmdCount = 0;
    (void)registerCommand(&s_cmdHelp);
    (void)registerCommand(&s_cmdLs);
    (void)registerCommand(&s_cmdCd);
    (void)registerCommand(&s_cmdPwd);
    (void)registerCommand(&s_cmdCat);
    (void)registerCommand(&s_cmdWrite);
    (void)registerCommand(&s_cmdAppend);
    (void)registerCommand(&s_cmdTouch);
    (void)registerCommand(&s_cmdMkdir);
    (void)registerCommand(&s_cmdRm);
    (void)registerCommand(&s_cmdStat);
    (void)registerCommand(&s_cmdDf);
    (void)registerCommand(&s_cmdSync);
    (void)registerCommand(&s_cmdMkfs);
    (void)registerCommand(&s_cmdMount);
    (void)registerCommand(&s_cmdUmount);
    (void)registerCommand(&s_cmdDmesg);
    (void)registerCommand(&s_cmdLsblk);
    (void)registerCommand(&s_cmdLspci);
    (void)registerCommand(&s_cmdLsusb);
    (void)registerCommand(&s_cmdNvme);
    (void)registerCommand(&s_cmdAhci);
    (void)registerCommand(&s_cmdStorage);
    (void)registerCommand(&s_cmdPs);
    (void)registerCommand(&s_cmdFree);
    (void)registerCommand(&s_cmdUname);
    (void)registerCommand(&s_cmdClear);
    (void)registerCommand(&s_cmdReboot);
    (void)registerCommand(&s_cmdHistory);
    (void)registerCommand(&s_cmdNano);
    (void)registerCommand(&s_cmdEdit);
    (void)registerCommand(&s_cmdChkbd);
    (void)registerCommand(&s_cmdInstall);
    (void)registerCommand(&s_cmdInsmod);
    (void)registerCommand(&s_cmdRmmod);
    (void)registerCommand(&s_cmdLsmod);
    (void)registerCommand(&s_cmdRun);
    (void)registerCommand(&s_cmdCc);
}

void Shell::printPrompt() const noexcept {
    const proc::Task* t   = proc::Scheduler::instance().getCurrentTask();
    const char*       cwd = t ? t->getCwd() : "/";
    kprintf("\033[92mroot@cortez-kernel\033[0m:\033[94m%s\033[0m# ", cwd);
}

void Shell::readLine(char* outBuf, usize maxLen) noexcept {
    LineEditor::instance().readLine(outBuf, maxLen);
}

void Shell::executeLine(char* line) noexcept {
    if (!line) return;
    while (*line == ' ' || *line == '\t') ++line;
    if (*line == '\0' || *line == '#') return;

    // Support `echo <text> > <file>` (overwrite) and `echo <text> >> <file>` (append)
    if (str::compareN(line, "echo ", 5) == 0) {
        const char* redir = str::findChar(line + 5, '>');
        if (redir) {
            bool isAppend = (redir[1] == '>');
            char textPart[2048]{};
            usize textLen = static_cast<usize>(redir - (line + 5));
            while (textLen > 0 && (line[5 + textLen - 1] == ' ' || line[5 + textLen - 1] == '\t')) {
                --textLen;
            }
            if (textLen + 2 < sizeof(textPart)) {
                for (usize i = 0; i < textLen; ++i) textPart[i] = line[5 + i];
                textPart[textLen]     = '\n';
                textPart[textLen + 1] = '\0';
            }

            const char* filePart = isAppend ? (redir + 2) : (redir + 1);
            while (*filePart == ' ' || *filePart == '\t') ++filePart;
            if (*filePart) {
                const proc::Task* t   = proc::Scheduler::instance().getCurrentTask();
                const char*       cwd = t ? t->getCwd() : "/";
                Status st = Status::Ok;
                if (isAppend) {
                    auto fdRes = fs::VfsManager::instance().open(
                        cwd, filePart, fs::O_WRONLY | fs::O_CREAT | fs::O_APPEND);
                    if (fdRes.isError()) {
                        st = fdRes.status();
                    } else {
                        i32 fd = fdRes.value();
                        auto wRes = fs::VfsManager::instance().write(fd, textPart, str::length(textPart));
                        fs::VfsManager::instance().close(fd);
                        if (wRes.isError()) st = wRes.status();
                    }
                } else {
                    st = fs::VfsManager::instance().writeFileAll(cwd, filePart, textPart, str::length(textPart));
                }
                if (st != Status::Ok) {
                    kprintf("\033[31mecho: %s\033[0m\n", statusToString(st));
                }
                return;
            }
        } else {
            kprintf("%s\n", line + 5);
            return;
        }
    }

    CommandArgs args{};
    char* p = line;
    while (*p && args.argc < CommandArgs::MAX_ARGS) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            args.argv[args.argc++] = p;
            while (*p && *p != quote) ++p;
            if (*p == quote) *p++ = '\0';
        } else {
            args.argv[args.argc++] = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
            if (*p) *p++ = '\0';
        }
    }

    if (args.argc == 0) return;

    for (usize i = 0; i < m_cmdCount; ++i) {
        if (str::compare(m_commands[i]->getName(), args.argv[0]) == 0) {
            Status st = m_commands[i]->execute(args);
            if (st != Status::Ok && st != Status::InvalidArgument) {
                kprintf("\033[31m%s: %s\033[0m\n", args.argv[0], statusToString(st));
            }
            return;
        }
    }

    // Direct executable path execution: ./prog, /bin/prog, or a file with a path
    const char* cmd = args.argv[0];
    bool isPathExecution = (cmd[0] == '/' || (cmd[0] == '.' && cmd[1] == '/'));

    if (isPathExecution) {
        const proc::Task* t   = proc::Scheduler::instance().getCurrentTask();
        const char*       cwd = t ? t->getCwd() : "/";

        char fullPath[256]{};
        fs::PathResolver::normalize(cwd, cmd, fullPath, sizeof(fullPath));

        fs::FileStat fst{};
        if (fs::VfsManager::instance().stat(cwd, fullPath, &fst) == Status::Ok) {
            if (fst.type == fs::NodeType::Directory) {
                kprintf("\033[31mcortez-sh: %s: Is a directory\033[0m\n", cmd);
                return;
            }

            const char* progArgv[CommandArgs::MAX_ARGS]{};
            usize progArgc = args.argc;
            for (usize a = 0; a < progArgc; ++a) {
                progArgv[a] = args.argv[a];
            }

            i32 exitCode = 0;
            Status runSt = module::ModuleManager::instance().runProgram(fullPath, progArgc, progArgv, &exitCode);
            if (runSt == Status::Ok) {
                if (exitCode != 0) {
                    kprintf("\033[33m[%s] exited with code %d\033[0m\n", cmd, exitCode);
                }
            } else {
                kprintf("\033[31m%s: execution failed: %s\033[0m\n", cmd, statusToString(runSt));
            }
            return;
        } else {
            kprintf("\033[31mcortez-sh: %s: No such file or directory\033[0m\n", cmd);
            return;
        }
    }

    kprintf("\033[31mcortez-sh: command not found: %s\033[0m (type 'help' for commands)\n", args.argv[0]);
}

void Shell::runInteractiveLoop() noexcept {
    const fs::IFileSystem* rootFs = fs::VfsManager::instance().getRootFs();
    const char* rootDev = (rootFs && rootFs->getBlockDevice())
                              ? rootFs->getBlockDevice()->getName()
                              : nullptr;

    kprintf("\n\033[36m========================================================================\033[0m\n");
    kprintf("  \033[92mCortez Kernel v1.0.0 (x86_64 Monolithic C++20)\033[0m\n");
    if (rootDev) {
        kprintf("  RootFS mounted on /dev/%s. Type '\033[33mhelp\033[0m' for available commands.\n", rootDev);
    } else {
        kprintf("  No RootFS mounted. Type '\033[33mhelp\033[0m' or '\033[33mlsblk\033[0m' to inspect disks.\n");
    }
    kprintf("\033[36m========================================================================\033[0m\n\n");

    char lineBuf[MAX_LINE_LEN];
    for (;;) {
        printPrompt();
        readLine(lineBuf, sizeof(lineBuf));
        executeLine(lineBuf);
    }
}

} // namespace cortez::shell
