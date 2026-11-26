#include "common.h"
#include "parser_int.h"

/*
 * parser_expr.c - the binary end of the expression grammar. Every
 * level is one precedence step; each function parses its own
 * operators then delegates the tighter levels below. Assignment is
 * right-associative and exists at statement level via TK_ASSIGN; the
 * compound forms (+=, -=, ...) reuse the same right operand. The
 * catch handler binds at the level of `or`, so `f() catch |e| { }`
 * and the bare `f() catch { }` both attach to the whole call. Bit-or
 * is '$'; '||' and '&&' are the short-circuit or/and. Ternary does
 * not exist (if/match are expressions).
 */

Node *pp_parse_assign(Parser *p) {
    /* The left operand runs through the full precedence chain so a
     * catch handler, channel send, await or range expression may
     * appear as a statement on its own ('f() catch { ... };'). */
    Node *l = pp_parse_range(p);
    /* Every assignment form is one node here: plain '=' plus the
     * whole compound-assignment range, which occupies one contiguous
     * run of token kinds (see l_is_compound_assign in lower_ops.c).
     * The right operand recurses, so chains associate rightward. */
    TokenKind k = pp_pk(p, 0)->kind;
    if (k == TK_ASSIGN || (k >= TK_PLUS_EQ && k <= TK_USHR_EQ)) {
        Token t = *pp_pk(p, 0);
        p->pos++;
        Node *a = node_new(NK_BINARY, t);
        a->op = k;
        a->child[0] = l;
        a->child[1] = pp_parse_assign(p);   /* right-assoc: a = b = c */
        return a;
    }
    return l;
}

/* All ordinary infix operators share one precedence table. Keeping the
 * table in one place prevents the parser's grammar layers from drifting
 * apart when an operator is added or its precedence is corrected. The
 * Pratt loop is deliberately small: precedence and associativity are data,
 * while operand construction is identical for every binary operator. */
typedef struct {
    TokenKind token;
    int precedence;
    TokenKind normalized;
} InfixOperator;

static const InfixOperator infix_operators[] = {
    { TK_STAR,    90, TK_STAR },
    { TK_SLASH,   90, TK_SLASH },
    { TK_PERCENT, 90, TK_PERCENT },
    { TK_PLUS,    80, TK_PLUS },
    { TK_MINUS,   80, TK_MINUS },
    { TK_SHL,     70, TK_SHL },
    { TK_USHL,    70, TK_SHL },
    { TK_SHR,     70, TK_SHR },
    { TK_USHR,    70, TK_USHR },
    { TK_AMP,     60, TK_AMP },
    { TK_CARET,   50, TK_CARET },
    { TK_DOLLAR,  40, TK_DOLLAR },
    { TK_EQ,      30, TK_EQ },
    { TK_NE,      30, TK_NE },
    { TK_LT,      30, TK_LT },
    { TK_GT,      30, TK_GT },
    { TK_LE,      30, TK_LE },
    { TK_GE,      30, TK_GE },
    { TK_ANDAND,  20, TK_ANDAND },
    { TK_KW_AND,  20, TK_ANDAND },
    { TK_OROR,    10, TK_OROR },
    { TK_KW_OR,   10, TK_OROR },
};

static const InfixOperator *pp_infix_operator(TokenKind token) {
    size_t i;
    for (i = 0; i < sizeof(infix_operators) / sizeof(infix_operators[0]); ++i)
        if (infix_operators[i].token == token) return &infix_operators[i];
    return NULL;
}

static Node *pp_parse_binary(Parser *p, int min_precedence) {
    Node *left = pp_parse_unary(p);
    for (;;) {
        Token *next = pp_pk(p, 0);
        const InfixOperator *op = pp_infix_operator(next->kind);
        if (!op || op->precedence < min_precedence) break;

        Token token = *next;
        p->pos++;
        Node *node = node_new(NK_BINARY, token);
        node->op = op->normalized;
        node->child[0] = left;
        /* All infix operators in the language specification are
         * left-associative. min+1 prevents the right side from consuming
         * another operator at the same precedence. */
        node->child[1] = pp_parse_binary(p, op->precedence + 1);
        left = node;
    }
    return left;
}

Node *pp_parse_or(Parser *p)    { return pp_parse_binary(p, 10); }
Node *pp_parse_and(Parser *p)   { return pp_parse_binary(p, 20); }
Node *pp_parse_bitor(Parser *p) { return pp_parse_binary(p, 40); }
Node *pp_parse_bitxor(Parser *p){ return pp_parse_binary(p, 50); }
Node *pp_parse_bitand(Parser *p){ return pp_parse_binary(p, 60); }
Node *pp_parse_eq(Parser *p)    { return pp_parse_binary(p, 30); }
Node *pp_parse_rel(Parser *p)   { return pp_parse_binary(p, 30); }
Node *pp_parse_shift(Parser *p) { return pp_parse_binary(p, 70); }
Node *pp_parse_add(Parser *p)   { return pp_parse_binary(p, 80); }
Node *pp_parse_mul(Parser *p)   { return pp_parse_binary(p, 90); }

Node *pp_parse_catch(Parser *p) {
    Node *l = pp_parse_or(p);
    if (pp_at(p, TK_KW_CATCH)) {
        Token t = *pp_pk(p, 0);
        p->pos++;
        Node *n = node_new(NK_CATCH, t);
        /* catch only forms as an expression; its handler value may
         * feed the enclosing expression */
        n->flags |= NF_EXPR;
        n->child[0] = l;
        if (pp_at(p, TK_PIPE)) {            /* catch |e| { }: name binds
                                             * the error value for the
                                             * handler block */
            p->pos++;
            Token v = *pp_ex(p, TK_IDENT, "catch parameter");
            n->name = pp_strndup(v.start, v.len);
            pp_ex(p, TK_PIPE, "'|' after catch parameter");
        }
        n->child[1] = pp_parse_block(p);
        return n;
    }
    return l;
}

Node *pp_parse_ternary(Parser *p) {
    return pp_parse_catch(p);
}

Node *pp_parse_chan(Parser *p) {
    Node *l = pp_parse_ternary(p);
    if (pp_at(p, TK_SENDRECV)) {          /* send: ch <- value; right-associative */
        Token t = *pp_pk(p, 0);
        p->pos++;
        Node *n = node_new(NK_BINARY, t);
        n->op = TK_SENDRECV;
        n->child[0] = l;
        n->child[1] = pp_parse_chan(p);
        return n;
    }
    return l;
}

Node *pp_parse_await(Parser *p) {
    Node *l = pp_parse_chan(p);
    while (pp_at(p, TK_KW_AWAIT)) {
        Token t = *pp_pk(p, 0);
        p->pos++;
        Node *n = node_new(NK_UNARY, t);
        n->op = TK_KW_AWAIT;
        n->child[0] = l;
        l = n;
    }
    return l;
}

Node *pp_parse_range(Parser *p) {
    Node *l = pp_parse_await(p);
    if (pp_at(p, TK_DOTDOT)) {
        Token t = *pp_pk(p, 0);
        if (!l)
            pp_err(p, "range requires a starting value before '..'");
        p->pos++;
        Node *n = node_new(NK_RANGE, t);
        n->child[0] = l;
        if (pp_ac(p, TK_LT)) n->flags |= NF_EXCLUSIVE;
        if (pp_at(p, TK_SEMI) || pp_at(p, TK_RBRACE) ||
            pp_at(p, TK_COMMA) || pp_at(p, TK_EOF))
            pp_err(p, "range requires an ending value after '..'");
        n->child[1] = pp_parse_await(p);
        if (!n->child[1])
            pp_err(p, "range requires an ending value after '..'");
        return n;
    }
    return l;
}

Node *pp_parse_expr(Parser *p) {
    return pp_parse_range(p);
}
