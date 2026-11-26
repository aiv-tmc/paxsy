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
        fprintf(stderr, "too many arguments near '%s'\n", v);
        return -1;
    }
    return 0;
}

int cli_parse(Cli *c, int argc, char **argv) {
    memset(c, 0, sizeof(*c));
    c->max_errors = 20;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            c->rest = (const char **)&argv[i + 1];
            c->rest_count = argc - i - 1;
            break;
        }
        if (a[0] != '-' || a[1] == '\0') {
            if (set_positional(c, a) != 0) return -1;
            continue;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)
            c->help = 1;
        else if (strcmp(a, "-l") == 0 || strcmp(a, "--license") == 0)
            c->license = 1;
        else if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0)
            c->version = 1;
        else if (strcmp(a, "--verbose") == 0)
            c->verbose = 1;
        else if (strcmp(a, "--profile") == 0) {
            /* The spelling is intentionally shared by two commands:
             * `build --profile` enables compiler timings, while
             * `run --profile NAME` selects a runtime environment. */
            if (c->command && strcmp(c->command, "run") == 0) {
                if (i + 1 >= argc || argv[i + 1][0] == '-') {
                    fprintf(stderr, "run --profile requires a profile name\n");
                    return -1;
                }
                c->run_profile = argv[++i];
            } else {
                c->profile = 1;
            }
        }
        else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0)
            c->quiet = 1;
        else if (strcmp(a, "--no-git") == 0)
            c->no_git = 1;
        else if (strcmp(a, "--fix") == 0)
            c->fix = 1;
        else if (strcmp(a, "--check") == 0)
            c->strict = 1; /* formatter check mode */
        else {
            fprintf(stderr,
                    "unknown option '%s' (run 'paxsy --help' for basic usage)\n",
                    a);
            return -1;
        }
    }
    if (c->fix && c->command && strcmp(c->command, "clippy") != 0) {
        fprintf(stderr, "--fix is only valid with 'clippy'\n");
        return -1;
    }
    if (c->strict && c->command && strcmp(c->command, "fmt") != 0) {
        fprintf(stderr, "--check is only valid with 'fmt'\n");
        return -1;
    }
    if (c->no_git && c->command && strcmp(c->command, "new") != 0) {
        fprintf(stderr, "--no-git is only valid with 'new'\n");
        return -1;
    }
    return 0;
}
