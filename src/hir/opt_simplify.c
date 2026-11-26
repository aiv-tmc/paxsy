#include "common.h"
#include "hir.h"
#include "opt.h"

typedef struct {
    HModule *module;
    long changes;
    HNode **aggregate_init;
    unsigned char *aggregate_candidate;
    int aggregate_slots;
} SimplifyCtx;

static HNode *make_int(SimplifyCtx *ctx, HNode *ref, long long value) {
    HNode *n = hir_node(ctx->module, HK_CONST_INT, ref->tok);
    if (!n) return NULL;
    n->type = ref->type;
    n->u.ival = value;
    return n;
}

static void replace_node(SimplifyCtx *ctx, HNode *parent,
                         HNode *old_node, HNode *new_node) {
    if (!parent || !old_node || !new_node || old_node == new_node) return;
    if (opt_rewire(parent, old_node, new_node)) ctx->changes++;
}

/* Return the literal element selected by a direct struct field read. */
static HNode *sroa_selected_value(const HNode *read, const HNode *literal) {
    if (!read || !literal) return NULL;
    int index = -1;
    if (read->kind == HK_FIELD && literal->kind == HK_STRUCT_LIT) {
        index = read->u.index;
    }
    if (index < 0 || index >= literal->nitems) return NULL;
    return literal->items[index];
}

/* Collect immutable local struct literals that can be scalar-replaced.
 * Every use must be a direct known field read; taking the address, storing
 * a field, or using the aggregate as a whole rejects the transformation. */
static void sroa_collect(HNode *n, SimplifyCtx *ctx) {
    if (!n) return;
    if (n->kind == HK_LET && n->u.index >= 0 &&
        n->u.index < ctx->aggregate_slots && n->child[1] &&
        n->child[1]->kind == HK_STRUCT_LIT) {
        HNode *lit = n->child[1];
        int pure = 1;
        for (int i = 0; i < lit->nitems; ++i)
            if (!opt_is_pure(lit->items[i])) { pure = 0; break; }
        if (pure) {
            ctx->aggregate_init[n->u.index] = lit;
            ctx->aggregate_candidate[n->u.index] = 1;
        }
    }
    for (int i = 0; i < 4; ++i) sroa_collect(n->child[i], ctx);
    for (int i = 0; i < n->nitems; ++i) sroa_collect(n->items[i], ctx);
}

static void sroa_validate_uses(HNode *parent, HNode *n,
                               SimplifyCtx *ctx) {
    if (!n) return;
    if (n->kind == HK_LOCAL && n->u.index >= 0 &&
        n->u.index < ctx->aggregate_slots &&
        ctx->aggregate_candidate[n->u.index]) {
        int slot = n->u.index;
        HNode *init = ctx->aggregate_init[slot];
        if (!parent || (parent->kind != HK_FIELD && parent->kind != HK_INDEX) ||
            parent->child[0] != n || !sroa_selected_value(parent, init))
            ctx->aggregate_candidate[slot] = 0;
    }
    if ((n->kind == HK_FIELD || n->kind == HK_INDEX) && n->child[0] &&
        n->child[0]->kind == HK_LOCAL) {
        int slot = n->child[0]->u.index;
        if (slot >= 0 && slot < ctx->aggregate_slots &&
            ctx->aggregate_candidate[slot] && parent &&
            ((parent->kind == HK_STORE && parent->child[0] == n) ||
             parent->kind == HK_ADDR))
            ctx->aggregate_candidate[slot] = 0;
    }
    for (int i = 0; i < 4; ++i) sroa_validate_uses(n, n->child[i], ctx);
    for (int i = 0; i < n->nitems; ++i)
        sroa_validate_uses(n, n->items[i], ctx);
}

static void sroa_rewrite(HNode *parent, HNode *n, SimplifyCtx *ctx) {
    if (!n) return;
    if ((n->kind == HK_FIELD || n->kind == HK_INDEX) && n->child[0] &&
        n->child[0]->kind == HK_LOCAL) {
        int slot = n->child[0]->u.index;
        if (slot >= 0 && slot < ctx->aggregate_slots &&
            ctx->aggregate_candidate[slot] && parent) {
            HNode *value = sroa_selected_value(n, ctx->aggregate_init[slot]);
            if (value && opt_rewire(parent, n, value)) ctx->changes++;
            return;
        }
    }
    for (int i = 0; i < 4; ++i) sroa_rewrite(n, n->child[i], ctx);
    for (int i = 0; i < n->nitems; ++i)
        sroa_rewrite(n, n->items[i], ctx);
}

static void sroa_find_max(HNode *n, int *max_slot) {
    if (!n) return;
    if ((n->kind == HK_LOCAL || n->kind == HK_LET ||
         n->kind == HK_PARAM || n->kind == HK_FOR) &&
        n->u.index > *max_slot) *max_slot = n->u.index;
    for (int i = 0; i < 4; ++i) sroa_find_max(n->child[i], max_slot);
    for (int i = 0; i < n->nitems; ++i) sroa_find_max(n->items[i], max_slot);
}

static void sroa_run(HNode *fn, SimplifyCtx *ctx) {
    int max_slot = -1;
    sroa_find_max(fn, &max_slot);
    if (max_slot < 0 || max_slot > 1000000) return;
    ctx->aggregate_slots = max_slot + 1;
    ctx->aggregate_init = calloc((size_t)ctx->aggregate_slots,
                                 sizeof(*ctx->aggregate_init));
    ctx->aggregate_candidate = calloc((size_t)ctx->aggregate_slots, 1);
    if (!ctx->aggregate_init || !ctx->aggregate_candidate) {
        free(ctx->aggregate_init); free(ctx->aggregate_candidate);
        ctx->aggregate_init = NULL; ctx->aggregate_candidate = NULL;
        ctx->aggregate_slots = 0;
        return;
    }
    sroa_collect(fn, ctx);
    sroa_validate_uses(NULL, fn, ctx);
    sroa_rewrite(NULL, fn, ctx);
    free(ctx->aggregate_init); free(ctx->aggregate_candidate);
    ctx->aggregate_init = NULL; ctx->aggregate_candidate = NULL;
    ctx->aggregate_slots = 0;
}

static int stable_scalar_type(const HType *t) {
    return t && (t->kind == HTY_BOOL || t->kind == HTY_CHAR ||
                 t->kind == HTY_INT || t->kind == HTY_UINT ||
                 t->kind == HTY_PTR);
}

static int same_stable_value(const HNode *a, const HNode *b) {
    if (!a || !b || a->kind != b->kind || a->type != b->type ||
        !stable_scalar_type(a->type)) return 0;
    switch (a->kind) {
    case HK_LOCAL:
    case HK_PARAM:
        return a->u.index == b->u.index;
    case HK_CONST_INT:
        return a->u.ival == b->u.ival && a->type == b->type;
    case HK_CONST_CHAR:
        return a->u.slice.len == b->u.slice.len &&
               a->u.slice.start && b->u.slice.start &&
               memcmp(a->u.slice.start, b->u.slice.start,
                      (size_t)a->u.slice.len) == 0;
    default:
        return 0;
    }
}

static int all_ones_for_type(const HType *t, long long value) {
    if (value == -1) return 1;
    if (!t || (t->kind != HTY_INT && t->kind != HTY_UINT) ||
        t->width <= 0 || t->width >= 64) return 0;
    return (unsigned long long)value == ((1ULL << t->width) - 1ULL);
}

static void simplify_node(SimplifyCtx *ctx, HNode *parent, HNode *n) {
    if (!n) return;
    for (int i = 0; i < 4; ++i) simplify_node(ctx, n, n->child[i]);
    for (int i = 0; i < n->nitems; ++i) simplify_node(ctx, n, n->items[i]);

    /* Scalar-replace reads from immutable aggregate literals.  This is
     * deliberately restricted to literals whose every element is pure:
     * otherwise forwarding one field would silently discard evaluation
     * of the other fields (and potentially their side effects). */
    if (n->kind == HK_FIELD && n->child[0] &&
        n->child[0]->kind == HK_STRUCT_LIT && n->u.index >= 0) {
        HNode *literal = n->child[0];
        int pure = 1;
        for (int i = 0; i < literal->nitems; ++i)
            if (!opt_is_pure(literal->items[i])) { pure = 0; break; }
        if (pure && n->u.index < literal->nitems &&
            literal->items[n->u.index]) {
            replace_node(ctx, parent, n, literal->items[n->u.index]);
            return;
        }
    }
    /* Constant-index reads from array literals need no materialized
     * aggregate.  As above, preserve semantics by requiring all elements
     * to be side-effect free before removing the literal construction. */
    if (n->kind == HK_INDEX && n->child[0] && n->child[1] &&
        n->child[0]->kind == HK_ARRAY_LIT &&
        n->child[1]->kind == HK_CONST_INT &&
        n->child[1]->u.ival >= 0 &&
        n->child[1]->u.ival < n->child[0]->nitems) {
        HNode *literal = n->child[0];
        int pure = 1;
        for (int i = 0; i < literal->nitems; ++i)
            if (!opt_is_pure(literal->items[i])) { pure = 0; break; }
        if (pure) {
            HNode *element = literal->items[n->child[1]->u.ival];
            if (element) {
                replace_node(ctx, parent, n, element);
                return;
            }
        }
    }
    if (n->kind == HK_CAST && n->child[0] &&
        n->child[0]->type == n->type) {
        replace_node(ctx, parent, n, n->child[0]);
        return;
    }
    if (n->kind == HK_BINOP) {
        HNode *a = n->child[0], *b = n->child[1];
        if (!a || !b) return;

        if (same_stable_value(a, b)) {
            long long result;
            switch (n->u.binop) {
            case HOP_EQ: result = 1; break;
            case HOP_NE: result = 0; break;
            case HOP_LE: case HOP_GE:
                if (a->type->kind == HTY_PTR) return;
                result = 1;
                break;
            case HOP_LT: case HOP_GT:
                if (a->type->kind == HTY_PTR) return;
                result = 0;
                break;
            case HOP_SUB: case HOP_XOR:
                if (a->type->kind == HTY_PTR) return;
                result = 0;
                break;
            case HOP_AND:
                replace_node(ctx, parent, n, a);
                return;
            case HOP_LAND: case HOP_LOR:
                if (a->type->kind == HTY_BOOL)
                    replace_node(ctx, parent, n, a);
                return;
            default: return;
            }
            HNode *k = make_int(ctx, n, result);
            replace_node(ctx, parent, n, k);
            return;
        }

        if (b->kind == HK_CONST_INT) {
            long long v = b->u.ival;
            switch (n->u.binop) {
            case HOP_ADD: case HOP_SUB: case HOP_OR: case HOP_XOR:
            case HOP_SHL: case HOP_SHR: case HOP_USHR:
                if (v == 0) replace_node(ctx, parent, n, a);
                break;
            case HOP_MUL:
                if (v == 1) replace_node(ctx, parent, n, a);
                else if (v == 0 && opt_is_pure(a))
                    replace_node(ctx, parent, n, make_int(ctx, n, 0));
                break;
            case HOP_DIV:
                if (v == 1) replace_node(ctx, parent, n, a);
                break;
            case HOP_MOD:
                if (v == 1 && opt_is_pure(a))
                    replace_node(ctx, parent, n, make_int(ctx, n, 0));
                break;
            case HOP_AND:
                if (v == 0 && opt_is_pure(a))
                    replace_node(ctx, parent, n, make_int(ctx, n, 0));
                else if (all_ones_for_type(n->type, v))
                    replace_node(ctx, parent, n, a);
                break;
            case HOP_LAND:
                if (v != 0 && a->type && a->type->kind == HTY_BOOL)
                    replace_node(ctx, parent, n, a);
                else if (v == 0 && opt_is_pure(a))
                    replace_node(ctx, parent, n, make_int(ctx, n, 0));
                break;
            case HOP_LOR:
                if (v == 0 && a->type && a->type->kind == HTY_BOOL)
                    replace_node(ctx, parent, n, a);
                else if (v != 0 && opt_is_pure(a))
                    replace_node(ctx, parent, n, make_int(ctx, n, 1));
                break;
            default:
                break;
            }
        } else if (a->kind == HK_CONST_INT &&
                   (n->u.binop == HOP_LAND || n->u.binop == HOP_LOR) &&
                   n->type && n->type->kind == HTY_BOOL) {
            /* The left operand controls short-circuit evaluation. Replacing
             * false && rhs / true || rhs is always safe because rhs is not
             * evaluated; the other two identities retain rhs evaluation. */
            long long v = a->u.ival;
            if (n->u.binop == HOP_LAND) {
                if (v == 0)
                    replace_node(ctx, parent, n, make_int(ctx, n, 0));
                else
                    replace_node(ctx, parent, n, b);
            } else {
                if (v != 0)
                    replace_node(ctx, parent, n, make_int(ctx, n, 1));
                else
                    replace_node(ctx, parent, n, b);
            }
        } else if ((n->u.binop == HOP_ADD || n->u.binop == HOP_MUL ||
                    n->u.binop == HOP_AND || n->u.binop == HOP_OR ||
                    n->u.binop == HOP_XOR) &&
                   a->kind == HK_CONST_INT) {
            long long v = a->u.ival;
            if ((n->u.binop == HOP_ADD || n->u.binop == HOP_OR ||
                 n->u.binop == HOP_XOR) && v == 0)
                replace_node(ctx, parent, n, b);
            else if (n->u.binop == HOP_MUL && v == 1)
                replace_node(ctx, parent, n, b);
            else if (n->u.binop == HOP_MUL && v == 0 && opt_is_pure(b))
                replace_node(ctx, parent, n, make_int(ctx, n, 0));
            else if (n->u.binop == HOP_AND && v == 0 && opt_is_pure(b))
                replace_node(ctx, parent, n, make_int(ctx, n, 0));
            else if (n->u.binop == HOP_AND && all_ones_for_type(n->type, v))
                replace_node(ctx, parent, n, b);
        }
    }
}

long opt_sroa(HModule *m) {
    HNode *root = hir_root(m);
    if (!root) return 0;
    long changes = 0;
    for (int i = 0; i < root->nitems; ++i) {
        HNode *fn = root->items[i];
        if (!fn || fn->kind != HK_FUNC) continue;
        SimplifyCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.module = m;
        sroa_run(fn, &ctx);
        changes += ctx.changes;
    }
    return changes;
}

long opt_simplify(HModule *m) {
    HNode *root = hir_root(m);
    if (!root) return 0;
    SimplifyCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.module = m;
    for (int i = 0; i < root->nitems; ++i) {
        HNode *fn = root->items[i];
        if (!fn || fn->kind != HK_FUNC) continue;
        for (int c = 0; c < 4; ++c) simplify_node(&ctx, fn, fn->child[c]);
        for (int c = 0; c < fn->nitems; ++c)
            simplify_node(&ctx, fn, fn->items[c]);
    }
    return ctx.changes;
}
