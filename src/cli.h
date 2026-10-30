#ifndef PAXSY_CLI_H
#define PAXSY_CLI_H

/*
 * cli.h - command-line interface of the paxsy driver.
 *
 * Positional arguments are assigned in order: the first one is the
 * command name, the second the project path (or the project name for
 * init/new, or the search term for search, ...), the third and fourth
 * are command-specific operands such as a package name and a version.
 * Options may appear anywhere on the command line.
 */

typedef struct {
    const char *command;   /* first positional: subcommand name */
    const char *path;      /* second positional: path / name / key */
    const char *arg;       /* third positional:  operand (package, ...) */
    const char *value;     /* fourth positional: operand (version, value) */

    /* frontend behaviour */
    int max_errors;        /* --max-errors=N (default 20) */
    int dump_ast;          /* --ast  */
    int dump_tokens;       /* --tokens */
    int recursive;         /* --recursive */

    /* quality gates */
    int wextra;            /* --Wextra  */
    int werror;            /* --Werror  */
    int ignore_warnings;   /* --ignore-warnings */
    int strict;            /* --strict (clippy) */
    int fix;               /* --fix (clippy) */

    /* general */
    int verbose;           /* --verbose */
    int quiet;             /* -q, --quiet */
    int help;              /* -h, --help */
    int license;           /* -l, --license */
    int version;           /* -v, --version */
} Cli;

/* Fills defaults then scans argv. Returns 0 on success, -1 on a bad
 * option (a message is printed to stderr). */
int cli_parse(Cli *c, int argc, char **argv);

#endif /* PAXSY_CLI_H */
