#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "lexer.h"
#include "srcdir.h"
#include "color.h"
#include "toml.h"
#include <sys/stat.h>

/*
 * cmd_clippy.c - the clippy command driver. It walks every source of
 * the project, lexes it, runs the clean-code rules from cmd_lint.c and
 * reports the findings; with --fix it additionally rewrites whitespace
 * violations (tabs, trailing spaces) in place. Warning policy comes
 * from [quality] in the project TOML.
 */

/* Implemented in cmd_lint.c; declared here so the driver can invoke
 * the rule set after the file is loaded and lexed. */
void lint_tokens(Diag *d, const Token *tv, int count,
                 const Source *src, const char *path,
                 const Cli *cli);

/* Strips the directory part from a path for display. */
static const char *lint_base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Rewrites one file in place: tabs become four spaces, trailing
 * whitespace is removed. Returns 1 when the file changed. */
static int fix_whitespace(const char *path, const Source *src) {
    size_t cap = src->len * 4 + 1;
    char *out = malloc(cap);
    if (!out) return 0;
    size_t pos = 0, last_word = 0;
    int changed = 0;
    for (size_t i = 0; i < src->len; i++) {
        char c = src->text[i];
        if (c == '\t') {
            out[pos++] = ' '; out[pos++] = ' ';
            out[pos++] = ' '; out[pos++] = ' ';
            changed = 1;
        } else if (c == ' ') {
            out[pos++] = c;
        } else if (c == '\n') {
            pos = last_word;
            out[pos++] = '\n';
        } else {
            out[pos++] = c;
            last_word = pos;
        }
    }
    if (pos > 0 && out[pos - 1] != '\n') out[pos++] = '\n';
    if (!changed && pos == src->len && memcmp(out, src->text, pos) == 0) {
        free(out);
        return 0;
    }
    FILE *f = fopen(path, "w");
    if (!f) { free(out); return 0; }
    fwrite(out, 1, pos, f);
    fclose(f);
    free(out);
    return 1;
}

int cmd_clippy(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    Cli options = *cli;
    char cfg[PAXSY_PATH_MAX], value[32];
    if (toml_find(root, cfg, sizeof(cfg)) == 0) {
        if (toml_get(cfg, "source", "recursive", value, sizeof(value)))
            options.recursive = strcmp(value, "true") == 0;
        else if (toml_get(cfg, "build", "recursive", value, sizeof(value)))
            options.recursive = strcmp(value, "true") == 0; /* legacy */
        if (toml_get(cfg, "quality", "max_errors", value, sizeof(value))) {
            char *end = NULL;
            long n = strtol(value, &end, 10);
            if (end == value || !end || *end || n < 1 || n > 10000) {
                fprintf(stderr, "invalid [quality].max_errors (expected 1..10000)\n");
                return 2;
            }
            options.max_errors = (int)n;
        }
        if (toml_get(cfg, "quality", "extra_warnings", value, sizeof(value)))
            options.wextra = strcmp(value, "true") == 0;
        if (toml_get(cfg, "quality", "warnings_as_errors", value, sizeof(value)))
            options.werror = strcmp(value, "true") == 0;
        if (toml_get(cfg, "quality", "ignore_warnings", value, sizeof(value)))
            options.ignore_warnings = strcmp(value, "true") == 0;
    }
    /* Source discovery grows a zero-initialised list as paths arrive. */
    SrcList srcs = {0};
    if (srcdir_collect(root, options.recursive, 0, &srcs) < 0) {
        fprintf(stderr, "source discovery failed under '%s'\n", root);
        return 1;
    }
    if (srcs.count == 0) {
        struct stat fst;
        int reg = stat(root, &fst) == 0 && S_ISREG(fst.st_mode);
        if (reg && strlen(root) > 3 &&
            strcmp(root + strlen(root) - 3, ".px") == 0) {
            /* A single source file can be linted directly. */
            if (srclist_push(&srcs, root) != 0) {
                fprintf(stderr, "clippy: cannot store source path\n");
                srclist_free(&srcs);
                return 1;
            }
        }
    }
    if (srcs.count == 0) {
        srclist_free(&srcs);
        fprintf(stderr, "no .px sources under '%s/src'\n", root);
        return 1;
    }
    int files_with_issues = 0, files_with_errors = 0, fixed = 0;
    for (int i = 0; i < srcs.count; i++) {
        const char *path = srcs.paths[i];
        Source src;
        if (source_load(&src, path) != 0) continue;
        Diag d;
        diag_init(&d, options.max_errors);
        d.suppress_warnings = options.ignore_warnings;
        TokenVec tv;
        lexer_run(&src, &tv, &d);

        lint_tokens(&d, tv.items, tv.count, &src, path, &options);
        int warnings = diag_warning_count(&d);
        int errors = diag_error_count(&d);
        diag_print_all(&d, &src);
        if (options.fix && fix_whitespace(path, &src)) {
            printf("%srewrite%s %s\n", c_green, c_reset, lint_base(path));
            fixed++;
        }
        if (options.werror && warnings > 0) {
            errors += warnings;
            warnings = 0;
        }
        if (errors > 0) files_with_errors++;
        if (errors > 0 || warnings > 0) files_with_issues++;
        diag_free(&d);
        token_vec_free(&tv);
        source_free(&src);
    }
    srclist_free(&srcs);
    if (files_with_issues == 0) {
        printf("%sall clean%s\n", c_green, c_reset);
    } else {
        printf("%s%d file(s) with issues%s%s\n",
               c_yellow, files_with_issues, c_reset,
               fixed ? " (whitespace fixed)" : "");
    }
    /* Only errors fail the run; warnings_as_errors promotes warnings. */
    return files_with_errors ? 1 : 0;
}
