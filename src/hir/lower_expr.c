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


/* Layout of scalar and pointer types is target-defined here (the
 * currently emitted native target is x86-64). Aggregate layout is
 * handled by the aggregate lowering path, not guessed here. */
static int lower_builtin_layout(const HType *type, int alignment,
                                unsigned long long *result) {
    unsigned long long size = 0;
    switch (type->kind) {
    case HTY_VOID: size = 0; break;
    case HTY_BOOL: case HTY_CHAR: size = 1; break;
    case HTY_INT: case HTY_UINT: case HTY_REAL:
        size = (unsigned long long)type->width / 8;
        break;
    case HTY_PTR: case HTY_CHAN: size = 8; break;
    case HTY_VIEW: size = 16; break;
    case HTY_ARRAY: {
        unsigned long long elem = 0;
        if (!lower_builtin_layout(type->u.array.elem, 0, &elem)) return 0;
        if (type->u.array.count < 0 ||
            (type->u.array.count != 0 &&
             elem > 18446744073709551615ULL /
                    (unsigned long long)type->u.array.count)) return 0;
        size = elem * (unsigned long long)type->u.array.count;
        break;
    }
    default: return 0;
    }
    if (alignment) {
        if (type->kind == HTY_VOID) size = 1;
        else if (type->kind == HTY_VIEW) size = 8;
        else if (type->kind == HTY_ARRAY) {
            if (!lower_builtin_layout(type->u.array.elem, 1, &size)) return 0;
        } else if (size > 8) size = 8;
    }
    *result = size;
    return 1;
}

static HNode *lower_layout_builtin(Lower *l, const Node *call) {
    const Node *callee = call->child[0];
    if (!callee || callee->kind != NK_NAME || call->item_count != 1)
        return NULL;
    int alignment = strcmp(callee->name, "alignof") == 0;
    if (!alignment && strcmp(callee->name, "sizeof") != 0) return NULL;
    const Node *arg = call->items[0];
    if (!arg || arg->kind != NK_NAME) {
        lerr(l, &call->tok, "sizeof and alignof require a type name");
        return lnode(l, HK_CONST_INT, call->tok);
    }
    Node type_node = *arg;
    type_node.kind = NK_TYPE_NAME;
    const HType *type = lower_type(l, &type_node);
    unsigned long long value = 0;
    if (!lower_builtin_layout(type, alignment, &value)) {
        lerr(l, &arg->tok,
             "sizeof/alignof currently require a scalar, pointer, view, or fixed array type");
        return lnode(l, HK_CONST_INT, call->tok);
    }
    HNode *constant = lnode(l, HK_CONST_INT, call->tok);
    constant->u.ival = (long long)value;
    constant->type = hty_uint(64);
    return constant;
}

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

/* Struct literals: positional items stay in order; named items keep
 * their field name so the typing pass can bind each one to its slot
 * (u.index) against the Struct declaration. Unnamed slots are
 * zero-filled by the backend. */
static HNode *lower_struct_lit(Lower *l, const Node *n) {
    HNode *s = lnode(l, HK_STRUCT_LIT, n->tok);
    s->name = lglobal_name(l, n->name);
    for (int i = 0; i < n->item_count; i++) {
        Node *it = n->items[i];
        /* value nodes keep their own names; named bindings are
         * resolved to field positions below */
        hir_add(s, lower_expr(l, it->kind == NK_NAMED_INIT ? it->child[0] : it));
    }
    /* Bind named items to field positions against the struct
     * declaration, keeping unset fields as NULL gaps (zero-filled by
     * the backend). Positional items fill the remaining slots in
     * order. */
    const char *qname = s->name ? s->name : "";
    const HNode *agg = NULL;
    HNode *root = l->mod ? hir_root(l->mod) : NULL;
    for (int i = 0; root && i < root->nitems; i++) {
        if (root->items[i]->name &&
            strcmp(root->items[i]->name, qname) == 0) {
            const HNode *gn = root->items[i];
            /* static aggregates are stored directly (a struct/union/
             * enum node named after the binding); plain globals wrap
             * the value in child[0]. */
            if (gn->kind == HK_STRUCT || gn->kind == HK_UNION ||
                gn->kind == HK_ENUM)
                agg = gn;
            else
                agg = gn->child[0];
            break;
        }
    }
    if (agg && agg->kind == HK_STRUCT && agg->nitems > 0) {
        int slots = agg->nitems > s->nitems ? agg->nitems : s->nitems;
        HNode **ord = calloc((size_t)slots, sizeof(HNode *));
        int next = 0, any_named = 0;
        for (int i = 0; i < s->nitems; i++) {
            HNode *hv = s->items[i];
            const Node *c = n->items[i];
            const char *dot = c->kind == NK_NAMED_INIT ? c->name : NULL;
            if (!dot) {
                while (next < slots && ord[next]) next++;
                if (next < slots) ord[next++] = hv;
                continue;
            }
            any_named = 1;
            long pos = -1;
            for (long k = 0; k < agg->nitems; k++)
                if (agg->items[k]->name &&
                    strcmp(agg->items[k]->name, dot) == 0) { pos = k; break; }
            if (pos >= 0) ord[pos] = hv;
            else {
                while (next < slots && ord[next]) next++;
                if (next < slots) ord[next++] = hv;
            }
        }
        if (any_named) {
            free(s->items);
            s->items = ord;
            s->nitems = slots;
        } else {
            free(ord);
        }
    }
    return s;
}

HNode *lower_expr(Lower *l, const Node *n) {
    switch (n->kind) {
    case NK_FMT:
        return lower_fmt(l, n);
    case NK_LITERAL:
        switch (n->tok.kind) {
        case TK_INT_LIT: {
            HNode *c = lnode(l, HK_CONST_INT, n->tok);
            c->u.ival = lint_value(n->tok.start, n->tok.len);
            return c;
        }
        case TK_KW_TRUE: case TK_KW_FALSE: {
            /* The machine representation is the immediate 1/0; the
             * preset Bool type keeps assignments and the textual
             * backends honest. */
            HNode *c = lnode(l, HK_CONST_INT, n->tok);
            c->u.ival = n->tok.kind == TK_KW_TRUE ? 1 : 0;
            c->type = &hty_bool;
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
            /* The lexer slice holds the literal content only (both
             * quotes excluded, escape sequences left raw). */
            c->u.slice.start = n->tok.start;
            c->u.slice.len = n->tok.len;
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
        if (k == HK_FREE) {
            /* The native `free p` operator is lowered to the standard
             * heap release routine. Source discovery injects the heap
             * module dependency whenever this operator appears, so the
             * user does not need an explicit `use std::heap`. */
            HNode *c = lnode(l, HK_CALL, n->tok);
            HNode *callee = lnode(l, HK_GLOBAL_REF, n->tok);
            callee->name = lglobal_name(l, "std::heap::free");
            {
                /* std::heap: free(ptr: &Void): Void */
                const HType **pr = calloc(1, sizeof(HType *));
                pr[0] = hty_ptr(&hty_void);
                callee->type = hty_func(pr, 1, &hty_void);
            }
            c->child[0] = callee;
            hir_add(c, lower_expr(l, n->child[0]));
            return c;
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
        HNode *layout = lower_layout_builtin(l, n);
        if (layout) return layout;
        HNode *generic = lower_generic_call(l, n);
        if (generic) return generic;
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
        /* `alloc T` and `alloc(size)` are native language forms. The
         * frontend lowers them to the standard heap allocator; source
         * discovery supplies that implementation automatically when
         * the operator is used, without requiring a user import. */
        HNode *c = lnode(l, HK_CALL, n->tok);
        HNode *callee = lnode(l, HK_GLOBAL_REF, n->tok);
        callee->name = lglobal_name(l, "std::heap::alloc");
        {
            /* std::heap: alloc(size: UInt64): &Void! */
            const HType **pr = calloc(1, sizeof(HType *));
            pr[0] = hty_uint(64);
            callee->type = hty_func(pr, 1,
                                    hty_error(hty_ptr(&hty_void)));
        }
        c->child[0] = callee;
        if (n->flags & NF_TYPE_OPERAND) {
            const HType *t = lower_type(l, n->child[0]);
            HNode *sz = lnode(l, HK_CONST_INT, n->tok);
            {
                long bytes = hty_size_bytes(t);
                if (bytes < 0) lerr(l, &n->tok, "alloc of a named type is not supported");
                sz->u.ival = bytes;
            }
            hir_add(c, sz);
            /* alloc T[n] yields a pointer to the element type */
            c->type = hty_ptr(t->kind == HTY_ARRAY ? t->u.array.elem : t);
        } else {
            /* alloc(size): the element type is not statically known */
            hir_add(c, lower_expr(l, n->child[0]));
            c->type = hty_ptr(&hty_void);
        }
        return c;
    }
    case NK_CATCH: {
        HNode *c = lnode(l, HK_CATCH, n->tok);
        c->name = n->name;
        c->child[0] = lower_expr(l, n->child[0]);
        lscope_push(l);
        /* The native selector stores the caught error code in the
         * catch binding's local slot. Keep that slot on the HIR node
         * as well as in the lexical scope; otherwise the selector
         * writes slot zero while reads use the actual binding slot. */
        c->u.index = -1;
        if (n->name) {
            c->u.index = l->next_local++;
            lscope_put(l, n->name, c->u.index, 0);
        }
        c->child[1] = lower_block(l, n->child[1]);
        lscope_pop(l);
        if (n->flags & NF_EXPR) {
            /* value catch: the handler block yields the result */
            HNode *blk = c->child[1];
            if (blk && blk->kind == HK_BLOCK && blk->nitems > 0)
                blk->items[blk->nitems - 1]->flags |= HF_ARMVAL;
        }
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
