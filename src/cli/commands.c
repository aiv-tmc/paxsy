#include "common.h"
#include "commands.h"
#include "banner.h"
#include "color.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>

/*
 * commands.c - the dispatch table and the run/suggest logic.
 * The table is the single place where a verb is bound to its
 * implementation, so --help, error suggestions and execution can
 * never disagree about which commands exist.
 */

const Command kCommands[] = {
    { "new",       cmd_new,       "create a new project" },
    { "build",     cmd_build,     "run the full frontend pipeline" },
    { "check",     cmd_check,     "verify syntax and semantics" },
    { "run",       cmd_run,       "execute the compiled program" },
    { "link",      cmd_link,      "link objects into an executable" },
    { "test",      cmd_test,      "compile and list discovered tests" },
    { "bench",     cmd_bench,     "measure lexer throughput" },
    { "clean",     cmd_clean,     "remove build/, target/, debug/" },
    { "tree",      cmd_tree,      "print the project directory layout" },
    { "clippy",    cmd_clippy,    "check the clean-code rules" },
    { "fmt",       cmd_fmt,       "reformat the sources mechanically" },
    { "fetch",     cmd_fetch,     "rebuild the local package index" },
    { "search",    cmd_search,    "search the local package index" },
    { "install",   cmd_install,   "install a package from the cache" },
    { "uninstall", cmd_uninstall, "remove an installed package" },
    { "update",    cmd_update,    "refresh an installed package" },
    { "add",       cmd_add,       "record a dependency" },
    { "remove",    cmd_remove,    "drop a dependency" },
    { "list",      cmd_list,      "list installed packages" },
    { "publish",   cmd_publish,   "assemble a distributable tree" },
    { "meta",      cmd_meta,      "print project metadata" },
    { "config",    cmd_config,    "read or write a config value" },
    { "state",     cmd_state,     "emit a .a frontend archive" },
    { "debug",     cmd_debug,     "run the binary under a debugger" },
    { "disasm",    cmd_disasm,    "annotated disassembly of the binary" },
    { "recovery",  cmd_recovery, "restore embedded source or a machine listing" },
    { "shared",    cmd_shared,    "emit a .sfa frontend archive" },
    { NULL,        NULL,          NULL }
};

/* Commands whose name shares the first letter with the typo, used to
 * build the "did you mean" hint. */
static void suggest(const char *unknown) {
    int found = 0;
    for (int i = 0; kCommands[i].name; i++) {
        if (kCommands[i].name[0] == unknown[0]) {
            fprintf(stderr, "        %s%s%s\n",
                    c_cyan, kCommands[i].name, c_reset);
            found = 1;
        }
    }
    if (!found)
        fprintf(stderr, "        %s(no commands start with '%c')%s\n",
                c_dim, unknown[0], c_reset);
}

int commands_run(const Cli *cli) {
    color_setup();
    for (int i = 0; kCommands[i].name; i++) {
        if (strcmp(cli->command, kCommands[i].name) == 0)
            return kCommands[i].run(cli);
    }
    /* A path to an executable runs it directly, so a freshly
     * linked artifact can be invoked as 'paxsy <file> [args]'. */
    {
        struct stat st;
        if (stat(cli->command, &st) == 0 && S_ISREG(st.st_mode) &&
            access(cli->command, X_OK) == 0) {
            int argc = 1 + cli->rest_count;
            char **argv = calloc((size_t)argc + 1, sizeof(char *));
            if (!argv) {
                fprintf(stderr, "cannot allocate process argument vector\n");
                return 2;
            }
            argv[0] = (char *)cli->command;
            for (int i = 0; i < cli->rest_count; i++)
                argv[1 + i] = (char *)cli->rest[i];
            argv[argc] = NULL;
            pid_t pid = fork();
            if (pid == 0) {
                execv(cli->command, argv);
                _exit(127);
            }
            free(argv);
            if (pid < 0)
                return 2;
            int status = 0;
            if (waitpid(pid, &status, 0) < 0)
                return 2;
            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            if (WIFSIGNALED(status))
                return 128 + WTERMSIG(status);
            return 2;
        }
    }
    fprintf(stderr, "%sunknown command '%s%s%s'\n",
            c_red, c_yellow, cli->command, c_reset);
    fprintf(stderr, "%sdid you mean one of these?%s\n", c_dim, c_reset);
    suggest(cli->command);
    fprintf(stderr, "%srun 'paxsy --help' for the full command list%s\n",
            c_dim, c_reset);
    return 2;
}
