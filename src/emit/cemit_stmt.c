#include "common.h"
#include "cemit_int.h"

/*
 * cemit_stmt.c - C statement emission (core statements).
 *
 * The heavy control-flow helpers live in cemit_ctrl.c: try/catch
 * lowering (ctry_let/ccatch_let), range-loop emission with the GPU
 * dispatch (cfor_host/cfor_gpu), labeled loops (clabeled) and select
 * (cselect). This file owns the block/let/store/expression-statement
 * forms, the plain control flow, match-as-switch, return with its
 * T! forms, and break/continue with the label and increment rules.
 *
 * Defer: a block collects its direct HK_DEFER items and emits their
 * bodies at the closing brace, so a block that falls off the end runs
 * its defers. A return that is a direct item of the block runs them
 * first; returns hidden deeper in control flow are not intercepted.
 *
 * Expression statements: a bare assignment or a channel operation is
 * parsed as an expression, so it arrives wrapped in HK_EXPR_STMT;
 * this file unwraps the assignment to a store statement and routes the
 * channel/scheduler forms to cstmt_aux instead of asking cexpr for a
 * value form they do not have.
 *
 * Range loops: '..' is exclusive at the upper bound, so the generated
 * condition is '<'; the HF_EXCLUSIVE flag ('..<') is the inclusive
 * variant and uses '<='. Array variables initialize from a plain
 * brace list: a compound literal is not a constant expression in C99,
 * so array variables cannot start from one.
 */

/* The block items of a defer-carrying block: defers run last. */
static void cblock_items(CE *c, const HNode *blk, int depth) {
    for (int i = 0; i < blk->nitems; i++) {
        const HNode *s = blk->items[i];
        if (s->kind == HK_DEFER) continue;
        cstmt(c, s, depth);
    }
    for (int i = 0; i < blk->nitems; i++) {
        const HNode *s = blk->items[i];
        if (s->kind == HK_DEFER && s->child[0]) {
            cindent(c, depth);
            fputs("/* defer */\n", c->out);
            cstmt(c, s->child[0], depth);
        }
    }
}

void cstmt(CE *c, const HNode *n, int depth) {
    if (!n) return;
    switch (n->kind) {
    case HK_BLOCK:
        cindent(c, depth);
        fputs("{\n", c->out);
        cblock_items(c, n, depth + 1);
        cindent(c, depth);
        fputs("}\n", c->out);
        break;
    case HK_LET: {
        char tgt[24];
        cslot(tgt, 0, n->u.index);
        /* the initializer lives in child[1] */
        const HNode *init = n->child[1];
        if (init && init->kind == HK_TRY) {
            ctry_let(c, init->child[0], tgt, n->type, depth);
            break;
        }
        if (init && init->kind == HK_CATCH) {
            ccatch_let(c, init, tgt, n->type, depth);
            break;
        }
        cindent(c, depth);
        if (n->type && n->type->kind == HTY_ARRAY) {
            cty(c, n->type->u.array.elem);
            fprintf(c->out, " %s[%ld]", tgt, n->type->u.array.count);
        } else {
            cty(c, n->type);
            fprintf(c->out, " %s", tgt);
        }
        if (init && init->kind == HK_RECV) {
            fputs(" = 0;\n", c->out);
            cindent(c, depth);
            fputs("px_chan_recv((px_chan *)(void*)(", c->out);
            cexpr(c, init->child[0]);
            fprintf(c->out, "), (long long *)(void*)&%s);\n", tgt);
        } else if (init && init->kind == HK_ARRAY_LIT) {
            /* an array literal initializes with a plain brace list:
             * a compound literal is not a constant expression in
             * C99, so array variables cannot start from one */
            fputs(" = { ", c->out);
            for (int i = 0; i < init->nitems; i++) {
                if (i) fputs(", ", c->out);
                cexpr(c, init->items[i]);
            }
            fputs(" };\n", c->out);
        } else if (init && (init->flags & HF_EXPR) &&
                   (init->kind == HK_IF || init->kind == HK_MATCH)) {
            fputs(" = 0;\n", c->out);
            cassign_branch(c, init, tgt, depth);
        } else if (init) {
            fputs(" = ", c->out);
            cexpr(c, init);
            fputs(";\n", c->out);
        } else {
            fputs(" = 0;\n", c->out);
        }
        break;
    }
    case HK_STORE:
        cindent(c, depth);
        cexpr(c, n->child[0]);
        fputs(" = ", c->out);
        cexpr(c, n->child[1]);
        fputs(";\n", c->out);
        break;
    case HK_EXPR_STMT: {
        const HNode *e = n->child[0];
        /* a bare assignment arrives wrapped in an expression
         * statement; emit it as a store, not as a value */
        if (e && e->kind == HK_STORE) {
            cindent(c, depth);
            cexpr(c, e->child[0]);
            fputs(" = ", c->out);
            cexpr(c, e->child[1]);
            fputs(";\n", c->out);
            break;
        }
        /* channel statements, select, panic, asm and the scheduler
         * forms keep their own statement emission */
        if (e && (e->kind == HK_SEND || e->kind == HK_CLOSE ||
                  e->kind == HK_RECV || e->kind == HK_SELECT ||
                  e->kind == HK_PANIC || e->kind == HK_INLINE_ASM ||
                  e->kind == HK_GO_ROUTINE || e->kind == HK_ASYNC ||
                  e->kind == HK_AWAIT)) {
            if (!cstmt_aux(c, e, depth))
                cerr_(c, e, "unsupported statement in the C backend");
            break;
        }
        if (e && e->kind == HK_TRY) { ctry_let(c, e->child[0], NULL, NULL, depth); break; }
        if (e && e->kind == HK_CATCH) { ccatch_let(c, e, NULL, NULL, depth); break; }
        if (e && e->kind == HK_RECV) {
            cindent(c, depth);
            fputs("(void)px_chan_recv0((px_chan *)(void*)(", c->out);
            cexpr(c, e->child[0]);
            fputs("));\n", c->out);
            break;
        }
        if (e && (e->flags & HF_EXPR) &&
            (e->kind == HK_IF || e->kind == HK_MATCH)) {
            char t[32];
            snprintf(t, sizeof(t), "px_v%d", c->temp++);
            cindent(c, depth);
            cty(c, e->type);
            fprintf(c->out, " %s;\n", t);
            cassign_branch(c, e, t, depth);
            break;
        }
        cindent(c, depth);
        cexpr(c, e);
        fputs(";\n", c->out);
        break;
    }
    case HK_IF:
        if (!n->child[1]) {
            cindent(c, depth);
            cexpr(c, n->child[0]);
            fputs(";\n", c->out);
            break;
        }
        cindent(c, depth);
        fputs("if (", c->out);
        cexpr(c, n->child[0]);
        fputs(") ", c->out);
        cstmt(c, n->child[1], depth);
        if (n->child[2]) {
            fputs("\n", c->out);
            cindent(c, depth);
            fputs("else ", c->out);
            cstmt(c, n->child[2], depth);
        }
        fputs("\n", c->out);
        break;
    case HK_WHILE:
        cindent(c, depth);
        fputs("while (", c->out);
        cexpr(c, n->child[0]);
        fputs(") ", c->out);
        cstmt(c, n->child[1], depth);
        fputs("\n", c->out);
        break;
    case HK_DOWHILE:
        cindent(c, depth);
        fputs("do ", c->out);
        cstmt(c, n->child[0], depth);
        fputs(" while (", c->out);
        cexpr(c, n->child[1]);
        fputs(");\n", c->out);
        break;
    case HK_FOR:
        if (!cfor_gpu(c, n, depth))
            cfor_host(c, n, depth);
        break;
    case HK_MATCH: {
        cindent(c, depth);
        fputs("switch (", c->out);
        cexpr(c, n->child[0]);
        fputs(") {\n", c->out);
        for (int i = 0; i < n->nitems; i++) {
            const HNode *kase = n->items[i];
            for (int k = 0; k < kase->nitems; k++) {
                cindent(c, depth + 1);
                fputs("case ", c->out);
                cexpr(c, kase->items[k]);
                fputs(":\n", c->out);
            }
            cblock_items(c, kase->child[1], depth + 2);
            cindent(c, depth + 2);
            fputs("break;\n", c->out);
        }
        if (n->child[2]) {
            cindent(c, depth + 1);
            fputs("default:\n", c->out);
            cblock_items(c, n->child[2], depth + 2);
            cindent(c, depth + 2);
            fputs("break;\n", c->out);
        }
        cindent(c, depth);
        fputs("}\n", c->out);
        break;
    }
    case HK_RETURN: {
        const HNode *v = n->child[0];
        if (v && v->kind == HK_TRY) {
            char rn[64], t[32];
            cur_res_name(c, rn, sizeof(rn));
            snprintf(t, sizeof(t), "px_t%d", c->temp++);
            cindent(c, depth);
            fprintf(c->out, "struct %s %s = ", rn, t);
            cexpr(c, v->child[0]);
            fputs(";\n", c->out);
            cindent(c, depth);
            fprintf(c->out, "if (%s.err != 0) return %s;\n", t, t);
            cindent(c, depth);
            fprintf(c->out, "return %s.value;\n", t);
            break;
        }
        cindent(c, depth);
        if (!v || cis_main(c)) {
            fputs("return 0;\n", c->out);
        } else if (v->kind == HK_ERR_VAL) {
            char rn[64];
            cur_res_name(c, rn, sizeof(rn));
            fprintf(c->out, "return (struct %s){ ", rn);
            if (v->child[0] && v->child[0]->kind == HK_CONST_INT)
                cexpr(c, v->child[0]);
            else cerr_(c, v, "error value must be a code literal");
            fputs(" };\n", c->out);
        } else if (cfunc_res_value(c->cur_func)) {
            /* a plain value returned from a T! function is the
             * success path: wrap it as a zeroed-error result */
            char rn[64];
            cur_res_name(c, rn, sizeof(rn));
            fprintf(c->out, "return (struct %s){ 0, ", rn);
            cexpr(c, v);
            fputs(" };\n", c->out);
        } else {
            fputs("return ", c->out);
            cexpr(c, v);
            fputs(";\n", c->out);
        }
        break;
    }
    case HK_BREAK: case HK_CONTINUE: {
        int is_break = n->kind == HK_BREAK;
        cindent(c, depth);
        if (n->name) {
            const char *l = find_lbl(c, n->name, is_break);
            if (l) fprintf(c->out, "goto %s;\n", l);
            else cerr_(c, n, "label not found for break/continue");
        } else if (!is_break && c->cur_inc) {
            fprintf(c->out, "goto %s;\n", c->cur_inc);
        } else {
            fputs(is_break ? "break;\n" : "continue;\n", c->out);
        }
        break;
    }
    case HK_LABEL:
        clabeled(c, n, depth);
        break;
    default: {
        /* Channel statements, select, inline asm and panic share one
         * dispatcher in cemit_ctrl.c; anything left is unsupported. */
        if (!cstmt_aux(c, n, depth))
            cerr_(c, n, "unsupported statement in the C backend");
        break;
    }
    }
}
