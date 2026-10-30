#include "common.h"
#include "commands.h"
#include "banner.h"
#include "color.h"

/*
 * commands.c - the dispatch table and the run/suggest logic.
 * The table is the single place where a verb is bound to its
 * implementation, so --help, error suggestions and execution can
 * never disagree about which commands exist.
 */

const Command kCommands[] = {
    { "init",      cmd_init,      "create a new project" },
    { "new",       cmd_init,      "alias of init" },
    { "build",     cmd_build,     "run the full frontend pipeline" },
    { "check",     cmd_check,     "verify syntax and semantics" },
    { "run",       cmd_run,       "execute the compiled program" },
    { "test",      cmd_test,      "compile and list discovered tests" },
    { "bench",     cmd_bench,     "measure lexer/parser throughput" },
    { "clean",     cmd_clean,     "remove build/, target/, debug/" },
    { "tree",      cmd_tree,      "print the project directory layout" },
    { "clippy",    cmd_clippy,    "check the clean-code rules" },
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
    { "config",    cmd_config,    "read or write a configuration value" },
    { "doc",       cmd_doc,       "regenerate doc.md from doc comments" },
    { "state",     cmd_state,     "emit a static frontend archive" },
    { "shared",    cmd_shared,    "emit a shared frontend archive" },
    { NULL, NULL, NULL }
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
    fprintf(stderr, "%spaxsy:%s unknown command '%s%s%s'\n",
            c_red, c_reset, c_yellow, cli->command, c_reset);
    fprintf(stderr, "%sdid you mean one of these?%s\n", c_dim, c_reset);
    suggest(cli->command);
    fprintf(stderr, "%srun 'paxsy --help' for the full command list%s\n",
            c_dim, c_reset);
    return 2;
}
