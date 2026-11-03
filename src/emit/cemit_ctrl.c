#include "common.h"
#include "cemit_int.h"
#include "offload.h"

/*
 * cemit_ctrl.c - statement emission for the control-heavy forms:
 * the T! try/catch ABI, range loops with GPU dispatch, labeled loops
 * and the channel statements (send/close/recv/select).
 *
 * Try/catch: the T! ABI passes error values as result structs. The
 * scratch struct and the payload belong to the called function (its
 * result struct names both), while error propagation builds the
 * enclosing function's struct, so a try deep inside main can forward
 * an Int! error through a Void! result. Catch runs its block on the
 * error path with the code bound to the catch slot and binds the
 * payload on the success path; a Void! callee has no payload member.
 *
 * Range loops: '..' is exclusive at the upper bound ('<'); the
 * HF_EXCLUSIVE flag ('..<') is the inclusive variant ('<='). A
 * labeled while gets a continue label before the condition (continue
 * re-tests it); a labeled for becomes an init plus a while whose
 * body ends with a labeled increment, so named and plain continues
 * always run the increment. A plain continue targets c->cur_inc, which
 * for loops set to their increment label and nested loops save and
 * restore around their own bodies.
 *
 * GPU offload: a for loop the offload analysis accepts with a sum or
 * map pattern is emitted as a PAXSY_HAS_CUDA guarded call to the
 * matching pax_gpu_* entry point (kernel ordinal from c->kernel,
 * mirroring cuda-emit), with the serial loop as the fallback. The
 * single body statement of a pattern loop arrives wrapped in an
 * expression statement and is unwrapped first. Pointer parameters the
 * device expression reads are forwarded as kernel arguments so the
 * kernel never references host frame slots.
 */

/* The result-struct name of the function under emission. */
void cur_res_name(CE *c, char *buf, size_t n) {
    cres_name(cfunc_res_value(c->cur_func), buf, n);
}

/* The result ABI of the function called by `call`: the scratch
 * struct and the payload type belong to the callee, while the
 * propagation target stays the enclosing function's struct. */
static const HType *call_res_value(CE *c, const HNode *call) {
    if (call->kind == HK_CALL && call->child[0]->kind == HK_GLOBAL_REF) {
        const char *name = call->child[0]->name;
        const HNode *fn = cfind_func(c, name);
        if (fn) return cfunc_res_value(fn);
    }
    return NULL;
}

/* let x = try f(...): scratch struct of the callee, propagate the
 * error in the caller's struct, bind the payload. */
void ctry_let(CE *c, const HNode *call, const char *target,
              const HType *ty, int depth) {
    if (!cfunc_res_value(c->cur_func)) {
        cerr_(c, call, "try needs an error-returning enclosing function");
        return;
    }
    const HType *callee = call_res_value(c, call);
    if (!callee) {
        cerr_(c, call, "try needs a call to an error-returning function");
        return;
    }
    char rn[64], crn[64], t[32];
    cur_res_name(c, rn, sizeof(rn));
    cres_name(callee, crn, sizeof(crn));
    snprintf(t, sizeof(t), "px_t%d", c->temp++);
    cindent(c, depth);
    fprintf(c->out, "struct %s %s = ", crn, t);
    cexpr(c, call);
    fputs(";\n", c->out);
    cindent(c, depth);
    fprintf(c->out, "if (%s.err != 0) return (struct %s){ %s.err };\n",
            t, rn, t);
    /* a Void! callee has no payload member */
    if (target && ty && ty->kind != HTY_VOID && callee->kind != HTY_VOID) {
        cindent(c, depth);
        cty(c, ty);
        fprintf(c->out, " %s = %s.value;\n", target, t);
    }
}

/* let x = f(...) catch |e| { blk }: payload or handler block value. */
void ccatch_let(CE *c, const HNode *cat, const char *target,
                const HType *ty, int depth) {
    const HNode *call = cat->child[0];
    if (!call || call->kind != HK_CALL) {
        cerr_(c, cat, "catch needs a call on its left side");
        return;
    }
    const HType *callee = call_res_value(c, call);
    if (!callee) {
        cerr_(c, cat, "catch needs a call to an error-returning function");
        return;
    }
    char rn[64], crn[64], t[32], slot[24];
    cur_res_name(c, rn, sizeof(rn));
    cres_name(callee, crn, sizeof(crn));
    snprintf(t, sizeof(t), "px_t%d", c->temp++);
    cindent(c, depth);
    fprintf(c->out, "struct %s %s = ", crn, t);
    cexpr(c, call);
    fputs(";\n", c->out);
    cslot(slot, 0, cat->u.index);
    cindent(c, depth);
    fprintf(c->out, "if (%s.err != 0) {\n", t);
    cindent(c, depth + 1);
    fprintf(c->out, "long long %s = %s.err;\n", slot, t);
    if (!cblock_into(c, cat->child[1], target, depth + 1))
        cerr_(c, cat, "catch block must yield a value here");
    cindent(c, depth);
    if (target) {
        fputs("} else {\n", c->out);
        if (ty && ty->kind != HTY_VOID && callee->kind != HTY_VOID) {
            cindent(c, depth + 1);
            cty(c, ty);
            fprintf(c->out, " %s = %s.value;\n", target, t);
        }
        cindent(c, depth);
        fputs("}\n", c->out);
    } else {
        fprintf(c->out, "} else (void)%s.value;\n", t);
    }
}

/* Host fallback for one range loop: '..' is exclusive, the
 * HF_EXCLUSIVE flag is the inclusive variant. */
void cfor_host(CE *c, const HNode *n, int depth) {
    char v[24];
    cslot(v, 0, n->u.index);
    const HType *lt = n->child[0] ? n->child[0]->type : NULL;
    const HType *rt = n->child[1] ? n->child[1]->type : NULL;
    const HType *vt = lt ? lt : (rt ? rt : hty_int(64));
    cindent(c, depth);
    fputs("for (", c->out);
    cty(c, vt);
    fprintf(c->out, " %s = ", v);
    if (n->child[0]) cexpr(c, n->child[0]);
    else fputs("0", c->out);
    if (n->child[1]) {
        fprintf(c->out, "; %s %s ", v,
                n->flags & HF_EXCLUSIVE ? "<=" : "<");
        cexpr(c, n->child[1]);
        fprintf(c->out, "; %s++) ", v);
    } else {
        fprintf(c->out, "; ; %s++) ", v);
    }
    cstmt(c, n->child[2], depth);
}

/* Emits a GPU-dispatchable loop; returns 1 when handled. The device
 * expression is extracted from the pattern body, and pointer
 * parameters it reads are forwarded as kernel arguments. */
int cfor_gpu(CE *c, const HNode *n, int depth) {
    int acc;
    const char *pat = offload_pattern(n, &acc);
    if (!pat) return 0;
    long long k = c->kernel++;
    const HNode *store = n->child[2]->items[0];
    const HNode *expr;
    if (store->kind == HK_EXPR_STMT) store = store->child[0];
    if (strcmp(pat, "sum") == 0) {
        const HNode *add = store->child[1];
        expr = (add->child[0]->kind == HK_LOCAL &&
                add->child[0]->u.index == acc) ? add->child[1]
                                               : add->child[0];
    } else {
        expr = store->child[1];
    }
    int slots[4], cnt = 0;
    offload_expr_params(expr, slots, &cnt);
    cindent(c, depth);
    fputs("#ifdef PAXSY_HAS_CUDA\n", c->out);
    cindent(c, depth);
    if (strcmp(pat, "sum") == 0) {
        char a[24];
        cslot(a, 0, acc);
        fprintf(c->out, "%s += pax_gpu_sum_%lld((long long)(", a, k);
        if (n->child[0]) cexpr(c, n->child[0]); else fputs("0", c->out);
        fputs("), (long long)(", c->out);
        if (n->child[1]) cexpr(c, n->child[1]); else fputs("0", c->out);
        for (int i = 0; i < cnt; i++) {
            char pb[24];
            cslot(pb, 1, slots[i]);
            fprintf(c->out, ", (long long *)(void*)(%s)", pb);
        }
        fprintf(c->out, ")%s);\n",
                n->flags & HF_EXCLUSIVE ? " + 1" : "");
    } else {  /* map: arr[i] = expr(i) */
        const HNode *base = store->child[0]->child[0];
        fprintf(c->out, "pax_gpu_map_%lld((long long *)(void*)(", k);
        if (base->kind == HK_GLOBAL_REF) {
            char nm[96];
            cname(base->name, nm, sizeof(nm));
            fputs(nm, c->out);
        } else {
            cexpr(c, base);
        }
        fputs("), (long long)(", c->out);
        if (n->child[0]) cexpr(c, n->child[0]); else fputs("0", c->out);
        fputs("), (long long)(", c->out);
        if (n->child[1]) cexpr(c, n->child[1]); else fputs("0", c->out);
        for (int i = 0; i < cnt; i++) {
            char pb[24];
            cslot(pb, 1, slots[i]);
            fprintf(c->out, ", (long long *)(void*)(%s)", pb);
        }
        fprintf(c->out, ")%s);\n",
                n->flags & HF_EXCLUSIVE ? " + 1" : "");
    }
    cindent(c, depth);
    fputs("#else\n", c->out);
    cfor_host(c, n, depth);
    cindent(c, depth);
    fputs("#endif\n", c->out);
    return 1;
}

/* A labeled loop with its generated labels; see the file header. */
void clabeled(CE *c, const HNode *lab, int depth) {
    const HNode *loop = lab->child[0];
    int id = c->glbl++;
    const char *saved_inc = c->cur_inc;
    if (c->nlbl < 16) {
        c->lbl[c->nlbl].name = lab->name;
        snprintf(c->lbl[c->nlbl].brk, sizeof(c->lbl[0].brk),
                 "px_brk_%d", id);
        /* a for's continue target is its increment label */
        snprintf(c->lbl[c->nlbl].cont, sizeof(c->lbl[0].cont),
                 loop->kind == HK_FOR ? "px_inc_%d" : "px_cont_%d", id);
        c->nlbl++;
    }
    if (loop->kind == HK_WHILE) {
        c->cur_inc = NULL;
        cindent(c, depth);
        fprintf(c->out, "%s: ;\n", c->lbl[c->nlbl - 1].cont);
        cindent(c, depth);
        fputs("while (", c->out);
        cexpr(c, loop->child[0]);
        fputs(") ", c->out);
        cstmt(c, loop->child[1], depth);
        fputs("\n", c->out);
    } else if (loop->kind == HK_FOR) {
        char v[24], inc[32];
        const HNode *body = loop->child[2];
        cslot(v, 0, loop->u.index);
        snprintf(inc, sizeof(inc), "px_inc_%d", id);
        c->cur_inc = inc;
        cindent(c, depth);
        cty(c, loop->child[0] ? loop->child[0]->type : hty_int(64));
        fprintf(c->out, " %s = ", v);
        if (loop->child[0]) cexpr(c, loop->child[0]); else fputs("0", c->out);
        fputs(";\n", c->out);
        cindent(c, depth);
        fputs("while (", c->out);
        if (loop->child[1]) {
            cexpr(c, loop->child[1]);
            if (loop->flags & HF_EXCLUSIVE) fputs(" + 1", c->out);
        } else fputs("1", c->out);
        fputs(") {\n", c->out);
        for (int i = 0; i < body->nitems; i++)
            cstmt(c, body->items[i], depth + 1);
        cindent(c, depth + 1);
        fprintf(c->out, "%s: %s++;\n", inc, v);
        cindent(c, depth);
        fputs("}\n", c->out);
    } else {           /* do-while: plain continue already re-tests  */
        c->cur_inc = NULL;
        cindent(c, depth);
        fputs("do ", c->out);
        cstmt(c, loop->child[0], depth);
        fputs(" while (", c->out);
        cexpr(c, loop->child[1]);
        fputs(");\n", c->out);
    }
    cindent(c, depth);
    fprintf(c->out, "%s: (void)0;\n", c->lbl[c->nlbl - 1].brk);
    c->cur_inc = saved_inc;
    c->nlbl--;
}

const char *find_lbl(CE *c, const char *name, int is_break) {
    for (int i = c->nlbl - 1; i >= 0; i--)
        if (strcmp(c->lbl[i].name, name) == 0)
            return is_break ? c->lbl[i].brk : c->lbl[i].cont;
    return NULL;
}

/* select: poll the arms with the try-variants, pause between
 * rounds, run the else arm once when present. */
void cselect(CE *c, const HNode *n, int depth) {
    cindent(c, depth);
    fputs("for (;;) {\n", c->out);
    for (int i = 0; i < n->nitems; i++) {
        const HNode *a = n->items[i];
        if (a->child[0]) {                 /* receive arm            */
            char slot[24];
            cindent(c, depth + 1);
            fputs("long long px_v = 0;\n", c->out);
            cindent(c, depth + 1);
            fputs("if (px_chan_try_recv(", c->out);
            cexpr(c, a->child[0]);
            fputs(", &px_v)) {\n", c->out);
            if (a->name) {
                cslot(slot, 0, a->u.index);
                cindent(c, depth + 2);
                fprintf(c->out, "long long %s = px_v;\n", slot);
            }
        } else {                           /* send arm               */
            cindent(c, depth + 1);
            fputs("if (px_chan_try_send(", c->out);
            cexpr(c, a->child[1]);
            fputs(", (long long)(", c->out);
            cexpr(c, a->child[2]);
            fputs("))) {\n", c->out);
        }
        const HNode *body = a->child[3];
        for (int k = 0; k < body->nitems; k++)
            cstmt(c, body->items[k], depth + 2);
        cindent(c, depth + 2);
        fputs("break;\n", c->out);
        cindent(c, depth + 1);
        fputs("}\n", c->out);
    }
    if (n->child[3]) {
        const HNode *els = n->child[3];
        for (int k = 0; k < els->nitems; k++)
            cstmt(c, els->items[k], depth + 1);
        cindent(c, depth + 1);
        fputs("break;\n", c->out);
    } else {
        cindent(c, depth + 1);
        fputs("px_chan_pause();\n", c->out);
    }
    cindent(c, depth);
    fputs("}\n", c->out);
}

/* Channel statements, inline asm, panic and the scheduler forms: one
 * dispatcher used from cstmt's default case. Returns 1 when the node
 * was handled. */
int cstmt_aux(CE *c, const HNode *n, int depth) {
    switch (n->kind) {
    case HK_SEND:
        cindent(c, depth);
        fputs("px_chan_send((px_chan *)(void*)(", c->out);
        cexpr(c, n->child[0]);
        fputs("), (long long)(", c->out);
        cexpr(c, n->child[1]);
        fputs("));\n", c->out);
        return 1;
    case HK_CLOSE:
        cindent(c, depth);
        fputs("px_chan_close((px_chan *)(void*)(", c->out);
        cexpr(c, n->child[0]);
        fputs("));\n", c->out);
        return 1;
    case HK_RECV:
        cindent(c, depth);
        fputs("(void)px_chan_recv0((px_chan *)(void*)(", c->out);
        cexpr(c, n->child[0]);
        fputs("));\n", c->out);
        return 1;
    case HK_SELECT:
        cselect(c, n, depth);
        return 1;
    case HK_INLINE_ASM:
        cindent(c, depth);
        fputs("__asm__ __volatile__(", c->out);
        fwrite(n->u.slice.start, 1, (size_t)n->u.slice.len, c->out);
        fputs(");\n", c->out);
        return 1;
    case HK_PANIC:
        cindent(c, depth);
        fputs("px_panic((long long)(", c->out);
        if (n->child[0]) cexpr(c, n->child[0]); else fputs("0", c->out);
        fputs("));\n", c->out);
        return 1;
    case HK_GO_ROUTINE: case HK_ASYNC: case HK_AWAIT:
        cerr_(c, n, "routine/async/await need the scheduler runtime");
        return 1;
    default:
        return 0;
    }
}
