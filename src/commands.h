#ifndef PAXSY_COMMANDS_H
#define PAXSY_COMMANDS_H

/*
 * commands.h - the subcommand registry shared by the dispatcher, the
 * help text and the individual command implementations.
 * Every command is a function taking the parsed CLI and returning a
 * process exit code (0 success, 1 command failure, 2 usage error).
 */

#include "cli.h"

typedef int (*CommandFn)(const Cli *cli);

typedef struct {
    const char *name;    /* subcommand verb as typed by the user */
    CommandFn run;       /* implementation */
    const char *summary; /* one-line description shown in listings */
} Command;

/* Dispatch table; terminated by a NULL name. */
extern const Command kCommands[];

/* Looks up cli->command in the table and runs it. Handles the
 * no-command case (banner + usage) and unknown-command suggestions.
 * Returns the command exit code. */
int commands_run(const Cli *cli);

/* Implementations, grouped by translation unit. */
int cmd_init(const Cli *cli);    /* cmd_project.c: create project   */
int cmd_build(const Cli *cli);   /* cmd_build.c:   full pipeline    */
int cmd_check(const Cli *cli);   /* cmd_build.c:   analyse only     */
int cmd_run(const Cli *cli);     /* cmd_build.c:   execute binary   */
int cmd_test(const Cli *cli);    /* cmd_artifacts.c: compile tests  */
int cmd_bench(const Cli *cli);   /* cmd_artifacts.c: throughput     */
int cmd_clean(const Cli *cli);   /* cmd_project.c: remove artifacts */
int cmd_tree(const Cli *cli);    /* cmd_project.c: print layout     */
int cmd_clippy(const Cli *cli);  /* cmd_lint.c:    clean-code checks*/
int cmd_fetch(const Cli *cli);   /* cmd_pkg.c:    refresh index     */
int cmd_search(const Cli *cli);  /* cmd_pkg.c:    search index      */
int cmd_install(const Cli *cli); /* cmd_pkg.c:    cache -> installed*/
int cmd_uninstall(const Cli *cli);/* cmd_pkg.c:   drop installed    */
int cmd_update(const Cli *cli);  /* cmd_pkg.c:    refresh installed */
int cmd_add(const Cli *cli);     /* cmd_pkg.c:    edit dependencies */
int cmd_remove(const Cli *cli);  /* cmd_pkg.c:    edit dependencies */
int cmd_list(const Cli *cli);    /* cmd_list.c:   list installed pkgs */
int cmd_publish(const Cli *cli); /* cmd_publish.c: assemble dist    */
int cmd_meta(const Cli *cli);    /* cmd_meta.c:   show metadata     */
int cmd_config(const Cli *cli);  /* cmd_meta.c:   read/write config */
int cmd_doc(const Cli *cli);     /* cmd_meta.c:   regenerate doc.md */
int cmd_state(const Cli *cli);   /* cmd_artifacts.c: frontend arch  */
int cmd_shared(const Cli *cli);  /* cmd_artifacts.c: frontend arch  */

#endif /* PAXSY_COMMANDS_H */
