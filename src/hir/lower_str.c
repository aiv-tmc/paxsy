#include "common.h"
#include "lower_int.h"

/*
 * lower_str.c - lowering of string and character literal nodes.
 *
 * The lexer token for a string or character literal already excludes
 * both quote characters: the token starts immediately after the
 * opening quote and ends immediately before the closing one. The HIR
 * constant node stores a freshly allocated copy of that slice: the
 * source buffer is released when the analysis phase returns, long
 * before emission runs, so a borrowed pointer would dangle (and the
 * reuse of that block by later allocations is exactly what turned
 * the copy into a requirement). Escape sequences stay inside the
 * copy exactly as written; the emitters re-escape them when they
 * print the string table.
 */

/* Lowers one string/char literal AST node to a string-table constant. */
HNode *lower_string(Lower *l, const Node *n) {
    HNode *s = lnode(l, n->tok.kind == TK_CHAR_LIT ? HK_CONST_CHAR
                                                  : HK_CONST_STRING,
                     n->tok);
    char *buf = malloc((size_t)n->tok.len + 1);
    memcpy(buf, n->tok.start, (size_t)n->tok.len);
    buf[n->tok.len] = '\0';
    s->u.slice.start = buf;
    s->u.slice.len = n->tok.len;
    s->aux = -1;
    lflags(s);
    return s;
}
