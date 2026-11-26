/*
 * cmd_fmt.c - 'paxsy fmt': the mechanical source formatter.
 *
 * Layout rules follow the toolchain manual: 4-space indentation, K&R
 * braces, single spaces around binary operators, no spaces around
 * unary operators and member access, at most one blank line, a single
 * trailing newline, and sorted groups of consecutive top-level 'use'
 * statements. Long lines that only a string literal would overflow
 * are left alone - strings are never broken.
 *
 * The formatter is token-based: comments and literals pass through
 * verbatim, everything else is re-laid out from the token stream.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

char *sm_strdup(const char *s);  /* util/arena: owned string copy */

#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "lexer.h"
#include "srcdir.h"
#include "toml.h"

/* ---- output buffer ------------------------------------------------ */

typedef struct {
    char *p;
    size_t len, cap;
    int failed;
} Buf;

static void bput(Buf *b, const char *s, size_t n) {
    if (b->failed || n == 0) return;
    if (!s || n > SIZE_MAX - b->len) { b->failed = 1; return; }
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        char *np;
        while (nc < b->len + n) {
            if (nc > SIZE_MAX / 2) { b->failed = 1; return; }
            nc *= 2;
        }
        np = realloc(b->p, nc);
        if (!np) { b->failed = 1; return; }
        b->p = np;
        b->cap = nc;
    }
    if (!b->p) { b->failed = 1; return; }
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void bch(Buf *b, char c) { bput(b, &c, 1); }

static void bstr(Buf *b, const char *s) {
    if (!s) { b->failed = 1; return; }
    bput(b, s, strlen(s));
}

static void bindent(Buf *b, int depth) {
    int i;
    for (i = 0; i < depth * 4; i++) bch(b, ' ');
}

/* ---- spacing decision ---------------------------------------------- */

static int is_operand_end(TokenKind k) {
    switch (k) {
    case TK_IDENT: case TK_INT_LIT: case TK_FLOAT_LIT:
    case TK_STRING_LIT: case TK_CHAR_LIT:
    case TK_RPAREN: case TK_RBRACKET: case TK_RBRACE:
        return 1;
    default:
        return 0;
    }
}


/* Space between the previous and the current token, following the
 * manual: nothing before closers, separators and openers attach to
 * their operand, unary operators hug their operand. */
static int fspace(TokenKind pk, int last_pipe_close, int pipe_open, TokenKind ck) {
    /* never a space after an opener or before a closer/separator */
    if (pk == TK_LPAREN || pk == TK_LBRACKET || pk == TK_DOT ||
        pk == TK_DOTDOT || pk == TK_COLONCOLON || pk == TK_AT)
        return 0;
    if (ck == TK_RPAREN || ck == TK_RBRACKET || ck == TK_SEMI ||
        ck == TK_COMMA || ck == TK_DOT || ck == TK_DOTDOT ||
        ck == TK_COLONCOLON || ck == TK_LPAREN || ck == TK_LBRACKET ||
        ck == TK_COLON)
        return 0;
    if (pk == TK_COMMA || pk == TK_COLON)
        return 1;
    /* The error marker '!' attaches to its type (Int!), and the
     * loop/catch pipes hug their binder. */
    if (pk == TK_BANG || ck == TK_BANG)
        return 0;
    /* '|' only appears as the loop-variable/catch binder pipes
     * (bitwise or is '$'). The scanner tracks which pipe was last:
     * the opening pipe hugs its binder, the closing pipe is
     * followed by a space. */
    if (pk == TK_PIPE)
        return last_pipe_close;   /* space only after the closing one */
    if (ck == TK_PIPE)
        return pipe_open ? 0 : 1; /* the opening pipe breathes */

    /* unary operators hug the operand that follows them */
    if ((ck == TK_MINUS || ck == TK_PLUS || ck == TK_BANG ||
         ck == TK_TILDE || ck == TK_STAR || ck == TK_AMP) &&
        !is_operand_end(pk))
        return 0;
    if (pk == TK_AT)
        return 0;

    /* everything else breathes */
    return 1;
}

/* ---- layout --------------------------------------------------------- */

typedef struct {
    Buf out;
    int depth;
    int line_has;    /* non-indent content on the current line */
    int prev_line;   /* source line of the previous token */
    TokenKind prev;
    int prev_len;
    int pipe_open;        /* binder pipe state machine   */
    int last_pipe_close;
    const char *prev_start;
    int prev_unary;  /* previous token was a unary operator */
    int pending_blank;
    int blk[64];     /* brace stack: 1 block, 0 literal */
    int nblk;
} Lay;

static void lnewline(Lay *L) {
    bch(&L->out, '\n');
    if (L->pending_blank) {
        bch(&L->out, '\n');
        L->pending_blank = 0;
    }
    L->line_has = 0;
}

/* Begin a token on a fresh line if the current one is full. */
static void lbegin(Lay *L) {
    if (!L->line_has) {
        bindent(&L->out, L->depth);
        L->line_has = 1;
    }
}

/* A buffered top-level 'use' statement, kept for group sorting. */
typedef struct { char *text; } UseLine;

static int use_cmp(const void *a, const void *b) {
    const UseLine *x = a, *y = b;
    return strcmp(x->text, y->text);
}

/* Counts comment markers for line and block comments; they leave no tokens
 * in the stream, so the token check alone cannot notice their loss. */
static long ccount(const char *s, long n) {
    long c = 0, i;
    for (i = 0; i + 1 < n; i++)
        if (s[i] == '/' && (s[i + 1] == '/' || s[i + 1] == '*'))
            c++;
    return c;
}

static int format_source(const Source *src, Buf *out) {
    Diag d;
    TokenVec tv;
    size_t i;
    Lay L;
    UseLine *uses = NULL;
    int nuses = 0, capuses = 0;
    int uses_emitted = 0;
    /* ranges of top-level use statements (token index spans) */
    int *use_end = NULL;
    size_t k;

    diag_init(&d, 256);
    lexer_run(src, &tv, &d);
    if (diag_error_count(&d) > 0) {
        diag_print_all(&d, src);
        token_vec_free(&tv);
        diag_free(&d);
        return -1;
    }

    memset(&L, 0, sizeof(L));
    L.prev_line = 1;

    /* pass 1: find top-level use statements and buffer their text */
    use_end = calloc(tv.count + 1, sizeof(int));
    if (!use_end) {
        token_vec_free(&tv);
        diag_free(&d);
        return -1;
    }
    {
        int depth = 0;
        size_t t;
        for (t = 0; t < (size_t)tv.count; t++) {
            Token *tok = &tv.items[t];
            if (tok->kind == TK_LBRACE) depth++;
            else if (tok->kind == TK_RBRACE) depth--;
            if (tok->kind == TK_KW_USE && depth == 0) {
                Buf one = {0};
                int guard = 0;
                size_t e = t;
                while (e < (size_t)tv.count && tv.items[e].kind != TK_SEMI &&
                       guard++ < 1000) {
                    Token *u = &tv.items[e];
                    bput(&one, u->start, (size_t)u->len);
                    if (e + 1 < (size_t)tv.count &&
                        tv.items[e + 1].kind != TK_SEMI &&
                        tv.items[e + 1].kind != TK_DOT &&
                        tv.items[e + 1].kind != TK_COLONCOLON &&
                        u->kind != TK_DOT && u->kind != TK_COLONCOLON &&
                        u->kind != TK_AT)
                        bch(&one, ' ');
                    e++;
                }
                if (e < (size_t)tv.count && tv.items[e].kind == TK_SEMI)
                    bch(&one, ';');
                bch(&one, '\0');
                if (one.failed || !one.p) {
                    free(one.p);
                    L.out.failed = 1;
                    goto format_cleanup;
                }
                if (nuses >= capuses) {
                    int next_cap = capuses ? capuses * 2 : 8;
                    UseLine *next = realloc(uses,
                        (size_t)next_cap * sizeof(UseLine));
                    if (!next) {
                        free(one.p);
                        L.out.failed = 1;
                        goto format_cleanup;
                    }
                    uses = next;
                    capuses = next_cap;
                }
                uses[nuses++].text = one.p;
                use_end[t] = (int)e + 1;
                t = e;
            }
        }
    }

    /* pass 2: layout; skip use ranges, emit the sorted group before the
     * first other top-level token */
    {
        for (i = 0; i < (size_t)tv.count; i++) {
            Token *t = &tv.items[i];
            if (t->kind == TK_EOF) break;

            if (use_end && use_end[i]) {
                /* token i starts a use statement: skip its whole span */
                size_t e = (size_t)use_end[i];
                L.prev_line = tv.items[e - 1].line;
                L.prev = TK_SEMI;
                if (L.line_has) lnewline(&L);
                i = e - 1;
                continue;
            }

            /* flush sorted use group before the first real statement */
            if (!uses_emitted && nuses > 0 &&
                t->kind != TK_EOF) {
                int q;
                qsort(uses, (size_t)nuses, sizeof(UseLine), use_cmp);
                for (q = 0; q < nuses; q++) {
                    lbegin(&L);
                    bstr(&L.out, uses[q].text);
                    lnewline(&L);
                }
                uses_emitted = 1;
            }

            if (L.line_has && t->line - L.prev_line >= 2)
                L.pending_blank = 1;


            switch (t->kind) {
            case TK_LBRACE: {
                /* A brace is a block when it opens a function body
                 * (after the parameter list), a do/else body, or
                 * follows a control keyword on the same line; any
                 * other brace is a literal (struct, array, tuple). */
                int block = 0;
                if (L.prev == TK_RPAREN || L.prev == TK_KW_ELSE ||
                    L.prev == TK_KW_DO)
                    block = 1;
                else if (L.line_has) {
                    /* scan only the current line for a control
                     * keyword; earlier lines must not leak in */
                    static const char *const ctl[] = {
                        "if", "while", "for", "match", "else", "do",
                        "func", "case", NULL };
                    const char *base = L.out.p;
                    size_t blen = L.out.len, ci, cl;
                    const char *nl = NULL, *p;
                    size_t remain;
                    size_t k;
                    for (k = blen; k > 0; k--)
                        if (base[k - 1] == '\n') { nl = base + k - 1; break; }
                    p = nl ? nl + 1 : base;
                    remain = blen - (size_t)(p - base);
                    for (ci = 0; ctl[ci]; ci++) {
                        size_t rl = remain;
                        const char *q = p;
                        cl = strlen(ctl[ci]);
                        while (rl >= cl) {
                            if (memcmp(q, ctl[ci], cl) == 0 &&
                                (q == p || q[-1] == ' ' || q[-1] == '{' ||
                                 q[-1] == '}' || q[-1] == '(') &&
                                (rl == cl || q[cl] == ' ' || q[cl] == '(')) {
                                block = 1;
                                break;
                            }
                            q++; rl--;
                        }
                    }
                }
                if (L.nblk < 64) L.blk[L.nblk++] = block;
                if (L.line_has) bch(&L.out, ' ');
                bch(&L.out, '{');
                if (block) {
                    L.depth++;
                    lnewline(&L);
                }
                break;
            }
            case TK_RBRACE: {
                int was_block = L.nblk > 0 ? L.blk[--L.nblk] : 1;
                if (was_block) {
                    L.depth--;
                    if (L.depth < 0) L.depth = 0;
                    if (L.line_has) lnewline(&L);
                    else L.pending_blank = 0;
                    bindent(&L.out, L.depth);
                } else if (L.line_has) {
                    bch(&L.out, ' ');
                }
                bch(&L.out, '}');
                L.line_has = 1;
                if (i + 1 < (size_t)tv.count &&
                    tv.items[i + 1].kind != TK_EOF &&
                    tv.items[i + 1].kind != TK_SEMI &&
                    tv.items[i + 1].kind != TK_COMMA &&
                    tv.items[i + 1].kind != TK_RPAREN &&
                    tv.items[i + 1].kind != TK_RBRACE &&
                    tv.items[i + 1].kind != TK_KW_ELSE &&
                    was_block)
                    lnewline(&L);
                break;
            }
            case TK_SEMI:
                bch(&L.out, ';');
                lnewline(&L);
                break;
            default: {
                TokenKind ck = t->kind;
                int kw_cont = 0;
                if (ck == TK_KW_ELSE || ck == TK_KW_CASE) {
                    if (L.line_has && L.prev != TK_LBRACE)
                        bch(&L.out, ' ');
                    else {
                        if (L.line_has) lnewline(&L);
                        lbegin(&L);
                    }
                    kw_cont = 1;
                }
                if (L.line_has && !kw_cont &&
                    fspace(L.prev, L.last_pipe_close, L.pipe_open, ck))
                    bch(&L.out, ' ');
                lbegin(&L);
                if (t->kind == TK_STRING_LIT) bch(&L.out, '"');
                else if (t->kind == TK_CHAR_LIT) bch(&L.out, 39);
                bput(&L.out, t->start, (size_t)t->len);
                if (t->kind == TK_STRING_LIT) bch(&L.out, '"');
                else if (t->kind == TK_CHAR_LIT) bch(&L.out, 39);
                break;
            }
            }
            L.prev_unary =
                (t->kind == TK_MINUS || t->kind == TK_PLUS ||
                 t->kind == TK_TILDE || t->kind == TK_BANG ||
                 t->kind == TK_AT) &&
                !is_operand_end(L.prev);
            L.prev = t->kind;
            if (t->kind == TK_PIPE) {
                L.last_pipe_close = L.pipe_open;
                L.pipe_open = !L.pipe_open;
            } else if (!L.pipe_open) {
                L.last_pipe_close = 0;
            }
            L.prev_start = t->start;
            L.prev_len = t->len;
            L.prev_line = t->line;
        }
        /* a file consisting only of use statements */
        if (!uses_emitted && nuses > 0) {
            int q;
            qsort(uses, (size_t)nuses, sizeof(UseLine), use_cmp);
            for (q = 0; q < nuses; q++) {
                lbegin(&L);
                bstr(&L.out, uses[q].text);
                lnewline(&L);
            }
        }
    }

    if (L.line_has) lnewline(&L);
    while (L.out.len > 0 &&
           (L.out.p[L.out.len - 1] == '\n' || L.out.p[L.out.len - 1] == ' ' ||
            L.out.p[L.out.len - 1] == '\t'))
        L.out.len--;
    bch(&L.out, '\n');

format_cleanup:
    for (k = 0; k < (size_t)nuses; k++) free(uses[k].text);
    free(uses);
    free(use_end);
    if (L.out.failed) {
        token_vec_free(&tv);
        diag_free(&d);
        return -1;
    }

    /* Safety contract: the formatted output must carry exactly the
     * same token sequence as the input. When the re-lex fails or any
     * token differs, the file is left untouched - a formatter that
     * can lose source text is worse than none. */
    {
        Source fs;
        TokenVec nv;
        Diag nd;
        size_t a;
        int same;
        fs.path = src->path;
        fs.text = L.out.p;
        fs.len = (long)L.out.len;
        diag_init(&nd, 256);
        lexer_run(&fs, &nv, &nd);
        same = diag_error_count(&nd) == 0 && nv.count == tv.count;
        /* comments leave no tokens behind: count the comment
         * markers in both texts and refuse when any vanished */
        if (same &&
            ccount(src->text, src->len) != ccount(L.out.p, (long)L.out.len))
            same = 0;
        for (a = 0; same && a < (size_t)tv.count; a++) {
            if (nv.items[a].kind != tv.items[a].kind ||
                nv.items[a].len != tv.items[a].len ||
                memcmp(nv.items[a].start, tv.items[a].start,
                       (size_t)tv.items[a].len) != 0)
                same = 0;
        }
        token_vec_free(&nv);
        diag_free(&nd);
        if (!same) {
            free(L.out.p);
            token_vec_free(&tv);
            diag_free(&d);
            return 1;      /* token stream changed: refuse to write */
        }
    }
    *out = L.out;
    token_vec_free(&tv);
    diag_free(&d);
    return 0;
}

int cmd_fmt(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    Cli options = *cli;
    char cfg[PAXSY_PATH_MAX], value[32];
    if (toml_find(root, cfg, sizeof(cfg)) == 0) {
        if (toml_get(cfg, "source", "recursive", value, sizeof(value)))
            options.recursive = strcmp(value, "true") == 0;
        else if (toml_get(cfg, "build", "recursive", value, sizeof(value)))
            options.recursive = strcmp(value, "true") == 0; /* legacy */
    }
    SrcList srcs = {0};
    int changed = 0, failed = 0, i;

    if (srcdir_collect(root, options.recursive, 0, &srcs) < 0) {
        fprintf(stderr, "source discovery failed under '%s'\n", root);
        return 1;
    }
    if (srcs.count == 0) {
        srclist_free(&srcs);
        fprintf(stderr, "no .px sources under '%s/src'\n", root);
        return 1;
    }
    for (i = 0; i < srcs.count; i++) {
        const char *path = srcs.paths[i];
        Source src;
        Buf out = {0};
        if (source_load(&src, path) != 0) { failed = 1; continue; }
        {
            int fr = format_source(&src, &out);
            if (fr < 0) {
                fprintf(stderr, "fmt: cannot parse '%s'\n", path);
                source_free(&src);
                failed = 1;
                continue;
            }
            if (fr > 0) {
                source_free(&src);
                continue;
            }
        }
        if (out.len != src.len ||
            (src.len > 0 && memcmp(out.p, src.text, (size_t)src.len) != 0)) {
            changed++;
            if (options.strict) {
                printf("%s\n", path);      /* --check: list only */
            } else {
                FILE *f = fopen(path, "wb");
                int write_failed = f == NULL;
                if (f) {
                    if (out.len && fwrite(out.p, 1, out.len, f) != out.len)
                        write_failed = 1;
                    if (fclose(f) != 0) write_failed = 1;
                }
                if (write_failed) {
                    fprintf(stderr, "fmt: cannot write '%s'\n", path);
                    failed = 1;
                }
            }
        }
        free(out.p);
        source_free(&src);
    }
    srclist_free(&srcs);
    if (failed) return 1;
    if (options.strict && changed) return 1;   /* --check: diff found */
    return 0;
}
