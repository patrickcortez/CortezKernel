/**
 * @file shell.cpp
 * @brief Implementation of the interactive Cortez Kernel Shell.
 */

#include "shell.hpp"
#include "commands/fs_commands.hpp"
#include "commands/sys_commands.hpp"
#include "../drivers/input/ps2_keyboard.hpp"
#include "../drivers/video/terminal_driver.hpp"
#include "../proc/scheduler.hpp"
#include "../fs/vfs_manager.hpp"
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
commands::DmesgCommand    s_cmdDmesg;
commands::LsblkCommand    s_cmdLsblk;
commands::LspciCommand    s_cmdLspci;
commands::LsusbCommand    s_cmdLsusb;
commands::NvmeInfoCommand s_cmdNvme;
commands::PsCommand       s_cmdPs;
commands::FreeCommand     s_cmdFree;
commands::UnameCommand    s_cmdUname;
commands::ClearCommand    s_cmdClear;
commands::RebootCommand   s_cmdReboot;
} // namespace

Shell& Shell::instance() noexcept {
    static Shell s_shell;
    return s_shell;
}

void Shell::registerCommand(IShellCommand* cmd) noexcept {
    if (cmd && m_cmdCount < MAX_COMMANDS) {
        m_commands[m_cmdCount++] = cmd;
    }
}

void Shell::init() noexcept {
    m_cmdCount = 0;
    registerCommand(&s_cmdHelp);
    registerCommand(&s_cmdLs);
    registerCommand(&s_cmdCd);
    registerCommand(&s_cmdPwd);
    registerCommand(&s_cmdCat);
    registerCommand(&s_cmdWrite);
    registerCommand(&s_cmdAppend);
    registerCommand(&s_cmdTouch);
    registerCommand(&s_cmdMkdir);
    registerCommand(&s_cmdRm);
    registerCommand(&s_cmdStat);
    registerCommand(&s_cmdDf);
    registerCommand(&s_cmdSync);
    registerCommand(&s_cmdMkfs);
    registerCommand(&s_cmdDmesg);
    registerCommand(&s_cmdLsblk);
    registerCommand(&s_cmdLspci);
    registerCommand(&s_cmdLsusb);
    registerCommand(&s_cmdNvme);
    registerCommand(&s_cmdPs);
    registerCommand(&s_cmdFree);
    registerCommand(&s_cmdUname);
    registerCommand(&s_cmdClear);
    registerCommand(&s_cmdReboot);
}

void Shell::printPrompt() const noexcept {
    const proc::Task* t   = proc::Scheduler::instance().getCurrentTask();
    const char*       cwd = t ? t->getCwd() : "/";
    kprintf("\033[92mroot@cortez-kernel\033[0m:\033[94m%s\033[0m# ", cwd);
}

void Shell::readLine(char* outBuf, usize maxLen) noexcept {
    usize pos = 0;
    outBuf[0] = '\0';

    auto& kbd = drivers::input::Ps2KeyboardDriver::instance();

    for (;;) {
        if (!kbd.hasData()) {
            kbd.pollHardware();
            if (!kbd.hasData()) {
                proc::Scheduler::instance().yield();
            }
        }

        char ch = kbd.readCharBlocking();
        if (ch == '\r' || ch == '\n') {
            kprintf("\n");
            outBuf[pos] = '\0';
            return;
        }
        if (ch == '\x0C') { // Ctrl+L
            drivers::video::TerminalDriver::instance().clear();
            printPrompt();
            klog::Logger::instance().writeRaw(outBuf, pos);
            continue;
        }
        if (ch == '\b' || ch == 127) {
            if (pos > 0) {
                --pos;
                outBuf[pos] = '\0';
                kprintf("\b");
            }
            continue;
        }
        if (static_cast<u8>(ch) >= 0x20U && pos + 1 < maxLen) {
            outBuf[pos++] = ch;
            outBuf[pos]   = '\0';
            kprintf("%c", ch);
        }
    }
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
