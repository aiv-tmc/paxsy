#include "common.h"
#include "parser_int.h"

/*
 * parser_type.c - type grammar: qualified names, &T pointers, T[N]
 * arrays, tuple types, chan T and the error suffix T!. Postfix [] and !
 * bind tighter than prefix & and chan.
 */

Node *pp_parse_type(Parser *p) {
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
        t->child[0] = pp_parse_type(p);
    } else if (pp_at(p, TK_KW_CHAN)) {
        t = node_new(NK_TYPE_CHAN, tok);
        p->pos++;
        t->child[0] = pp_parse_type(p);
    } else {
        t = node_new(NK_TYPE_NAME, tok);
        t->name = pp_qualify(p);
    }
    for (;;) {                              /* postfix: [N] and ! */
        if (pp_at(p, TK_LBRACKET)) {
            Token bt = *pp_pk(p, 0);
            p->pos++;
            Node *arr = node_new(NK_TYPE_ARRAY, bt);
            arr->child[0] = t;
            if (!pp_at(p, TK_RBRACKET)) arr->child[1] = pp_parse_expr(p);
            pp_ex(p, TK_RBRACKET, "']' in array type");
            t = arr;
        } else if (pp_ac(p, TK_BANG)) {
            Node *e = node_new(NK_TYPE_ERROR, tok);
            e->child[0] = t;
            t = e;
        } else break;
    }
    return t;
}
