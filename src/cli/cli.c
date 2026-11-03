#include "common.h"
#include "cli.h"

/*
 * cli.c - single-pass option scanner.
 * Recognised switches are matched exactly; every other dash-prefixed
 * argument is rejected with a pointer to --help. A literal '--' ends
 * option scanning: all remaining arguments are stored in Cli.rest and
 * forwarded verbatim to the invoked program.
 */

static int set_positional(Cli *c, const char *v) {
    if (!c->command) c->command = v;
    else if (!c->path) c->path = v;
    else if (!c->arg) c->arg = v;
    else if (!c->value) c->value = v;
    else {
        fprintf(stderr, "paxsy: too many arguments near '%s'\n", v);
        return -1;
    }
    return 0;
}

int cli_parse(Cli *c, int argc, char **argv) {
    memset(c, 0, sizeof(*c));
    c->max_errors = 20;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') {
            if (set_positional(c, a) != 0) return -1;
        } else if (strcmp(a, "--") == 0) {
            /* Everything after '--' belongs to the invoked program and
             * is forwarded verbatim by commands such as run. */
            c->rest = (const char **)&argv[i + 1];
            c->rest_count = argc - i - 1;
            break;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            c->help = 1;
        } else if (strcmp(a, "-l") == 0 || strcmp(a, "--license") == 0) {
            c->license = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            c->version = 1;
        } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
            c->quiet = 1;
        } else if (strcmp(a, "--verbose") == 0) {
            c->verbose = 1;
        } else if (strcmp(a, "--ast") == 0) {
            c->dump_ast = 1;
        } else if (strcmp(a, "--tokens") == 0) {
            c->dump_tokens = 1;
        } else if (strcmp(a, "--hir") == 0) {
            c->dump_hir = 1;
        } else if (strcmp(a, "--emit-llvm") == 0) {
            c->emit_llvm = 1;
        } else if (strcmp(a, "--emit-mlir") == 0) {
            c->emit_mlir = 1;
        } else if (strcmp(a, "--dump-offload") == 0) {
            c->dump_offload = 1;
        } else if (strcmp(a, "--emit-cuda") == 0) {
            c->emit_cuda = 1;
        } else if (strcmp(a, "--emit-obj") == 0) {
            c->emit_obj = 1;
        } else if (strcmp(a, "--emit-a") == 0) {
            c->emit_a = 1;
        } else if (strcmp(a, "--emit-exe") == 0) {
            c->emit_pe = 1;
        } else if (strcmp(a, "--offload=simulate") == 0) {
            c->offload_sim = 1;
        } else if (strcmp(a, "--recursive") == 0) {
            c->recursive = 1;
        } else if (strcmp(a, "--Wextra") == 0) {
            c->wextra = 1;
        } else if (strcmp(a, "--Werror") == 0) {
            c->werror = 1;
        } else if (strcmp(a, "--ignore-warnings") == 0) {
            c->ignore_warnings = 1;
        } else if (strcmp(a, "--strict") == 0) {
            c->strict = 1;
        } else if (strcmp(a, "--fix") == 0) {
            c->fix = 1;
        } else if (strncmp(a, "--max-errors=", 13) == 0) {
            int n = atoi(a + 13);
            if (n <= 0) {
                fprintf(stderr, "paxsy: bad --max-errors value\n");
                return -1;
            }
            c->max_errors = n;
        } else {
            fprintf(stderr, "paxsy: unknown option '%s' "
                            "(see 'paxsy --help')\n", a);
            return -1;
        }
    }
    return 0;
}
