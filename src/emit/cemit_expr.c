#include "common.h"
#include "cemit_int.h"

/*
 * cemit_expr.c - the C type printer and the C value-expression
 * printer.
 *
 * cty() maps every HType onto a C99 declarator: fixed-width scalars
 * use stdint types, Bool maps to int (C comparisons already yield
 * int 0/1), named aggregates reference their struct tag, channels
 * are opaque px_chan pointers, and arrays decay to element pointers
 * (array bounds travel with the declarations that carry a name: let,
 * globals and struct fields print "<elem> <name>[<count>]").
 * 128-bit integers have no stdint spelling, so they use the px_i128
 * / px_u128 typedefs emitted with the aggregates; arithmetic on them
 * is not generated and the C compiler of the emitted unit reports
 * any use.
 *
 * cexpr() prints a value in expression position. Everything the C
 * backend cannot express in that position (channel receives, try,
 * catch, branch values, spawns) is reported through cerr_: the pass
 * contract is to diagnose, never to silently drop or miscompile.
 * std::inout::println dispatches to a per-type shim emitted on
 * demand, so the unit links without the standard library sources.
 *
 * The address-of operator answers two HIR spellings - the dedicated
 * HK_ADDR node and the unary-op form HK_UNOP with HOP_ADDR_OF - and
 * both go through the same array-to-pointer decay rule, so either
 * lowering convention emits working C.
 */

/* Literal and operator printers shared with the statement printer;
 * defined in cemit_lit.c. */
void cint_lit(CE *c, long long v);
void cfloat_lit(CE *c, double v);
void cbinop(CE *c, HBinOp op);

void cty(CE *c, const HType *t) {
    if (!t) { fputs("long long", c->out); return; }
    switch (t->kind) {
    case HTY_VOID:  fputs("void", c->out); break;
    case HTY_BOOL:  fputs("int", c->out); break;
    case HTY_CHAR:  fputs("char", c->out); break;
    case HTY_INT:
        switch (t->width) {
        case 8:  fputs("int8_t", c->out);   break;
        case 16: fputs("int16_t", c->out);  break;
        case 32: fputs("int32_t", c->out);  break;
        case 64: fputs("int64_t", c->out);  break;
        default: fputs("px_i128", c->out);  break;
        }
        break;
    case HTY_UINT:
        switch (t->width) {
        case 8:  fputs("uint8_t", c->out);  break;
        case 16: fputs("uint16_t", c->out); break;
        case 32: fputs("uint32_t", c->out); break;
        case 64: fputs("uint64_t", c->out); break;
        default: fputs("px_u128", c->out);  break;
        }
        break;
    case HTY_REAL:
        fputs(t->width == 32 ? "float" : "double", c->out);
        break;
    case HTY_PTR:
        cty(c, t->u.ptr.elem);
        fputs(" *", c->out);
        break;
    case HTY_ARRAY:
        cty(c, t->u.array.elem);   /* arrays decay to element pointers */
        fputs(" *", c->out);
        break;
    case HTY_NAMED: {
        char nm[80];
        cname(t->u.named.name, nm, sizeof(nm));
        fprintf(c->out, "struct px_%s", nm);
        break;
    }
    case HTY_CHAN:  fputs("px_chan *", c->out); break;
    default:
        fputs("long long", c->out);
        break;
    }
}

void cexpr(CE *c, const HNode *n) {
    if (!n) { fputs("0", c->out); return; }
    switch (n->kind) {
    case HK_CONST_INT:
        cint_lit(c, n->u.ival);
        break;
    case HK_CONST_FLOAT:
        cfloat_lit(c, n->u.fval);
        break;
    case HK_CONST_STRING:
        if (n->aux < 0) { cerr_(c, n, "string constant without an id"); break; }
        fprintf(c->out, "px_s%d", n->aux);
        break;
    case HK_CONST_CHAR:
        if (n->aux < 0) { cerr_(c, n, "char constant without an id"); break; }
        fprintf(c->out, "(int)(unsigned char)px_s%d[0]", n->aux);
        break;
    case HK_PARAM: case HK_LOCAL: {
        char b[24];
        /* inside CUDA kernels the loop variable slot prints as the
         * thread index name */
        if (n->u.index == c->alias_slot && c->alias_name) {
            fputs(c->alias_name, c->out);
            break;
        }
        cslot(b, n->kind == HK_PARAM, n->u.index);
        fputs(b, c->out);
        break;
    }
    case HK_GLOBAL_REF: {
        char nm[96];
        cname(n->name, nm, sizeof(nm));
        fputs(nm, c->out);
        break;
    }
    case HK_BINOP:
        if (n->u.binop == HOP_USHR) {
            fputs("(long long)px_ushr((unsigned long long)(", c->out);
            cexpr(c, n->child[0]);
            fputs("), (", c->out);
            cexpr(c, n->child[1]);
            fputs("))", c->out);
            break;
        }
        fputs("((", c->out);
        cexpr(c, n->child[0]);
        fputs(") ", c->out);
        cbinop(c, n->u.binop);
        fputs(" (", c->out);
        cexpr(c, n->child[1]);
        fputs("))", c->out);
        break;
    case HK_UNOP: {
        const char *sym = n->u.unop == HUN_NEG ? "-" :
                          n->u.unop == HUN_BITNOT ? "~" :
                          n->u.unop == HUN_LNOT ? "!" :
                          n->u.unop == HOP_ADDR_OF ? "&" : "*";
        /* The address of an array decays to a pointer to its first
         * element through the array name alone; a written '&' would
         * bind to the whole array and mismatch pointer parameters.
         * The same rule answers both spellings of the address-of
         * operator: the dedicated HK_ADDR node and the unary-op
         * form, so both lowering conventions emit working C. */
        if (n->u.unop == HOP_ADDR_OF && n->child[0] &&
            n->child[0]->type && n->child[0]->type->kind == HTY_ARRAY) {
            cexpr(c, n->child[0]);
            break;
        }
        fputs("(", c->out);
        fputs(sym, c->out);
        cexpr(c, n->child[0]);
        fputs(")", c->out);
        break;
    }
    case HK_CALL: {
        const HNode *fn = n->child[0];
        if (fn->kind != HK_GLOBAL_REF) {
            cerr_(c, n, "method calls need the dispatch pass");
            break;
        }
        cexpr(c, n->child[0]);
        fputs("(", c->out);
        for (int i = 0; i < n->nitems; i++) {
            if (i) fputs(", ", c->out);
            cexpr(c, n->items[i]);
        }
        fputs(")", c->out);
        break;
    }
    case HK_CAST:
        fputs("((", c->out);
        cty(c, n->type);
        fputs(")(", c->out);
        cexpr(c, n->child[0]);
        fputs("))", c->out);
        break;
    case HK_INDEX:
        fputs("(", c->out);
        cexpr(c, n->child[0]);
        fputs(")[", c->out);
        cexpr(c, n->child[1]);
        fputs("]", c->out);
        break;
    case HK_FIELD: {
        char nm[96];
        if (n->u.index < 0) {
            cerr_(c, n, "unresolved struct field");
            break;
        }
        cname(n->name, nm, sizeof(nm));   /* documents the field     */
        fputs("(", c->out);
        cexpr(c, n->child[0]);
        fprintf(c->out, ").f%d /* %s */", n->u.index, nm);
        break;
    }
    case HK_STRUCT_LIT: {
        char nm[96];
        cname(n->name, nm, sizeof(nm));
        fprintf(c->out, "((struct px_%s){ ", nm);
        for (int i = 0; i < n->nitems; i++) {
            if (i) fputs(", ", c->out);
            cexpr(c, n->items[i]);
        }
        fputs(" })", c->out);
        break;
    }
    case HK_ARRAY_LIT: {
        const HType *t = n->type;
        fputs("((", c->out);
        if (t && t->kind == HTY_ARRAY) {
            cty(c, t->u.array.elem);
            fprintf(c->out, " [%ld]){ ", t->u.array.count);
        } else {
            fputs("long long [1]){ ", c->out);
        }
        for (int i = 0; i < n->nitems; i++) {
            if (i) fputs(", ", c->out);
            cexpr(c, n->items[i]);
        }
        fputs(" })", c->out);
        break;
    }
    case HK_ALLOC: {
        fputs("((", c->out);
        cty(c, n->type);
        fputs(")malloc(sizeof(", c->out);
        if (n->type && n->type->kind == HTY_PTR)
            cty(c, n->type->u.ptr.elem);
        else
            fputs("long long", c->out);
        fputs(")))", c->out);
        break;
    }
    case HK_NEW_CHAN:
        fputs("px_chan_new(", c->out);
        if (n->child[0]) cexpr(c, n->child[0]);
        else fputs("0", c->out);
        fputs(")", c->out);
        break;
    case HK_DEREF:
        fputs("(*(", c->out);
        cexpr(c, n->child[0]);
        fputs("))", c->out);
        break;
    case HK_ADDR: {
        /* Same decay rule as the unary spelling above: the address of
         * an array is the address of its first element, which C
         * produces from the array name alone. */
        HNode *base = n->child[0];
        if (base && base->type && base->type->kind == HTY_ARRAY) {
            cexpr(c, base);
        } else {
            fputs("(&(", c->out);
            cexpr(c, base);
            fputs("))", c->out);
        }
        break;
    }
    default:
        cerr_(c, n, "this expression has no C form in value position");
        fputs("0", c->out);
        break;
    }
}
