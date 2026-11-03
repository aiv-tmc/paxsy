#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "lexer.h"
#include "srcdir.h"
#include "color.h"

/*
 * cmd_clippy.c - the clippy command driver. It walks every source of
 * the project, lexes it, runs the clean-code rules from cmd_lint.c and
 * reports the findings; with --fix it additionally rewrites whitespace
 * violations (tabs, trailing spaces) in place. clippy is verbose by
 * design: every finding is shown, warnings only fail the run after
 * --strict promotes them to errors.
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
    /* Zero-initialised: srclist_push grows the array from count/cap,
     * so uninitialised storage would send realloc() to a wild
     * pointer (the release-build crash fixed together with the
     * normalisation guard inside srcdir_collect). */
    SrcList srcs = {0};
    if (srcdir_collect(root, cli->recursive, 0, &srcs) < 0) {
        fprintf(stderr, "paxsy: source discovery failed under '%s'\n", root);
        return 1;
    }
    if (srcs.count == 0) {
        srclist_free(&srcs);
        fprintf(stderr, "paxsy: no .px sources under '%s/src'\n", root);
        return 1;
    }
    int files_with_issues = 0, files_with_errors = 0, fixed = 0;
    for (int i = 0; i < srcs.count; i++) {
        const char *path = srcs.paths[i];
        Source src;
        if (source_load(&src, path) != 0) continue;
        Diag d;
        diag_init(&d, 1000000);
        TokenVec tv;
        lexer_run(&src, &tv, &d);

        lint_tokens(&d, tv.items, tv.count, &src, path, cli);
        int warnings = diag_warning_count(&d);
        int errors = diag_error_count(&d);
        if (cli->strict && warnings > 0) {
            errors += warnings;
            warnings = 0;
        }
        diag_print_all(&d, &src);
        if (cli->fix && fix_whitespace(path, &src)) {
            printf("%srewrite%s %s\n", c_green, c_reset, lint_base(path));
            fixed++;
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
    /* Only errors fail the run; --strict promotes warnings first. */
    return files_with_errors ? 1 : 0;
}
