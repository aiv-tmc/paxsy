#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "source.h"
#include "lexer.h"
#include <sys/stat.h>
#include <dirent.h>

/*
 * cmd_lint.c - the clippy command: checks every source file against
 * the clean-code rules. Structural rules (line length, tabs, header,
 * match-else, alloc/free pairing, naming) always run; the heuristic
 * rules (magic numbers, silent catch) are conservative and only run
 * with --Wextra. --fix repairs whitespace damage in place; --strict /
 * --Werror promote warnings to exit failures. Diagnostics use the
 * source path directly because the diagnostics list may still be
 * empty when a check runs.
 */

typedef struct { int errors, warnings, fixed; } LintStats;

static int name_is_snake(const char *s) {
    if (!s[0]) return 0;
    for (const char *p = s; *p; p++)
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_')) return 0;
    return 1;
}
static int name_is_pascal(const char *s) {
    if (!s[0] || !isupper((unsigned char)s[0])) return 0;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p)) return 0;
    return 1;
}
static int name_is_screaming(const char *s) {
    if (!s[0]) return 0;
    for (const char *p = s; *p; p++)
        if (!(isupper((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_')) return 0;
    return 1;
}
static void slice(char *dst, int n, const Token *t) {
    snprintf(dst, (size_t)n, "%.*s", t->len, t->start);
}
static int line_has(const Source *src, int line, const char *needle) {
    int len;
    const char *text = source_line(src, line, &len);
    return text && strstr(text, needle) != NULL;
}

/* Raw text rules: 80-column limit and the tab prohibition. */
static void check_lines(const Source *src, Diag *d) {
    for (int i = 1; i <= src->line_count; i++) {
        int len;
        const char *text = source_line(src, i, &len);
        if (len > 80)
            diag_report(d, DK_WARNING, src->path, i, 81,
                        "line exceeds 80 characters (line-length)");
        for (int j = 0; j < len; j++)
            if (text[j] == '\t') {
                diag_report(d, DK_ERROR, src->path, i, j + 1,
                            "tab character; use four spaces (tabs)");
                break;
            }
    }
}

/* The file must open with the license header block comment. */
static void check_header(const Source *src, Diag *d) {
    const char *p = src->text;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, "/*", 2) != 0) {
        diag_report(d, DK_WARNING, src->path, 1, 1,
                    "missing license header comment (file-header)");
        return;
    }
    char head[640];
    snprintf(head, sizeof(head), "%.*s", 600, p);
    if (!strstr(head, "Copyright") || !strstr(head, "License"))
        diag_report(d, DK_WARNING, src->path, 1, 1,
                    "header lacks Copyright/License lines (file-header)");
}

/* Every match body must contain an else at body level. */
static void check_match_else(const TokenVec *tv, int i, Diag *d,
                             const char *path) {
    int depth = 0, found = 0;
    for (int j = i + 1; j < tv->count; j++) {
        TokenKind k = tv->items[j].kind;
        if (k == TK_LBRACE) depth++;
        else if (k == TK_RBRACE && --depth == 0) break;
        else if (k == TK_KW_ELSE && depth == 1) found = 1;
    }
    if (!found)
        diag_report(d, DK_WARNING, path, tv->items[i].line, tv->items[i].col,
                    "match without an 'else' branch (match-else)");
}

/* A catch block should log or propagate instead of swallowing. */
static void check_catch(const TokenVec *tv, int i, Diag *d,
                        const char *path) {
    int j = i + 1;
    if (tv->items[j].kind == TK_PIPE) j += 3;      /* skip |e| */
    if (j >= tv->count || tv->items[j].kind != TK_LBRACE) return;
    int depth = 0, informative = 0;
    for (; j < tv->count; j++) {
        const Token *t = &tv->items[j];
        if (t->kind == TK_LBRACE) depth++;
        else if (t->kind == TK_RBRACE && --depth == 0) break;
        else if (t->kind == TK_KW_RETURN || t->kind == TK_KW_DEFER) informative = 1;
        else if (t->kind == TK_IDENT) {
            char w[32];
            slice(w, sizeof(w), t);
            if (strcmp(w, "println") == 0 || strcmp(w, "print") == 0 ||
                strcmp(w, "log") == 0 || strcmp(w, "fatal") == 0) informative = 1;
        }
    }
    if (!informative)
        diag_report(d, DK_WARNING, path, tv->items[i].line, tv->items[i].col,
                    "catch block neither logs nor returns (silent-catch)");
}

/* Collects a possibly qualified name starting at token i. */
static void read_name(const TokenVec *tv, int i, char *out, int n) {
    out[0] = '\0';
    int pos = 0;
    while (i < tv->count) {
        TokenKind k = tv->items[i].kind;
        if (k != TK_IDENT && k != TK_COLONCOLON && k != TK_DOT) break;
        pos += snprintf(out + pos, (size_t)(n - pos), "%.*s",
                        tv->items[i].len, tv->items[i].start);
        i++;
    }
}

static void check_func_name(const TokenVec *tv, int i, Diag *d,
                            const char *path) {
    char name[128];
    read_name(tv, i, name, sizeof(name));
    const char *last = strrchr(name, '.');
    last = last ? last + 1 : name;
    if (name[0] && !name_is_snake(last))
        diag_report(d, DK_WARNING, path, tv->items[i].line, tv->items[i].col,
                    "function name '%s' should be snake_case (naming)", last);
}

/* comptime names: PascalCase for types, SCREAMING_SNAKE_CASE for
 * constants; the kind is decided from the token after '='. */
static void check_comptime_name(const TokenVec *tv, int i, Diag *d,
                                const char *path) {
    char nm[128];
    slice(nm, sizeof(nm), &tv->items[i + 1]);
    int j = i + 2;
    while (j < tv->count && tv->items[j].kind != TK_ASSIGN &&
           tv->items[j].kind != TK_SEMI) j++;
    int is_type = 0;
    if (j < tv->count && tv->items[j].kind == TK_ASSIGN && j + 1 < tv->count) {
        TokenKind rhs = tv->items[j + 1].kind;
        if (rhs == TK_KW_STRUCT || rhs == TK_KW_UNION || rhs == TK_KW_ENUM)
            is_type = 1;
        else if (rhs == TK_IDENT && isupper((unsigned char)tv->items[j + 1].start[0]))
            is_type = 1;
    }
    int ok = is_type ? name_is_pascal(nm) : name_is_screaming(nm);
    if (!ok)
        diag_report(d, DK_WARNING, path,
                    tv->items[i + 1].line, tv->items[i + 1].col,
                    "comptime name '%s' should be %s (naming)", nm,
                    is_type ? "PascalCase" : "SCREAMING_SNAKE_CASE");
}

static void token_checks(const Source *src, const TokenVec *tv, Diag *d,
                         int wextra) {
    int brace = 0;          /* brace nesting level */
    int enum_mark = -1;     /* nesting level of the innermost Enum body */
    int allocs = 0, frees = 0;
    for (int i = 0; i < tv->count; i++) {
        const Token *t = &tv->items[i];
        if (t->kind == TK_LBRACE) { brace++; continue; }
        if (t->kind == TK_RBRACE) {
            if (enum_mark >= 0 && brace == enum_mark) enum_mark = -1;
            brace--;
            continue;
        }
        if (t->kind == TK_KW_ENUM) { enum_mark = brace + 1; continue; }
        if (t->kind == TK_KW_ALLOC) { allocs++; continue; }
        if (t->kind == TK_KW_FREE)  { frees++; continue; }
        if (wextra && (t->kind == TK_INT_LIT || t->kind == TK_FLOAT_LIT)) {
            char buf[64];
            slice(buf, sizeof(buf), t);
            double v = strtod(buf, NULL);
            if (v != 0.0 && v != 1.0 && enum_mark < 0 &&
                !line_has(src, t->line, "comptime"))
                diag_report(d, DK_WARNING, src->path, t->line, t->col,
                            "magic number %s (magic-numbers)", buf);
        }
        if (t->kind == TK_KW_MATCH) check_match_else(tv, i, d, src->path);
        if (wextra && t->kind == TK_KW_CATCH)
            check_catch(tv, i, d, src->path);
        if (t->kind == TK_KW_FUNC && i + 1 < tv->count &&
            tv->items[i + 1].kind == TK_IDENT)
            check_func_name(tv, i + 1, d, src->path);
        if (t->kind == TK_KW_LET && i + 1 < tv->count &&
            tv->items[i + 1].kind == TK_IDENT) {
            char nm[128];
            slice(nm, sizeof(nm), &tv->items[i + 1]);
            if (!name_is_snake(nm))
                diag_report(d, DK_WARNING, src->path, t->line, t->col,
                            "variable name '%s' should be snake_case (naming)", nm);
        }
        if (t->kind == TK_KW_COMPTIME && i + 1 < tv->count &&
            tv->items[i + 1].kind == TK_IDENT)
            check_comptime_name(tv, i, d, src->path);
    }
    /* Heuristic only: idiomatic allocation via std::array::create is
     * invisible to the raw token count, so both directions of the
     * mismatch are warnings, never errors. */
    if (frees != allocs)
        diag_report(d, DK_WARNING, src->path, 1, 1,
                    "alloc (%d) and free (%d) counts differ (alloc-free)",
                    allocs, frees);
}

/* Rewrites the file: tabs become four spaces, trailing whitespace is
 * stripped. Returns 1 when the file changed. */
static int fix_whitespace(const Source *src) {
    char *out = malloc(src->len * 4 + 1);
    size_t pos = 0, last_word = 0;
    int changed = 0;
    for (size_t i = 0; i < src->len; i++) {
        char c = src->text[i];
        if (c == '\t') {
            memcpy(out + pos, "    ", 4);
            pos += 4;
            last_word = pos;
            changed = 1;
            continue;
        }
        if (c == '\n') {
            pos = last_word;
            out[pos++] = '\n';
            continue;
        }
        out[pos++] = c;
        if (c != ' ' && c != '\r') last_word = pos;
    }
    pos = last_word;
    out[pos] = '\0';
    if (!changed && pos == src->len && memcmp(out, src->text, pos) == 0) {
        free(out);
        return 0;
    }
    FILE *f = fopen(src->path, "w");
    if (!f) { free(out); return 0; }
    fwrite(out, 1, pos, f);
    fclose(f);
    free(out);
    return 1;
}

static void lint_file(const char *path, const Cli *cli, LintStats *st) {
    Source src;
    if (source_load(&src, path) != 0) {
        fprintf(stderr, "paxsy: cannot open '%s'\n", path);
        st->errors++;
        return;
    }
    if (cli->fix && fix_whitespace(&src)) {
        printf("fixed %s\n", path);
        st->fixed++;
        source_free(&src);
        if (source_load(&src, path) != 0) { st->errors++; return; }
    }
    Diag d;
    diag_init(&d, 1000000);
    d.suppress_warnings = cli->ignore_warnings;
    check_lines(&src, &d);
    check_header(&src, &d);
    TokenVec tv;
    lexer_run(&src, &tv, &d);
    token_checks(&src, &tv, &d, cli->wextra);
    diag_print_all(&d, &src);
    st->errors += diag_error_count(&d);
    st->warnings += diag_warning_count(&d);
    token_vec_free(&tv);
    diag_free(&d);
    source_free(&src);
}

static void collect(const char *dir, int recursive, char files[][PAXSY_PATH_MAX],
                    int *count) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *count < 64) {
        if (e->d_name[0] == '.') continue;
        char path[PAXSY_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (recursive) collect(path, recursive, files, count); continue; }
        size_t len = strlen(e->d_name);
        if (len > 3 && strcmp(e->d_name + len - 3, ".px") == 0) {
            snprintf(files[*count], PAXSY_PATH_MAX, "%s", path);
            (*count)++;
        }
    }
    closedir(d);
}

int cmd_clippy(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    char files[64][PAXSY_PATH_MAX];
    int count = 0;
    char srcdir[PAXSY_PATH_MAX];
    snprintf(srcdir, sizeof(srcdir), "%s/src", root);
    collect(srcdir, cli->recursive, files, &count);
    if (count == 0) {
        fprintf(stderr, "paxsy: no .px sources under '%s/src'\n", root);
        return 1;
    }
    LintStats st = { 0, 0, 0 };
    for (int i = 0; i < count; i++) lint_file(files[i], cli, &st);
    int failures = st.errors;
    if (cli->strict || cli->werror) failures += st.warnings;
    if (!cli->quiet)
        printf("clippy: %d error(s), %d warning(s)%s\n",
               st.errors, st.warnings, st.fixed ? ", files fixed" : "");
    return failures ? 1 : 0;
}
