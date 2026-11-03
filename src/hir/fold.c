#include "common.h"
#include "tyinfer_int.h"

/*
 * fold.c - integer constant folding on typed HIR.
 *
 * tfold_expr() inspects one expression node whose operands the typing
 * pass has already typed and, when both operands are integer
 * constants, replaces the node in place with its result. In-place
 * replacement keeps every parent pointer valid: no rewrite pass, no
 * fixup lists. The node's own type (assigned by the typing pass just
 * before the fold attempt) survives the replacement, so a comparison
 * folded to 0/1 keeps its Bool type while an arithmetic fold keeps
 * the widened operand type.
 *
 * Folding uses wrapping unsigned arithmetic for add/sub/mul/bitops
 * and never folds division or remainder by zero. Floats, strings and
 * every non-arithmetic construct are left untouched: folding covers
 * exactly the operations the simulation step of the GPU offload
 * analysis must agree on, so the CPU and the simulated GPU always
 * compute from the same reduced tree.
 */

/* Evaluates one binop on two integer constants with wrapping
 * arithmetic; returns 1 when a definite result exists (never for
 * division or remainder by zero). Comparisons and logic yield 0/1. */
static int ffold_bin(HBinOp op, long long a, long long b, long long *out) {
    unsigned long long ua = (unsigned long long)a, ub = (unsigned long long)b;
    switch (op) {
    case HOP_ADD: *out = (long long)(ua + ub); return 1;
    case HOP_SUB: *out = (long long)(ua - ub); return 1;
    case HOP_MUL: *out = (long long)(ua * ub); return 1;
    case HOP_AND: *out = (long long)(ua & ub); return 1;
    case HOP_OR:  *out = (long long)(ua | ub); return 1;
    case HOP_XOR: *out = (long long)(ua ^ ub); return 1;
    case HOP_SHL: *out = (long long)(ua << (ub & 63)); return 1;
    case HOP_SHR: *out = (long long)((long long)ua >> (ub & 63)); return 1;
    case HOP_USHR:*out = (long long)(ua >> (ub & 63)); return 1;
    case HOP_DIV: case HOP_MOD:
        if (b == 0) return 0;
        *out = op == HOP_DIV ? a / b : a % b;
        return 1;
    case HOP_EQ: *out = a == b; return 1;
    case HOP_NE: *out = a != b; return 1;
    case HOP_LT: *out = a < b;  return 1;
    case HOP_LE: *out = a <= b; return 1;
    case HOP_GT: *out = a > b;  return 1;
    case HOP_GE: *out = a >= b; return 1;
    case HOP_LAND: *out = a != 0 && b != 0; return 1;
    case HOP_LOR:  *out = a != 0 || b != 0; return 1;
    }
    return 0;
}

/* Replaces a folded node with its constant result in place. */
static void fset_int(HNode *n, long long v) {
    n->kind = HK_CONST_INT;
    n->u.ival = v;
    n->flags &= ~(HF_SEND_RECV | HF_ALLOC | HF_DEFER | HF_SPAWN |
                  HF_UNKNOWN_CALL);
    n->child[0] = n->child[1] = NULL;
}

/* Attempts to fold one node; returns 1 when the node is now a
 * constant. Only integer-typed binops and the negation/bitnot/lognot
 * unops qualify; dereference and address-of never fold. */
int tfold_expr(HNode *n) {
    if (!n) return 0;
    if (n->kind == HK_BINOP &&
        n->child[0] && n->child[1] &&
        n->child[0]->kind == HK_CONST_INT &&
        n->child[1]->kind == HK_CONST_INT) {
        long long r;
        if (!ffold_bin(n->u.binop, n->child[0]->u.ival,
                       n->child[1]->u.ival, &r))
            return 0;
        fset_int(n, r);
        return 1;
    }
    if (n->kind == HK_UNOP && n->child[0] &&
        n->child[0]->kind == HK_CONST_INT &&
        n->u.unop != HUN_DEREF && n->u.unop != HOP_ADDR_OF) {
        long long v = n->child[0]->u.ival, r;
        if (n->u.unop == HUN_NEG)
            r = (long long)(0ULL - (unsigned long long)v);
        else if (n->u.unop == HUN_BITNOT)
            r = ~v;
        else
            r = v == 0;              /* LNOT */
        fset_int(n, r);
        return 1;
    }
    return 0;
}
