#include "common.h"
#include "lower_int.h"

/*
 * lower_expr.c - expression lowering: literals, names, operators,
 * calls, containers, channels and the try/catch error forms.
 *
 * Assignments become HK_STORE nodes; compound assignments expand to
 * the explicit read-modify-write binop so no backend needs to know
 * the compound forms. Name resolution prefers innermost bindings and
 * falls back to a module-global reference, which the typing pass then
 * binds to a declaration or leaves external.
 */

static HNode *lower_assign(Lower *l, const Node *n) {
    HNode *lv = lower_expr(l, n->child[0]);
    if (n->op == TK_ASSIGN) {
        HNode *s = lnode(l, HK_STORE, n->tok);
        s->child[0] = lv;
        s->child[1] = lower_expr(l, n->child[1]);
        return s;
    }
    /* Compound: lv = lv <base-op> rhs; the store's l-value is a
     * second node so backends never reuse one value node twice. */
    HNode *b = lnode(l, HK_BINOP, n->tok);
    b->u.binop = l_binop_map(l_compound_base(n->op));
    b->child[0] = lv;
    b->child[1] = lower_expr(l, n->child[1]);
    HNode *s = lnode(l, HK_STORE, n->tok);
    s->child[0] = lower_expr(l, n->child[0]);
    s->child[1] = b;
    return s;
}

/* Struct literals are positional. Named initializers name fields the
 * layout pass has not resolved yet, so they are diagnosed rather
 * than silently reordered. */
static HNode *lower_struct_lit(Lower *l, const Node *n) {
    for (int i = 0; i < n->item_count; i++)
        if (n->items[i]->kind == NK_NAMED_INIT) {
            lerr(l, &n->items[i]->tok,
                 "named initializers need the layout pass");
            return NULL;
        }
    HNode *s = lnode(l, HK_STRUCT_LIT, n->tok);
    s->name = lglobal_name(l, n->name);
    for (int i = 0; i < n->item_count; i++)
        hir_add(s, lower_expr(l, n->items[i]));
    return s;
}

HNode *lower_expr(Lower *l, const Node *n) {
    switch (n->kind) {
    case NK_LITERAL:
        switch (n->tok.kind) {
        case TK_INT_LIT: {
            HNode *c = lnode(l, HK_CONST_INT, n->tok);
            c->u.ival = lint_value(n->tok.start, n->tok.len);
            return c;
        }
        case TK_FLOAT_LIT: {
            HNode *c = lnode(l, HK_CONST_FLOAT, n->tok);
            char buf[64];
            int len = n->tok.len < (int)sizeof(buf) - 1 ? n->tok.len
                                                        : (int)sizeof(buf) - 1;
            memcpy(buf, n->tok.start, (size_t)len);
            buf[len] = '\0';
            c->u.fval = strtod(buf, NULL);
            return c;
        }
        case TK_STRING_LIT:
            return lower_string(l, n);
        case TK_CHAR_LIT: {
            HNode *c = lnode(l, HK_CONST_CHAR, n->tok);
            c->u.slice.start = n->tok.start + 1;      /* strip quotes */
            c->u.slice.len = n->tok.len - 2;
            return c;
        }
        default:
            goto unsupported;
        }
        break;   /* unreachable; literals above always return or goto */
    case NK_NAME: {
        int is_param = 0;
        int slot = lscope_find(l, n->name, &is_param);
        HNode *r = lnode(l, slot >= 0 ? (is_param ? HK_PARAM : HK_LOCAL)
                                      : HK_GLOBAL_REF, n->tok);
        if (slot >= 0) r->u.index = slot;
        else r->name = lglobal_name(l, n->name);
        return r;
    }
    case NK_BINARY:
        if (n->op == TK_ASSIGN || l_is_compound_assign(n->op))
            return lower_assign(l, n);
        if (l_binop_map(n->op) == (HBinOp)-1) break;
        {
            HNode *b = lnode(l, HK_BINOP, n->tok);
            b->u.binop = l_binop_map(n->op);
            b->child[0] = lower_expr(l, n->child[0]);
            b->child[1] = lower_expr(l, n->child[1]);
            return b;
        }
    case NK_UNARY: {
        HNodeKind k;
        HUnOp uo;
        switch (n->op) {
        case TK_MINUS:     k = HK_UNOP;    uo = HUN_NEG;     break;
        case TK_TILDE:     k = HK_UNOP;    uo = HUN_BITNOT;  break;
        case TK_KW_NOT:    k = HK_UNOP;    uo = HUN_LNOT;    break;
        case TK_AMP:       k = HK_UNOP;    uo = HOP_ADDR_OF; break;
        case TK_AT:        k = HK_UNOP;    uo = HUN_DEREF;   break;
        case TK_KW_TRY:    k = HK_TRY;     uo = HUN_NEG;     break;
        case TK_KW_FREE:   k = HK_FREE;    uo = HUN_NEG;     break;
        case TK_KW_AWAIT:  k = HK_AWAIT;   uo = HUN_NEG;     break;
        case TK_KW_ERROR:  k = HK_ERR_VAL; uo = HUN_NEG;     break;
        default: goto unsupported;
        }
        HNode *u = lnode(l, k, n->tok);
        if (k == HK_UNOP) u->u.unop = uo;
        u->child[0] = lower_expr(l, n->child[0]);
        return u;
    }
    case NK_CAST: {
        HNode *c = lnode(l, HK_CAST, n->tok);
        c->type = lower_type(l, n->child[1]);
        c->child[0] = lower_expr(l, n->child[0]);
        return c;
    }
    case NK_CALL: {
        HNode *c = lnode(l, HK_CALL, n->tok);
        c->child[0] = lower_expr(l, n->child[0]);
        for (int i = 0; i < n->item_count; i++)
            hir_add(c, lower_expr(l, n->items[i]));
        return c;
    }
    case NK_INDEX: {
        HNode *ix = lnode(l, HK_INDEX, n->tok);
        ix->child[0] = lower_expr(l, n->child[0]);
        if (n->child[1]) ix->child[1] = lower_expr(l, n->child[1]);
        return ix;
    }
    case NK_SLICE: {
        HNode *s = lnode(l, HK_SLICE, n->tok);
        if (n->flags & NF_EXCLUSIVE) s->flags |= HF_EXCLUSIVE;
        s->child[0] = lower_expr(l, n->child[0]);
        if (n->child[1]) s->child[1] = lower_expr(l, n->child[1]);
        if (n->child[2]) s->child[2] = lower_expr(l, n->child[2]);
        return s;
    }
    case NK_FIELD_ACCESS: {
        HNode *f = lnode(l, HK_FIELD, n->tok);
        f->name = n->name;
        f->u.index = -1;      /* typing pass resolves the field slot  */
        f->child[0] = lower_expr(l, n->child[0]);
        return f;
    }
    case NK_ARRAY_LIT: {
        HNode *a = lnode(l, HK_ARRAY_LIT, n->tok);
        for (int i = 0; i < n->item_count; i++)
            hir_add(a, lower_expr(l, n->items[i]));
        return a;
    }
    case NK_STRUCT_LIT:
        return lower_struct_lit(l, n);
    case NK_IF: case NK_MATCH:
        /* if/match expressions reuse the statement lowering; the
         * resulting node is an expression by construction. */
        return lower_stmt(l, n);
    case NK_ASYNC: {
        HNode *a = lnode(l, HK_ASYNC, n->tok);
        if (n->child[0]->kind == NK_BLOCK) {
            lscope_push(l);
            a->child[0] = lower_block(l, n->child[0]);
            lscope_pop(l);
        } else {
            a->child[0] = lower_expr(l, n->child[0]);
        }
        return a;
    }
    case NK_SEND: {
        HNode *s = lnode(l, HK_SEND, n->tok);
        s->child[0] = lower_expr(l, n->child[0]);
        s->child[1] = lower_expr(l, n->child[1]);
        return s;
    }
    case NK_RECV: {
        HNode *r = lnode(l, HK_RECV, n->tok);
        r->child[0] = lower_expr(l, n->child[0]);
        return r;
    }
    case NK_CHAN_NEW: {
        HNode *c = lnode(l, HK_NEW_CHAN, n->tok);
        c->type = hty_chan(hty_int(64));   /* runtime slot channel */
        if (n->child[0]) {
            c->child[0] = lnode(l, HK_CONST_INT, n->child[0]->tok);
            c->child[0]->u.ival = lint_value(n->child[0]->tok.start,
                                             n->child[0]->tok.len);
        }
        return c;
    }
    case NK_ALLOC: {
        HNode *a = lnode(l, HK_ALLOC, n->tok);
        const HType *t = lower_type(l, n->child[0]);
        a->type = hty_ptr(t);
        return a;
    }
    case NK_CATCH: {
        HNode *c = lnode(l, HK_CATCH, n->tok);
        c->name = n->name;
        c->child[0] = lower_expr(l, n->child[0]);
        lscope_push(l);
        lscope_put(l, n->name, l->next_local++, 0);
        c->child[1] = lower_block(l, n->child[1]);
        lscope_pop(l);
        return c;
    }
    default:
        break;
    }
    goto unsupported;
unsupported:
    lerr(l, &n->tok, "unsupported expression in this phase");
    return NULL;
}
