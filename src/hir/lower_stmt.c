#include "common.h"
#include "lower_int.h"

/*
 * lower_stmt.c - statement lowering: blocks, bindings, control flow,
 * channels, concurrency statements and inline assembly.
 *
 * Statements that produce no node (pass) return NULL and the caller
 * skips them; volatile unwraps to its inner statement because HIR
 * carries no volatile marker. Every binding form allocates a fresh
 * local slot and registers the name in the current scope, so later
 * HK_LOCAL/HK_PARAM references resolve by slot index. Select arms
 * keep the parser layout: a receive arm holds its channel in child[0]
 * and its block in child[3], a send arm holds the channel and value
 * in child[1]/child[2]; the discriminator is which of child[0] or
 * child[1] is present. A send or receive written as a statement is
 * parsed as an expression statement, so only expression lowering
 * handles those node kinds. A let keeps its initializer in child[1],
 * mirroring the parser layout the backends consume.
 */

/* Lowers a block: one HK_BLOCK whose items are the lowered
 * statements in source order. A trailing expression statement stays
 * an HK_EXPR_STMT; block value consumers (branch blocks of if/match
 * expressions) read it from the last item, mirroring the parser. */
HNode *lower_block(Lower *l, const Node *b) {
    HNode *blk = lnode(l, HK_BLOCK, b->tok);
    for (int i = 0; i < b->item_count; i++) {
        HNode *s = lower_stmt(l, b->items[i]);
        if (s) hir_add(blk, s);
    }
    return blk;
}

/* let x [: T] [= init]; - the initializer sees the outer scope only,
 * so it is lowered before the name is registered; it lands in
 * child[1], the annotation position child[0] is consumed by
 * lower_type at declaration time. */
static HNode *lower_let(Lower *l, const Node *n) {
    HNode *let = lnode(l, HK_LET, n->tok);
    let->name = n->name;
    let->type = n->child[0] ? lower_type(l, n->child[0]) : NULL;
    if (n->child[1]) let->child[1] = lower_expr(l, n->child[1]);
    let->u.index = l->next_local++;
    lscope_put(l, n->name, let->u.index, 0);
    return let;
}

/* for <lo>..<hi> [| i [: T]] { body } - the loop variable (slot
 * assigned here, type assigned by the typing pass from the bounds)
 * is visible only inside the body. Open bounds stay NULL children so
 * the offload analysis can recognise them without re-reading flags. */
static HNode *lower_for(Lower *l, const Node *n) {
    const Node *r = n->child[0];
    HNode *f = lnode(l, HK_FOR, n->tok);
    f->name = n->name;
    if (r->flags & NF_EXCLUSIVE) f->flags |= HF_EXCLUSIVE;
    if (!(r->flags & NF_OPEN_LO)) f->child[0] = lower_expr(l, r->child[0]);
    if (!(r->flags & NF_OPEN_HI)) f->child[1] = lower_expr(l, r->child[1]);
    f->u.index = l->next_local++;
    lscope_push(l);
    if (n->name) lscope_put(l, n->name, f->u.index, 0);
    f->child[2] = lower_block(l, n->child[3]);
    lscope_pop(l);
    return f;
}

/* match: one HK_MATCH, case nodes keep their patterns in items and
 * the body block in child[1] (the layout the C backend's branch
 * emitter expects). Expression matches carry HF_EXPR. */
static HNode *lower_match(Lower *l, const Node *n) {
    HNode *m = lnode(l, HK_MATCH, n->tok);
    if (n->flags & NF_EXPR) m->flags |= HF_EXPR;
    m->child[0] = lower_expr(l, n->child[0]);
    for (int i = 0; i < n->item_count; i++) {
        const Node *c = n->items[i];
        HNode *hc = lnode(l, HK_BLOCK, c->tok);   /* case container   */
        for (int k = 0; k < c->item_count; k++)
            hir_add(hc, lower_expr(l, c->items[k]));
        lscope_push(l);
        hc->child[1] = lower_block(l, c->child[1]);
        lscope_pop(l);
        hir_add(m, hc);
    }
    if (n->child[2]) {
        lscope_push(l);
        m->child[2] = lower_block(l, n->child[2]);
        lscope_pop(l);
    }
    return m;
}

/* select: arms keep the parser layout (see the file header). */
static HNode *lower_select(Lower *l, const Node *n) {
    HNode *s = lnode(l, HK_SELECT, n->tok);
    for (int i = 0; i < n->item_count; i++) {
        const Node *a = n->items[i];
        HNode *ha = lnode(l, HK_BLOCK, a->tok);
        ha->name = a->name;
        if (a->flags & NF_RECV) {
            ha->child[0] = lower_expr(l, a->child[0]);
        } else {
            ha->child[1] = lower_expr(l, a->child[1]);
            ha->child[2] = lower_expr(l, a->child[2]);
        }
        lscope_push(l);
        if (a->name) {
            ha->u.index = l->next_local;  /* slot of the arm binding */
            lscope_put(l, a->name, l->next_local++, 0);
        }
        ha->child[3] = lower_block(l, a->child[3]);
        lscope_pop(l);
        hir_add(s, ha);
    }
    if (n->child[3]) s->child[3] = lower_block(l, n->child[3]);
    return s;
}

HNode *lower_stmt(Lower *l, const Node *n) {
    switch (n->kind) {
    case NK_BLOCK:
        return lower_block(l, n);
    case NK_EXPR_STMT: {
        HNode *e = lnode(l, HK_EXPR_STMT, n->tok);
        e->child[0] = lower_expr(l, n->child[0]);
        return e;
    }
    case NK_LET:
        return lower_let(l, n);
    case NK_IF: {
        HNode *iff = lnode(l, HK_IF, n->tok);
        if (n->flags & NF_EXPR) iff->flags |= HF_EXPR;
        iff->child[0] = lower_expr(l, n->child[0]);
        if (n->child[1]) {
            lscope_push(l);
            iff->child[1] = lower_block(l, n->child[1]);
            lscope_pop(l);
        }
        if (n->child[2]) {
            lscope_push(l);
            iff->child[2] = lower_block(l, n->child[2]);
            lscope_pop(l);
        }
        return iff;
    }
    case NK_MATCH:
        return lower_match(l, n);
    case NK_WHILE: {
        HNode *w = lnode(l, HK_WHILE, n->tok);
        w->child[0] = lower_expr(l, n->child[0]);
        lscope_push(l);
        w->child[1] = lower_block(l, n->child[1]);
        lscope_pop(l);
        return w;
    }
    case NK_DOWHILE: {
        HNode *w = lnode(l, HK_DOWHILE, n->tok);
        lscope_push(l);
        w->child[0] = lower_block(l, n->child[0]);
        w->child[1] = lower_expr(l, n->child[1]);
        lscope_pop(l);
        return w;
    }
    case NK_FOR:
        return lower_for(l, n);
    case NK_RETURN: {
        HNode *r = lnode(l, HK_RETURN, n->tok);
        if (n->child[0]) r->child[0] = lower_expr(l, n->child[0]);
        return r;
    }
    case NK_BREAK: case NK_CONTINUE: {
        HNode *b = lnode(l, n->kind == NK_BREAK ? HK_BREAK : HK_CONTINUE,
                         n->tok);
        b->name = n->name;
        return b;
    }
    case NK_DEFER: {
        HNode *d = lnode(l, HK_DEFER, n->tok);
        d->child[0] = lower_stmt(l, n->child[0]);
        return d;
    }
    case NK_LABEL: {
        HNode *lab = lnode(l, HK_LABEL, n->tok);
        lab->name = n->name;
        lab->child[0] = lower_stmt(l, n->child[0]);
        return lab;
    }
    case NK_PASS:
        return NULL;
    case NK_VOLATILE:
        return lower_stmt(l, n->child[0]);
    case NK_CLOSE: {
        HNode *c = lnode(l, HK_CLOSE, n->tok);
        c->child[0] = lower_expr(l, n->child[0]);
        return c;
    }
    case NK_SELECT:
        return lower_select(l, n);
    case NK_ROUTINE: {
        HNode *r = lnode(l, HK_GO_ROUTINE, n->tok);
        lscope_push(l);
        r->child[0] = lower_block(l, n->child[0]);
        lscope_pop(l);
        return r;
    }
    case NK_ASYNC: {
        HNode *a = lnode(l, HK_ASYNC, n->tok);
        lscope_push(l);
        a->child[0] = lower_block(l, n->child[0]);
        lscope_pop(l);
        return a;
    }
    case NK_ASM: {
        /* The body is copied: the source buffer is released when the
         * analysis phase returns, long before the backend assembles
         * the statement, so a borrowed token slice would dangle. */
        HNode *a = lnode(l, HK_INLINE_ASM, n->tok);
        char *buf = malloc((size_t)n->tok.len + 1);
        memcpy(buf, n->tok.start, (size_t)n->tok.len);
        buf[n->tok.len] = '\0';
        a->u.slice.start = buf;
        a->u.slice.len = n->tok.len;
        return a;
    }
    case NK_FUNC:
        lerr(l, &n->tok, "nested functions are not supported");
        return NULL;
    default:
        lerr(l, &n->tok, "unsupported statement in this phase");
        return NULL;
    }
}
