/*
 * hello.c — Sample Runtime-Compiled Cortez Shell Command
 * Modify this file with 'edit /ext/commands/hello.c', then reload with 'rmmod hello' & 'insmod /ext/commands/hello.c'.
 */

#include <cortez.h>

static int cmd_hello(int argc, const char** argv) {
    const char* target = "Cortez Kernel";
    if (argc > 1) {
        target = argv[1];
    }
    k_printf("\033[1;32mHello, %s!\033[0m (compiled from /ext/commands/hello.c at runtime, uptime=%llu ms)\n",
             target, k_ticks());
    return 0;
}

int module_init(void) {
    return k_register_command("hello",
                              "Print a greeting from a runtime-compiled C command",
                              "hello [name]",
                              cmd_hello);
}

void module_exit(void) {
    k_unregister_command("hello");
}
