#include "common.h"
#include "llic_int.h"

/*
 * llic_stmt.c - the statement walker, function printers and the entry
 * point of the LLVM IR emission pass.
 *
 * Control flow lowers onto named basic blocks (L<n>). The emitter
 * tracks whether the current block already ends in a terminator, so
 * branches are never emitted after a return, which would be invalid
 * IR. Local slots are typed by the normalized class recorded in the
 * slot table (sub-word integers share i32, Int64 is i64, Bool is i1,
 * pointers are opaque ptr), reset per function. A function declared
 * T! returns its px_res_<T> struct: success values are built with
 * insertvalue, 'return error c' is a constant struct, and the implicit
 * tail returns zeroinitializer. try in statement form extracts the
 * error code, branches to a propagate block on failure and continues
 * in a fresh OK block; for the entry point main the propagate block
 * returns the exit status. match lowers onto an LLVM switch with one
 * entry per alternative pattern and an unreachable-terminated merge
 * block when no branch reaches it. The loop induction variable reuses
 * the HIR slot alloca, so body reads through HK_LOCAL see the same
 * object. Every function of every module - including the standard
 * library, which is compiled from Paxsy sources like any other code -
 * prints its signature in sweep 1, so call sites reference exactly
 * the symbols the unit defines.
 */

void lblock(LE *l, const HNode *n, int depth) {
    for (int i = 0; i < n->nitems; i++)
        lstmt(l, n->items[i], depth);
}

/* Statement-level try: call, extract the error code, branch to a
 * propagate block on failure, continue in a fresh OK block. Caller
 * and callee result structs are assumed to match, which holds for
 * same-typed propagation in the supported subset. */
static void ltry_stmt(LE *l, const HNode *try_node, int dst_slot) {
    const HNode *call = try_node->child[0];
    const HType *vt = try_node->type;
    int is_main = l->cur_func && l->cur_func->name &&
                  strcmp(l->cur_func->name, "main") == 0;
    int prop = ++l->label, ok = ++l->label;
    char rn[64], rname[32];
    int e, c2, val;
    lres_name(vt ? vt : NULL, rn, sizeof(rn));
    lexpr(l, call, rname, sizeof(rname));
    e = ++l->reg;
    fprintf(l->out, "    %%%d = extractvalue %s %s, 1\n", e, rn, rname);
    c2 = ++l->reg;
    fprintf(l->out, "    %%%d = icmp ne i32 %%%d, 0\n", c2, e);
    fprintf(l->out, "    br i1 %%%d, label %%L%d, label %%L%d\n", c2, prop, ok);
    l->terminated = 0;
    fprintf(l->out, "L%d:\n", prop);
    if (is_main) {
        fprintf(l->out, "    ret i32 %%%d\n", e);
    } else {
        const HType *cvt = lfunc_res_value(l->cur_func);
        char crn[64];
        lres_name(cvt ? cvt : vt, crn, sizeof(crn));
        fprintf(l->out, "    ret %s %s\n", crn, rname);
    }
    l->terminated = 0;
    fprintf(l->out, "L%d:\n", ok);
    if (dst_slot >= 0) {
        char vty[8];
        lslot_norm(vt, vty, sizeof(vty));
        val = ++l->reg;
        fprintf(l->out, "    %%%d = extractvalue %s %s, 0\n", val, rn, rname);
        fprintf(l->out, "    store %s %%%d, ptr %%loc%d\n",
                vty, val, dst_slot);
    }
}

void lstmt(LE *l, const HNode *n, int depth) {
    if (!n) return;
    switch (n->kind) {
    case HK_LET: {
        char ty[8];
        lslot_norm(n->type, ty, sizeof(ty));
        lslot_set(l, n->u.index, n->type);
        if (n->child[1] && n->child[1]->kind == HK_TRY) {
            fprintf(l->out, "    %%loc%d = alloca %s\n", n->u.index, ty);
            ltry_stmt(l, n->child[1], n->u.index);
            return;
        }
        char v[32];
        lexpr(l, n->child[1], v, sizeof(v));
        fprintf(l->out, "    %%loc%d = alloca %s\n", n->u.index, ty);
        fprintf(l->out, "    store %s %s, ptr %%loc%d\n", ty, v, n->u.index);
        return;
    }
    case HK_EXPR_STMT: {
        if (n->child[0] && n->child[0]->kind == HK_TRY) {
            ltry_stmt(l, n->child[0], -1);
            return;
        }
        char v[32];
        lexpr(l, n->child[0], v, sizeof(v));
        return;
    }
    case HK_RETURN: {
        const HType *vt = lfunc_res_value(l->cur_func);
        char v[64];
        int is_main = l->cur_func && l->cur_func->name &&
                      strcmp(l->cur_func->name, "main") == 0;
        if (!n->child[0]) {
            fputs("    ret void\n", l->out);
            l->terminated = 1;
            return;
        }
        lexpr(l, n->child[0], v, sizeof(v));
        if (vt && !is_main) {
            /* Wrap into the result struct: errors keep the constant
             * produced by HK_ERR_VAL, values get a zero error code. */
            char rn[64], vty[32];
            if (n->child[0]->kind == HK_ERR_VAL) {
                fprintf(l->out, "    ret %s\n", v);
                l->terminated = 1;
            } else {
                int r = ++l->reg;
                lres_name(vt, rn, sizeof(rn));
                lty_buf(vt, vty, sizeof(vty));
                fprintf(l->out, "    %%%d = insertvalue %s zeroinitializer,"
                        " %s %s, 0\n", r, rn, vty, v);
                fprintf(l->out, "    ret %s %%%d\n", rn, r);
                l->terminated = 1;
            }
        } else if (vt && is_main) {
            /* main returns the exit status: an error code passes
             * through, a value path reports success. */
            if (n->child[0]->kind == HK_ERR_VAL) {
                fprintf(l->out, "    ret %s\n", v);
                l->terminated = 1;
            } else {
                fputs("    ret i32 0\n", l->out);
                l->terminated = 1;
            }
        } else {
            fprintf(l->out, "    ret i32 %s\n", v);
            l->terminated = 1;
        }
        return;
    }
    case HK_BLOCK:
        lblock(l, n, depth);
        return;
    case HK_IF: {
        char c[32];
        int t = ++l->label, e = ++l->label, x = ++l->label;
        lexpr(l, n->child[0], c, sizeof(c));
        fprintf(l->out, "    br i1 %s, label %%L%d, label %%L%d\n", c, t, e);
        l->terminated = 0;
        fprintf(l->out, "L%d:\n", t);
        lstmt(l, n->child[1], depth);
        if (!l->terminated)
            fprintf(l->out, "    br label %%L%d\n", x);
        l->terminated = 0;
        fprintf(l->out, "L%d:\n", e);
        if (n->child[2]) lstmt(l, n->child[2], depth);
        if (!l->terminated)
            fprintf(l->out, "    br label %%L%d\n", x);
        l->terminated = 0;
        fprintf(l->out, "L%d:\n", x);
        return;
    }
    case HK_WHILE: {
        char v[32];
        int c = ++l->label, b = ++l->label, x = ++l->label;
        fprintf(l->out, "    br label %%L%d\n", c);
        l->terminated = 0;
        fprintf(l->out, "L%d:\n", c);
        lexpr(l, n->child[0], v, sizeof(v));
        fprintf(l->out, "    br i1 %s, label %%L%d, label %%L%d\n", v, b, x);
        l->terminated = 0;
        fprintf(l->out, "L%d:\n", b);
        lstmt(l, n->child[1], depth);
        if (!l->terminated)
            fprintf(l->out, "    br label %%L%d\n", c);
        l->terminated = 0;
        fprintf(l->out, "L%d:\n", x);
        return;
    }
    case HK_FOR: {
        if (!n->child[0] || !n->child[1]) {
            lerr_(l, n, "open for-range bounds");
            return;
        }
        char lo[32], hi[32];
        int c = ++l->label, b = ++l->label, x = ++l->label;
        int r;
        lexpr(l, n->child[0], lo, sizeof(lo));
        lexpr(l, n->child[1], hi, sizeof(hi));
        lslot_set(l, n->u.index, hty_int(32));
        fprintf(l->out, "    %%loc%d = alloca i32\n", n->u.index);
        fprintf(l->out, "    store i32 %s, ptr %%loc%d\n", lo, n->u.index);
        fprintf(l->out, "    br label %%L%d\n", c);
        l->terminated = 0;
        fprintf(l->out, "L%d:\n", c);
        r = ++l->reg;
        fprintf(l->out, "    %%%d = load i32, ptr %%loc%d\n", r, n->u.index);
        fprintf(l->out, "    %%%d = icmp slt i32 %%%d, %s\n",
                ++l->reg, r, hi);
        fprintf(l->out, "    br i1 %%%d, label %%L%d, label %%L%d\n",
                l->reg, b, x);
        l->terminated = 0;
        fprintf(l->out, "L%d:\n", b);
        lstmt(l, n->child[2], depth);
        r = ++l->reg;
        fprintf(l->out, "    %%%d = load i32, ptr %%loc%d\n", r, n->u.index);
        fprintf(l->out, "    %%%d = add i32 %%%d, 1\n", ++l->reg, r);
        fprintf(l->out, "    store i32 %%%d, ptr %%loc%d\n", l->reg, n->u.index);
        if (!l->terminated)
            fprintf(l->out, "    br label %%L%d\n", c);
        l->terminated = 0;
        fprintf(l->out, "L%d:\n", x);
        return;
    }
    case HK_MATCH: {
        /* match lowers onto an LLVM switch; every case pattern must
         * be a constant integer in this subset. */
        char s[32];
        int i, used = 0, *clabels = calloc((size_t)(n->nitems + 1), sizeof(int));
        int dlabel = ++l->label, end = ++l->label;
        lexpr(l, n->child[0], s, sizeof(s));
        for (i = 0; i < n->nitems; i++) clabels[i] = ++l->label;
        fprintf(l->out, "    switch i32 %s, label %%L%d [", s, dlabel);
        for (i = 0; i < n->nitems; i++) {
            int k;
            for (k = 0; k < n->items[i]->nitems; k++) {
                const HNode *pat = n->items[i]->items[k];
                if (!pat || pat->kind != HK_CONST_INT) {
                    lerr_(l, n->items[i], "non-constant match pattern");
                    continue;
                }
                fprintf(l->out, " i32 %lld, label %%L%d",
                        pat->u.ival, clabels[i]);
            }
        }
        fputs(" ]\n", l->out);
        for (i = 0; i < n->nitems; i++) {
            l->terminated = 0;
            fprintf(l->out, "L%d:\n", clabels[i]);
            lstmt(l, n->items[i]->child[1], depth);
            if (!l->terminated) {
                fprintf(l->out, "    br label %%L%d\n", end);
                used = 1;      /* a branch reaches the merge block */
            }
        }
        l->terminated = 0;
        fprintf(l->out, "L%d:\n", dlabel);
        if (n->child[2]) lstmt(l, n->child[2], depth);
        if (!l->terminated) {
            fprintf(l->out, "    br label %%L%d\n", end);
            used = 1;
        }
        l->terminated = 0;
        fprintf(l->out, "L%d:\n", end);
        if (!used) {
            /* Every arm terminated: the merge point is unreachable
             * and must still carry a terminator. */
            fputs("    unreachable\n", l->out);
            l->terminated = 1;
        }
        free(clabels);
        return;
    }
    default:
        lerr_(l, n, "this statement");
        return;
    }
}

/* Prints the signature; returns 1 when the function is the special
 * main/Void! entry form that must return i32. */
int lfunc_sig(LE *l, const HNode *f) {
    const HType *rt = f->type;
    int main_void_err = f->name && strcmp(f->name, "main") == 0 && rt &&
        ((rt->kind == HTY_VOID) ||
         (rt->kind == HTY_ERROR && rt->u.error.value &&
          rt->u.error.value->kind == HTY_VOID));
    if (main_void_err) fputs("i32", l->out);
    else if (rt && rt->kind == HTY_VOID) fputs("void", l->out);
    else if (rt && rt->kind == HTY_ERROR) {
        char rn[64];
        lres_name(rt->u.error.value, rn, sizeof(rn));
        fputs(rn, l->out);
    } else lty(l, rt);
    fprintf(l->out, " @%s(", f->name ? f->name : "px_fn");
    for (int i = 0; i < f->nitems; i++) {
        if (i) fputs(", ", l->out);
        lty(l, f->items[i]->type);
        fprintf(l->out, " %%p%d", i);
    }
    fputs(")", l->out);
    return main_void_err;
}

void lfunc_body(LE *l, const HNode *f) {
    const HNode *prev = l->cur_func;
    const HType *vt = lfunc_res_value(f);
    int main_void_err = lfunc_sig(l, f);
    l->cur_func = f;
    l->terminated = 0;
    free(l->slot_ty);
    l->slot_ty = NULL;
    l->nslot_ty = 0;
    fputs(" {\nentry:\n", l->out);
    if (f->child[3] && f->child[3]->kind == HK_BLOCK)
        lblock(l, f->child[3], 1);
    else if (f->child[3])
        lstmt(l, f->child[3], 1);
    if (f->type && f->type->kind == HTY_VOID && !main_void_err)
        fputs("    ret void\n", l->out);
    if (main_void_err)
        fputs("    ret i32 0\n", l->out);
    else if (vt) {
        char rn[64];
        lres_name(vt, rn, sizeof(rn));
        fprintf(l->out, "    ret %s zeroinitializer\n", rn);
    }
    fputs("}\n\n", l->out);
    l->cur_func = prev;
}

int llic_modules(HModule **mods, int nmods, Diag *diag, FILE *out) {
    LE l;
    memset(&l, 0, sizeof(l));
    l.mods = mods;
    l.nmods = nmods;
    l.diag = diag;
    l.out = out;

    fputs("; generated by paxsy llic\n", out);
    fputs("px_res_i8 = type { i8, i32 }\n", out);
    fputs("px_res_i16 = type { i16, i32 }\n", out);
    fputs("px_res_i32 = type { i32, i32 }\n", out);
    fputs("px_res_i64 = type { i64, i32 }\n", out);
    fputs("px_res_float = type { float, i32 }\n", out);
    fputs("px_res_double = type { double, i32 }\n", out);
    fputs("px_res_void = type { i32, i32 }\n\n", out);
    /* printf stays declared for the interpolated string feature,
     * which is the one listing-level construct that documents its
     * libc dependency. */
    fputs("declare i32 @printf(ptr, ...)\n\n", out);

    /* Sweep 1: string constants and signatures for every function. */
    for (int m = 0; m < nmods; m++) {
        HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            HNode *d = root->items[i];
            if (d->kind == HK_FUNC || d->kind == HK_EXTERN_FUNC) {
                if (d->child[3]) lcollect_strings(&l, d->child[3]);
                lfunc_sig(&l, d);
                fputs("\n", out);
            }
        }
    }
    fputc('\n', out);

    /* Sweep 2: bodies of defined functions. */
    for (int m = 0; m < nmods; m++) {
        HNode *root = hir_root(mods[m]);
        for (int i = 0; i < root->nitems; i++) {
            HNode *d = root->items[i];
            if (d->kind == HK_FUNC) lfunc_body(&l, d);
        }
    }
    return diag_error_count(diag);
}
