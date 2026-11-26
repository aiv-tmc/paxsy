#include "common.h"
#include "parser_int.h"

/*
 * parser_type.c - type grammar: qualified names, &T pointers, T[N]
 * arrays, tuple types, chan T and the error suffix T!. The error
 * suffix applies to the whole type: &UInt8! is an error of pointer
 * ("a pointer, or an error"), never a pointer to an error, so '!' is
 * parsed after the prefix (&, chan) and array layers are complete.
 */

/* Parses one non-error type: base name or tuple, wrapped by any
 * prefix pointer/chan layers and any trailing array layers. */
static Node *pp_parse_type_plain(Parser *p) {
    Node *t;
    Token tok = *pp_pk(p, 0);
    if (pp_ac(p, TK_LPAREN)) {              /* (Int32, Real64) */
        t = node_new(NK_TYPE_TUPLE, tok);
        if (!pp_at(p, TK_RPAREN)) {
            for (;;) {
                node_add(t, pp_parse_type(p));
                if (!pp_ac(p, TK_COMMA)) break;
            }
        }
        pp_ex(p, TK_RPAREN, "')' in tuple type");
    } else if (pp_at(p, TK_AMP)) {
        t = node_new(NK_TYPE_PTR, tok);
        p->pos++;
        t->child[0] = pp_parse_type_plain(p);
    } else if (pp_at(p, TK_KW_CHAN)) {
        t = node_new(NK_TYPE_CHAN, tok);
        p->pos++;
        t->child[0] = pp_parse_type_plain(p);
    } else {
        t = node_new(NK_TYPE_NAME, tok);
        t->name = pp_qualify(p);
    }
    while (pp_at(p, TK_LBRACKET)) {         /* postfix arrays: T[N] */
        Token bt = *pp_pk(p, 0);
        p->pos++;
        Node *arr = node_new(NK_TYPE_ARRAY, bt);
        arr->child[0] = t;
        if (pp_at(p, TK_DOTDOT)) {          /* T[...]: slice view   */
            p->pos++;
            arr->flags |= NF_OPEN_LO;
            if (!pp_at(p, TK_RBRACKET))
                arr->child[1] = pp_parse_expr(p);
        } else if (!pp_at(p, TK_RBRACKET)) arr->child[1] = pp_parse_expr(p);
        pp_ex(p, TK_RBRACKET, "']' in array type");
        t = arr;
    }
    return t;
}

Node *pp_parse_type(Parser *p) {
    Node *t = pp_parse_type_plain(p);
    while (pp_ac(p, TK_BANG)) {             /* T! wraps the whole type */
        Node *e = node_new(NK_TYPE_ERROR, *pp_pk(p, -1));
        e->child[0] = t;
        t = e;
    }
    return t;
}
