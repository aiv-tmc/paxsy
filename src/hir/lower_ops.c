#include "common.h"
#include "lower_int.h"

/*
 * lower_ops.c - the operator tables shared by the lowering passes.
 * Token kinds map to HIR operators through two small static tables
 * that live here so the driver file stays focused on context and
 * declaration walking. The compound-assignment tokens form one
 * contiguous enum range, so the base operator is an indexed lookup in
 * a parallel table.
 */

/* Binary operator token kinds in one table, scanned linearly. */
static const struct { short tk, hop; } kBinOps[] = {
    { TK_PLUS, HOP_ADD },    { TK_MINUS, HOP_SUB },
    { TK_STAR, HOP_MUL },    { TK_SLASH, HOP_DIV },
    { TK_PERCENT, HOP_MOD }, { TK_AMP, HOP_AND },
    { TK_DOLLAR, HOP_OR },   { TK_CARET, HOP_XOR },
    { TK_SHL, HOP_SHL },     { TK_SHR, HOP_SHR },
    { TK_USHR, HOP_USHR },
    { TK_EQ, HOP_EQ },       { TK_NE, HOP_NE },
    { TK_LT, HOP_LT },       { TK_LE, HOP_LE },
    { TK_GT, HOP_GT },       { TK_GE, HOP_GE },
    { TK_KW_AND, HOP_LAND }, { TK_KW_OR, HOP_LOR },
};

/* Maps a binary token kind to its HIR operator, or -1 when the
 * token is not a binary operator. */
HBinOp l_binop_map(int op) {
    for (size_t i = 0; i < sizeof(kBinOps) / sizeof(kBinOps[0]); i++)
        if (kBinOps[i].tk == op) return (HBinOp)kBinOps[i].hop;
    return (HBinOp)-1;
}

/* Compound-assignment tokens form one contiguous enum range: the base
 * operator is an indexed lookup in a parallel table. */
static const int kCompoundBase[] = {
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_AMP, TK_DOLLAR, TK_CARET, TK_SHL, TK_SHR, TK_USHR,
};

/* True for TK_PLUS_EQ .. TK_USHR_EQ. */
int l_is_compound_assign(int op) {
    return op >= TK_PLUS_EQ && op <= TK_USHR_EQ;
}

/* Returns the base (non-assign) token of a compound assignment, or
 * TK_ASSIGN for a plain assignment. */
int l_compound_base(int op) {
    if (l_is_compound_assign(op)) return kCompoundBase[op - TK_PLUS_EQ];
    return TK_ASSIGN;
}
