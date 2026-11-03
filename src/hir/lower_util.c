#include "common.h"
#include "lower_int.h"

/*
 * lower_util.c - the lowering helpers shared by every lowering unit:
 * the node factory, the effect-flag OR-ing pass, the integer literal
 * parser and the integer-width table. They sit in their own unit so
 * lower.c stays focused on the declaration driver and the
 * module-prefix logic. The Makefile picks this file up automatically
 * (SRC is discovered with find), no build file changes are needed.
 */

const int lwidths[6] = { 8, 16, 32, 64, 128, 0 };  /* 0 terminates scans */

/* Context helpers --------------------------------------------------- */

/* Allocates one HIR node in the module arena. */
HNode *lnode(Lower *l, HNodeKind kind, Token tok) {
    return hir_node(l->mod, kind, tok);
}

/* Lowering has no source path of its own; the driver prints stored
 * diagnostics against the loaded source, so the pseudo-path here
 * only disambiguates the phase in bare listings. */
void lerr(Lower *l, const Token *t, const char *what) {
    diag_report(l->diag, DK_ERROR, "<lower>", t->line, t->col, "%s", what);
}

/* Parses an integer literal slice: 0x/0o/0b base prefix, '_'
 * separators skipped, value accumulated with wrapping unsigned
 * arithmetic so an out-of-range literal never hits signed-overflow
 * undefined behaviour. */
long long lint_value(const char *s, int len) {
    int i = 0, base = 10;
    unsigned long long v = 0;
    if (len >= 2 && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X' || s[1] == 'o' || s[1] == 'O' ||
         s[1] == 'b' || s[1] == 'B')) {
        base = s[1] == 'x' || s[1] == 'X' ? 16 :
               s[1] == 'o' || s[1] == 'O' ?  8 : 2;
        i = 2;
    }
    for (; i < len; i++) {
        int d;
        char c = s[i];
        if (c == '_') continue;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * (unsigned)base + (unsigned)d;
    }
    return (long long)v;
}

/* ORs the children's effect bits into the parent. Position flags
 * (HF_EXPR, HF_EXCLUSIVE) describe the node itself and never travel
 * upward. */
void lflags(HNode *n) {
    int f = 0;
    for (int i = 0; i < 4; i++)
        if (n->child[i]) {
            lflags(n->child[i]);
            f |= n->child[i]->flags;
        }
    for (int i = 0; i < n->nitems; i++) {
        lflags(n->items[i]);
        f |= n->items[i]->flags;
    }
    n->flags |= f & ~(HF_EXPR | HF_EXCLUSIVE);
}
