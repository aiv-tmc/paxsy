#include "common.h"
#include "llic_int.h"

/*
 * llic_fmt.c - the interpolated-string (HK_FMT) printers of the LLVM
 * emission pass. An interpolated string becomes exactly one printf:
 * lcollect_fmt registers the format as a module-level private global
 * during the string-collection sweep (the global id rides in the
 * node's aux field), and lexpr_fmt emits the call with the expression
 * parts widened to i64 for the varargs convention.
 */

/* Registers the format global of one HK_FMT node when it has not been
 * collected yet: the chunks verbatim ('%' doubled) and one %lld per
 * expression part. */
void lcollect_fmt(LE *l, HNode *n) {
    int i, k, total = 1;
    if (n->kind != HK_FMT || n->aux >= 0) return;
    for (i = 0; i < n->nitems; i++) {
        const HNode *p2 = n->items[i];
        if (p2->kind == HK_CONST_STRING) {
            for (k = 0; k < p2->u.slice.len; k++) {
                char ch = p2->u.slice.start[k];
                total += (ch == '%') ? 2 : 1;
            }
        } else total += 4;
    }
    fprintf(l->out, "@.str%d = private constant [%d x i8] c\"",
            l->strno, total);
    for (i = 0; i < n->nitems; i++) {
        const HNode *p2 = n->items[i];
        if (p2->kind == HK_CONST_STRING) {
            for (k = 0; k < p2->u.slice.len; k++) {
                char ch = p2->u.slice.start[k];
                if (ch == '%') fputs("%%", l->out);
                else if (ch == '"' || ch == '\\') fprintf(l->out, "\\%02X", ch);
                else fputc(ch, l->out);
            }
        } else {
            fputs("%lld", l->out);
        }
    }
    fputs("\\00\"\n", l->out);
    n->aux = l->strno;
    l->strno++;
    /* The chunks are inlined into the format; mark them so the
     * generic collector skips them. */
    for (i = 0; i < n->nitems; i++)
        if (n->items[i]->kind == HK_CONST_STRING)
            n->items[i]->aux = -2;
}

/* Emits the printf for one HK_FMT node against its format global. */
void lexpr_fmt(LE *l, const HNode *n, char *res, size_t rn) {
    char a[8][32], w[8][32];
    int i, r = ++l->reg, na = 0;
    for (i = 0; i < n->nitems && i < 8; i++) {
        const HNode *p2 = n->items[i];
        int e2;
        if (p2->kind == HK_CONST_STRING) continue;
        lexpr(l, p2, a[na], sizeof(a[na]));
        e2 = ++l->reg;
        fprintf(l->out, "    %%%d = sext i32 %s to i64\n", e2, a[na]);
        snprintf(w[na], sizeof(w[na]), "%%%d", e2);
        na++;
    }
    fprintf(l->out, "    %%%d = call i32 (ptr, ...) @printf(ptr @.str%d",
            r, n->aux);
    for (i = 0; i < na; i++)
        fprintf(l->out, ", i64 %s", w[i]);
    fputs(")\n", l->out);
    snprintf(res, rn, "%%%d", r);
}
