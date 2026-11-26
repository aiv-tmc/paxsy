#ifndef PAXSY_CLI_H
#define PAXSY_CLI_H

/*
 * cli.h - command-line interface of the paxsy driver.
 *
 * Positional arguments are assigned in order: the first one is the
 * command name, the second the project path (or the project name for
 * new, or the search term for search, ...), the third and fourth
 * are command-specific operands such as a package name and a version.
 * Options may appear anywhere on the command line. Everything after a
 * literal '--' is collected into rest[] and forwarded verbatim to the
 * invoked program by commands such as run.
 */

typedef struct {
    const char *command;   /* first positional: subcommand name */
    const char *path;      /* second positional: path / name / key */
    const char *arg;       /* third positional:  operand (package, ...) */
    const char *value;     /* fourth positional: operand (version, value) */
    const char **rest;     /* argv after '--', passed to the program */
    int rest_count;        /* number of elements in rest */

    /* Effective options loaded from project TOML or set internally. */
    int max_errors;        /* [quality].max_errors (default 20) */
    int dump_ast;          /* internal AST inspection */
    int dump_tokens;       /* internal token inspection */
    int dump_hir;          /* internal HIR inspection */
    int emit_llvm;         /* [artifacts].llvm / [build].target */
    int emit_mlir;         /* [artifacts].mlir / [build].target */
    int dump_offload;      /* internal offload report */
    int emit_cuda;         /* [artifacts].cuda / [build].target */
    int emit_obj;          /* [artifacts].object / [build].target */
    int emit_a;            /* [artifacts].archive / [build].target */
    int emit_pe;           /* [artifacts].pe / [build].target */
    char target[64];       /* internal target override used by debug */
    int offload_sim;       /* internal offload simulation */
    int recursive;         /* [source].recursive */

    /* Quality policy from [quality]. */
    int wextra;
    int werror;
    int ignore_warnings;
    int strict;            /* fmt check mode */
    const char *breaks[16];
    int nbreaks;
    int dbg_run;
    int dbg_run_explicit;
    int dbg_stepi;
    int dbg_regs;
    int dbg_trace_memory;
    int dbg_trace_registers;
    int dbg_trace_all;
    const char *dbg_trace_file;
    int fix;               /* --fix (clippy) */

    /* general */
    int verbose;           /* -v, --verbose */
    int profile;           /* --profile: report compiler phase timings */
    const char *run_profile; /* run --profile NAME: [run.<name>] execution profile */
    int quiet;             /* -q, --quiet */
    int no_git;            /* new: disable repository initialization and automation */
    int help;              /* -h, --help */
    int license;           /* -l, --license */
    int version;           /* --version */
} Cli;

/* Fills defaults then scans argv. Returns 0 on success, -1 on a bad
 * option (a message is printed to stderr). */
int cli_parse(Cli *c, int argc, char **argv);

#endif /* PAXSY_CLI_H */
