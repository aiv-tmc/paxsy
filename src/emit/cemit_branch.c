#include "common.h"
#include "cemit_int.h"

/*
 * cemit_branch.c - branch values: if/match expressions lowered into
 * nested C assignments.
 *
 * A branch block's value is its trailing expression statement (the
 * parser's convention, kept by lowering). cblock_into() emits every
 * statement of the block and then assigns that trailing value into a
 * caller-provided target; a trailing if/match recurses into
 * cassign_branch() instead of needing a C expression form.
 *
 * cassign_branch() drives the nesting: an if emits the condition,
 * then assigns the value on each arm; a match binds the subject to
 * one scratch and chains comparisons (multiple patterns per case
 * become ||-ed alternatives). Arms without a value simply emit no
 * assignment, leaving the target at its initialized state.
 */

int cblock_into(CE *c, const HNode *blk, const char *target, int depth) {
    if (!blk || blk->nitems == 0) return 0;
    for (int i = 0; i + 1 < blk->nitems; i++)
        cstmt(c, blk->items[i], depth);
    const HNode *last = blk->items[blk->nitems - 1];
    if (last->kind != HK_EXPR_STMT) return 0;
    const HNode *v = last->child[0];
    cindent(c, depth);
    if (v && (v->flags & HF_EXPR) &&
        (v->kind == HK_IF || v->kind == HK_MATCH)) {
        cassign_branch(c, v, target, depth);
    } else {
        fprintf(c->out, "%s = ", target);
        cexpr(c, v);
        fputs(";\n", c->out);
    }
    return 1;
}

/* One if expression: assign on both arms when they carry values. A
 * bare condition if (no block arms) assigns the condition itself. */
static void cbranch_if(CE *c, const HNode *n, const char *target,
                       int depth) {
    if (!n->child[1]) {
        cindent(c, depth);
        fprintf(c->out, "%s = ", target);
        cexpr(c, n->child[0]);
        fputs(";\n", c->out);
        return;
    }
    cindent(c, depth);
    fputs("if (", c->out);
    cexpr(c, n->child[0]);
    fputs(") {\n", c->out);
    if (!cblock_into(c, n->child[1], target, depth + 1))
        cerr_(c, n, "if branch without a value");
    cindent(c, depth);
    if (n->child[2]) {
        fputs("} else {\n", c->out);
        if (!cblock_into(c, n->child[2], target, depth + 1))
            cerr_(c, n, "else branch without a value");
        cindent(c, depth);
    }
    fputs("}\n", c->out);
}

/* One match expression: bind the subject once, then an if/else chain
 * of ||-ed pattern comparisons. */
static void cbranch_match(CE *c, const HNode *n, const char *target,
                          int depth) {
    char subj[32];
    snprintf(subj, sizeof(subj), "px_m%d", c->temp++);
    cindent(c, depth);
    cty(c, n->child[0]->type ? n->child[0]->type : hty_int(64));
    fprintf(c->out, " %s = ", subj);
    cexpr(c, n->child[0]);
    fputs(";\n", c->out);
    for (int i = 0; i < n->nitems; i++) {
        const HNode *kase = n->items[i];
        cindent(c, depth);
        fputs(i == 0 ? "if (" : "else if (", c->out);
        for (int k = 0; k < kase->nitems; k++) {
            if (k) fputs(" || ", c->out);
            fprintf(c->out, "%s == ", subj);
            cexpr(c, kase->items[k]);
        }
        fputs(") {\n", c->out);
        if (!cblock_into(c, kase->child[1], target, depth + 1))
            cerr_(c, n, "match case without a value");
        cindent(c, depth);
        fputs("}\n", c->out);
    }
    if (n->child[2]) {
        cindent(c, depth);
        fputs("else {\n", c->out);
        if (!cblock_into(c, n->child[2], target, depth + 1))
            cerr_(c, n, "match else without a value");
        cindent(c, depth);
        fputs("}\n", c->out);
    }
}

void cassign_branch(CE *c, const HNode *v, const char *target, int depth) {
    if (v->kind == HK_IF) cbranch_if(c, v, target, depth);
    else if (v->kind == HK_MATCH) cbranch_match(c, v, target, depth);
    else cerr_(c, v, "not a branch expression");
}
