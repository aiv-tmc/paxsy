#include "common.h"
#include "lower_int.h"

/*
 * lower_str.c - lowering of string and character literal nodes.
 *
 * The lexer token for a string or character literal already excludes
 * both quote characters: the token starts immediately after the
 * opening quote and ends immediately before the closing one. The HIR
 * constant node stores a freshly allocated, decoded copy of that
 * slice: documented escapes (\n, \t, \r, \0, \\, \', \", \xNN)
 * become their byte values here, so every emitter works with real
 * bytes and re-escapes only for textual output formats (C, LLVM).
 */

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes the documented escape sequences of a literal body into a
 * fresh buffer. Anything unrecognized keeps the backslash verbatim. */
static char *ldup_decoded(const char *s, int len, int *out_len) {
    char *buf = malloc((size_t)len + 1);
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            char e = s[i + 1];
            if (e == 'n') { buf[j++] = '\n'; i++; continue; }
            if (e == 't') { buf[j++] = '\t'; i++; continue; }
            if (e == 'r') { buf[j++] = '\r'; i++; continue; }
            if (e == '0') { buf[j++] = '\0'; i++; continue; }
            if (e == '\\') { buf[j++] = '\\'; i++; continue; }
            if (e == '\'') { buf[j++] = '\''; i++; continue; }
            if (e == '"') { buf[j++] = '"'; i++; continue; }
            if (e == 'x' && i + 3 < len) {
                int hi = hexv(s[i + 2]), lo = hexv(s[i + 3]);
                if (hi >= 0 && lo >= 0) {
                    buf[j++] = (char)((hi << 4) | lo);
                    i += 3;
                    continue;
                }
            }
        }
        buf[j++] = s[i];
    }
    buf[j] = '\0';
    *out_len = j;
    return buf;
}

/* Lowers one string/char literal AST node to a string-table constant. */
HNode *lower_string(Lower *l, const Node *n) {
    HNode *s = lnode(l, HK_CONST_STRING, n->tok);
    int len = 0;
    s->u.slice.start = ldup_decoded(n->tok.start, (int)n->tok.len, &len);
    s->u.slice.len = len;
    s->aux = -1;
    lflags(s);
    return s;
}

/* An interpolated string lowers to an HK_FMT node whose items
 * alternate lowered chunk literals (even slots) and expressions
 * (odd slots). */
HNode *lower_fmt(Lower *l, const Node *n) {
    HNode *f = lnode(l, HK_FMT, n->tok);
    f->items = calloc((size_t)n->item_count, sizeof(HNode *));
    f->nitems = n->item_count;
    for (int i = 0; i < n->item_count; i++) {
        const Node *c = n->items[i];
        f->items[i] = (i % 2 == 0) ? lower_string(l, c)
                                   : lower_expr(l, c);
    }
    return f;
}
