#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "lexer.h"
#include "srcdir.h"
#include "color.h"

/*
 * cmd_lint.c - the clean-code rules behind the clippy command. The
 * rules run directly on the token stream and the source text, which
 * covers the layout rules today; semantic rules join once the typed
 * intermediate representation exists. The command driver (file
 * iteration, --fix rewriting, summary) lives in cmd_clippy.c.
 *
 * Rules: file-header, todo, naming (snake_case / UPPER_SNAKE_CASE),
 * magic-numbers and line-length (both --Wextra), match-else,
 * try-catch, alloc-free, asm. Warnings do not fail the run unless
 * --strict promotes them to errors.
 */

/* True for lower snake_case. */
static int snake_ok(const char *s, int n) {
    for (int i = 0; i < n; i++)
        if (!(islower((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '_'))
            return 0;
    return 1;
}

/* True for UPPER_SNAKE_CASE. */
static int upper_ok(const char *s, int n) {
    for (int i = 0; i < n; i++)
        if (!(isupper((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '_'))
            return 0;
    return 1;
}

/* Prints one diagnostic with the rule name attached. */
static void lint_diag(Diag *d, const char *path, const Token *t,
                      DiagKind kind, const char *rule, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    diag_report_full(d, kind, path, t->line, t->col, t->len,
                     "[%s] %s", rule, buf);
}

/* True when the token text equals the literal. */
static int tok_is(const Token *t, const char *s) {
    return (int)strlen(s) == t->len && memcmp(t->start, s, (size_t)t->len) == 0;
}

/* Every file must open with the standard header comment. */
static void check_file_header(Diag *d, const Source *src, const char *path) {
    const char *p = src->text;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, "/*", 2) == 0) return;
    lint_diag(d, path, &(Token){ TK_IDENT, src->text, 0, 1, 1 },
              DK_WARNING, "file-header",
              "file must begin with the standard header comment");
}

/* No TODO/FIXME/XXX/HACK markers in any source line. */
static void check_todo(Diag *d, const Source *src, const char *path) {
    static const char *markers[] = { "TODO", "FIXME", "XXX", "HACK" };
    for (int ln = 1; ln <= src->line_count; ln++) {
        int len;
        const char *line = source_line(src, ln, &len);
        if (!line) continue;
        for (int m = 0; m < 4; m++) {
            const char *hit = strstr(line, markers[m]);
            if (hit)
                lint_diag(d, path, &(Token){ TK_IDENT, line, len, ln, 1 },
                          DK_ERROR, "todo", "marker '%s' is not allowed",
                          markers[m]);
        }
    }
}

/* Variable, function, type and macro names are snake_case; comptime
 * constants are UPPER_SNAKE_CASE. */
static void check_naming(Diag *d, const Token *tv, int count, const char *path) {
    for (int i = 0; i < count; i++) {
        const Token *t = &tv[i];
        const Token *nx = i + 1 < count ? &tv[i + 1] : NULL;
        TokenKind k = t->kind;
        if (!nx || nx->kind != TK_IDENT) continue;
        if (k == TK_KW_COMPTIME && i + 2 < count &&
            tv[i + 2].kind == TK_ASSIGN) {
            if (!upper_ok(nx->start, nx->len))
                lint_diag(d, path, nx, DK_ERROR, "naming",
                          "comptime constant names must be UPPER_SNAKE_CASE");
        } else if ((k == TK_KW_LET || k == TK_KW_FUNC || k == TK_AT ||
                    k == TK_KW_STRUCT || k == TK_KW_UNION || k == TK_KW_ENUM) &&
                   !snake_ok(nx->start, nx->len)) {
            lint_diag(d, path, nx, DK_ERROR, "naming",
                      "names must be lower snake_case");
        }
    }
}

/* Magic numbers are forbidden under --Wextra: literal integers in the
 * 0..64 range that are not 0, 1 or a power of two, outside field
 * access, qualified names and enum variant values. */
static void check_magic_numbers(Diag *d, const Token *tv, int count,
                                const char *path) {
    for (int i = 0; i < count; i++) {
        const Token *t = &tv[i];
        if (t->kind != TK_INT_LIT) continue;
        if (i > 0 && (tv[i - 1].kind == TK_DOT || tv[i - 1].kind == TK_COLONCOLON))
            continue;
        if (i > 0 && tv[i - 1].kind == TK_COLON) continue;
        char buf[32];
        int n = t->len < (int)sizeof(buf) - 1 ? t->len : (int)sizeof(buf) - 1;
        memcpy(buf, t->start, (size_t)n);
        buf[n] = '\0';
        double v = strtod(buf, NULL);
        if (v != 0 && v < 65 && v != 1 && v != 2 && v != 4 &&
            v != 8 && v != 16 && v != 32 && v != 64)
            lint_diag(d, path, t, DK_WARNING, "magic-numbers",
                      "integer literal '%.*s' is a magic number", t->len, t->start);
    }
}

/* A match expression requires an else branch; tracked with a local
 * brace counter that ignores braces inside strings and chars. */
static void check_match_else(Diag *d, const Token *tv, int count,
                             const char *path) {
    for (int i = 0; i < count; i++) {
        TokenKind k = tv[i].kind;
        if (k == TK_STRING_LIT || k == TK_CHAR_LIT) continue;
        if (k != TK_KW_MATCH) continue;
        if (i + 1 < count && tv[i + 1].kind == TK_LBRACE) {
            int depth = 0, found = 0, j;
            for (j = i + 1; j < count; j++) {
                TokenKind m = tv[j].kind;
                if (m == TK_STRING_LIT || m == TK_CHAR_LIT) continue;
                if (m == TK_LBRACE) depth++;
                else if (m == TK_RBRACE && --depth == 0) break;
                else if (m == TK_KW_ELSE && depth == 1) found = 1;
            }
            if (!found)
                lint_diag(d, path, &tv[i], DK_WARNING, "match-else",
                          "match expression requires an 'else' branch");
            i = j;
        }
    }
}

/* Every try must be followed by catch on the same chain. */
static void check_catch(Diag *d, const Token *tv, int count, const char *path) {
    for (int i = 0; i < count; i++) {
        if (tv[i].kind != TK_KW_TRY) continue;
        if (!(i > 0 && tv[i - 1].kind == TK_DOT)) {
            lint_diag(d, path, &tv[i], DK_WARNING, "try-catch",
                      "try must be used in a call chain with catch");
            continue;
        }
        int j = i + 1, depth = 0;
        while (j < count) {
            TokenKind k = tv[j].kind;
            if (k == TK_LPAREN || k == TK_LBRACKET || k == TK_LBRACE) depth++;
            else if (k == TK_RPAREN || k == TK_RBRACKET || k == TK_RBRACE) depth--;
            if (depth < 0 || k == TK_SEMI || (k == TK_DOT && depth == 0)) break;
            if (k == TK_KW_CATCH) break;
            j++;
        }
        if (j >= count || tv[j].kind != TK_KW_CATCH) {
            lint_diag(d, path, &tv[i], DK_WARNING, "try-catch",
                      "try without catch on the same call chain");
            continue;
        }
        /* Skip the catch pattern |err| before continuing. */
        j++;
        if (j < count && tv[j].kind == TK_PIPE) {
            int p = 0;
            for (j++; j < count && tv[j].kind != TK_PIPE && p < 3; j++, p++);
            if (j < count && tv[j].kind == TK_PIPE) j++;
        }
        i = j - 1;
    }
}

/* Every alloc must be freed in the same file (static pairing check). */
static void check_alloc_free(Diag *d, const Token *tv, int count,
                             const char *path) {
    int allocs = 0, frees = 0;
    for (int i = 0; i < count; i++) {
        if (tok_is(&tv[i], "alloc")) allocs++;
        else if (tok_is(&tv[i], "free")) frees++;
    }
    if (allocs > frees)
        lint_diag(d, path, &tv[0], DK_WARNING, "alloc-free",
                  "%d alloc(s) without a matching free in the same file",
                  allocs - frees);
}

/* No inline assembly. */
static void check_asm(Diag *d, const Token *tv, int count, const char *path) {
    for (int i = 0; i < count; i++)
        if (tv[i].kind == TK_KW_ASM)
            lint_diag(d, path, &tv[i], DK_ERROR, "asm",
                      "inline assembly is not allowed");
}

/* 80-column line limit (Wextra). */
static void check_line_length(Diag *d, const Source *src, const char *path) {
    for (int i = 1; i <= src->line_count; i++) {
        int len;
        source_line(src, i, &len);
        if (len > 80) {
            Token t = { TK_IDENT, "", 0, i, 81 };
            lint_diag(d, path, &t, DK_WARNING, "line-length",
                      "line exceeds 80 columns (%d)", len);
        }
    }
}

/* Runs every rule over one file; invoked by the clippy driver in
 * cmd_clippy.c after the file is loaded and lexed. */
void lint_tokens(Diag *d, const Token *tv, int count,
                 const Source *src, const char *path,
                 const Cli *cli) {
    check_file_header(d, src, path);
    check_todo(d, src, path);
    check_naming(d, tv, count, path);
    check_match_else(d, tv, count, path);
    check_catch(d, tv, count, path);
    check_alloc_free(d, tv, count, path);
    check_asm(d, tv, count, path);
    if (cli->wextra) {
        check_magic_numbers(d, tv, count, path);
        check_line_length(d, src, path);
    }
}
