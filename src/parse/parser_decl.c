#include "common.h"
#include "parser_int.h"

/*
 * parser_decl.c - the heavier declarations: functions (plain,
 * qualified, methods, multi-struct methods, prototypes, expression
 * shorthand) and static declarations (constants, type aliases,
 * Struct/Union/Enum, static functions and static blocks).
 * A prototype with no body anywhere in the program declares an
 * external symbol; the linker resolves it (no extern keyword).
 */

/* Struct/Union body fields or Enum variants until '}'. */
static Node *pp_parse_aggregate(Parser *p, int kind, Token nm) {
    Node *s = node_new(kind, nm);
    s->name = pp_strndup(nm.start, nm.len);
    p->pos++;                               /* Struct/Union/Enum keyword */
    if (kind == NK_ENUM && !pp_at(p, TK_LBRACE))
        s->child[0] = pp_parse_type(p);     /* Enum Int32 { ... } */
    pp_ex(p, TK_LBRACE, "'{' to open type definition");
    while (!pp_at(p, TK_RBRACE) && !pp_at(p, TK_EOF)) {
        if (kind == NK_ENUM) {
            Token v = *pp_ex(p, TK_IDENT, "enum variant");
            Node *var = node_new(NK_VARIANT, v);
            var->name = pp_strndup(v.start, v.len);
            if (pp_ac(p, TK_ASSIGN)) var->child[0] = pp_parse_expr(p);
            node_add(s, var);
        } else {
            Token f = *pp_ex(p, TK_IDENT, "field name");
            Node *fd = node_new(NK_FIELD, f);
            fd->name = pp_strndup(f.start, f.len);
            pp_ex(p, TK_COLON, "':' before field type");
            fd->child[0] = pp_parse_type(p);
            node_add(s, fd);
        }
        if (!pp_ac(p, TK_COMMA) && !pp_ac(p, TK_SEMI)) break;
    }
    pp_ex(p, TK_RBRACE, "'}' to close type definition");
    return s;
}

static void pp_parse_params(Parser *p, Node *f) {
    pp_ex(p, TK_LPAREN, "'(' before parameter list");
    if (pp_ac(p, TK_RPAREN)) return;
    for (;;) {
        Token t = *pp_pk(p, 0);
        Node *param = node_new(NK_PARAM, t);
        if (pp_ac(p, TK_KW_STATIC)) {     /* static T */
            param->flags |= NF_STATIC;
            Token n = *pp_ex(p, TK_IDENT, "compile-time parameter name");
            param->name = pp_strndup(n.start, n.len);
            if (pp_ac(p, TK_ASSIGN))
                param->child[0] = pp_parse_type(p);
        } else {
            Token n = *pp_ex(p, TK_IDENT, "parameter name");
            param->name = pp_strndup(n.start, n.len);
            /* An omitted annotation defaults to Int64 downstream;
             * the declaration keeps child[0] empty. */
            if (pp_ac(p, TK_COLON))
                param->child[0] = pp_parse_type(p);
        }
        node_add(f, param);
        if (!pp_ac(p, TK_COMMA)) break;
    }
    pp_ex(p, TK_RPAREN, "')' after parameter list");
}

Node *pp_parse_func(Parser *p) {
    Token t = *pp_pk(p, 0);
    pp_ac(p, TK_KW_FUNC);
    Node * volatile f = node_new(NK_FUNC, t);

    if (pp_at(p, TK_LBRACE)) {              /* func {A, B}.name(...) */
        p->pos++;
        char *names = NULL;
        for (;;) {
            char *one = pp_qualify(p);
            if (!names) names = one;
            else {
                size_t n = strlen(names);
                names = realloc(names, n + 1 + strlen(one) + 1);
                strcat(names, ","); strcat(names, one);
            }
            if (!pp_ac(p, TK_COMMA)) break;
        }
        pp_ex(p, TK_RBRACE, "'}' after struct list");
        pp_ex(p, TK_DOT, "'.' after struct list");
        Token m = pp_name_ex(p, "method name");
        size_t n = strlen(names);
        names = realloc(names, n + 1 + (size_t)m.len + 1);
        strcat(names, "."); strncat(names, m.start, (size_t)m.len);
        f->name = names;
    } else {
        Token nm = pp_name_ex(p, "function name");
        f->name = pp_strndup(nm.start, nm.len);
        while (pp_ac(p, TK_COLONCOLON)) {   /* qualified name */
            if (!pp_member_kind(pp_pk(p, 0)->kind))
                pp_ex(p, TK_IDENT, "identifier after '::'");
            Token m = *pp_pk(p, 0);
            p->pos++;
            size_t n = strlen(f->name);
            f->name = realloc(f->name, n + 2 + (size_t)m.len + 1);
            strcat(f->name, "::"); strncat(f->name, m.start, (size_t)m.len);
        }
        if (pp_ac(p, TK_DOT)) {             /* method: Point.distance */
            Token m = pp_name_ex(p, "method name");
            size_t n = strlen(f->name);
            f->name = realloc(f->name, n + 1 + (size_t)m.len + 1);
            strcat(f->name, "."); strncat(f->name, m.start, (size_t)m.len);
        }
    }

    pp_parse_params(p, f);

    /* parse_type() consumes the '!' suffix itself (T!), so the error
     * flag is read back from the node kind it produced. */
    if (pp_ac(p, TK_COLON)) f->child[0] = pp_parse_type(p);
    int is_err = f->child[0] && f->child[0]->kind == NK_TYPE_ERROR;

    if (pp_ac(p, TK_ASSIGN)) {              /* func f(...): T = expr; */
        f->child[2] = pp_parse_expr(p);
        pp_ex(p, TK_SEMI, "';' after function shorthand body");
        return f;
    }
    if (pp_ac(p, TK_SEMI)) {                /* prototype: body elsewhere
                                             * in the program or external */
        f->flags |= NF_PROTOTYPE;
        return f;
    }
    int saved = p->cur_fn_err;              /* body: enable try when T! */
    p->cur_fn_err = is_err;
    f->child[3] = pp_parse_block(p);
    p->cur_fn_err = saved;
    return f;
}

/* Anonymous function literal: (params): ret { body } with a
 * synthesized name; binding happens at the declaration site. */
static Node *pp_parse_func_literal(Parser *p) {
    Token t = *pp_pk(p, 0);
    static int anon_seq;
    Node * volatile f = node_new(NK_FUNC, t);
    char anon[40];
    snprintf(anon, sizeof(anon), "@fn%d", anon_seq++);
    f->name = pp_strndup(anon, strlen(anon));
    pp_ex(p, TK_LPAREN, "'(' after func");
    while (!pp_at(p, TK_RPAREN) && !pp_at(p, TK_EOF)) {
        Token pn = *pp_ex(p, TK_IDENT, "parameter name");
        Node *ptype = NULL;
        if (pp_ac(p, TK_COLON)) ptype = pp_parse_type(p);
        Node *param = node_new(NK_PARAM, pn);
        param->name = pp_strndup(pn.start, pn.len);
        param->child[0] = ptype;
        node_add(f, param);
        if (!pp_ac(p, TK_COMMA)) break;
    }
    pp_ex(p, TK_RPAREN, "')' after parameters");
    if (pp_ac(p, TK_COLON)) f->child[0] = pp_parse_type(p);
    f->child[3] = pp_parse_block(p);
    return f;
}

Node *pp_parse_static(Parser *p) {
    Token t = *pp_pk(p, 0);
    pp_ac(p, TK_KW_STATIC);

    if (pp_at(p, TK_LBRACE) ||
        (pp_at(p, TK_IDENT) && pp_pk(p, 1)->kind == TK_LBRACE)) {
        Node *b = node_new(NK_STATIC_BLOCK, t);   /* static [name] { } */
        if (pp_at(p, TK_IDENT)) {
            b->name = pp_strndup(pp_pk(p, 0)->start, pp_pk(p, 0)->len);
            p->pos++;
        }
        pp_ex(p, TK_LBRACE, "'{' to open static block");
        p->sync_depth++;
        while (!pp_at(p, TK_RBRACE) && !pp_at(p, TK_EOF)) {
            if (setjmp(p->sync_envs[p->sync_depth - 1]) == 0) {
                p->panicking = 0;
                if (pp_at(p, TK_KW_STATIC)) node_add(b, pp_parse_static(p));
                else node_add(b, pp_parse_stmt(p));
            } else {
                pp_sync(p);
            }
        }
        p->sync_depth--;
        pp_ex(p, TK_RBRACE, "'}' to close static block");
        return b;
    }
    if (pp_ac(p, TK_KW_LET)) {              /* static let x: T = expr; */
        Node *c = node_new(NK_STATIC_CONST, t);
        Token n = *pp_ex(p, TK_IDENT, "name after 'static let'");
        c->name = pp_strndup(n.start, n.len);
        /* Keep the same annotation/value layout as an ordinary let.
         * `static let` is a declaration spelling, not a separate type
         * grammar, so explicit annotations must work here as well. */
        if (pp_ac(p, TK_COLON)) c->child[0] = pp_parse_type(p);
        pp_ex(p, TK_ASSIGN, "'=' in static declaration");
        c->child[1] = pp_parse_expr(p);
        pp_ex(p, TK_SEMI, "';' after static declaration");
        return c;
    }

    Token nm = *pp_ex(p, TK_IDENT, "static name");

    if (pp_at(p, TK_COLON)) {
        /* static NAME: Type = value; */
        Node *c = node_new(NK_STATIC_CONST, nm);
        c->name = pp_strndup(nm.start, nm.len);
        p->pos++;                       /* ':' itself              */
        c->child[0] = pp_parse_type(p);
        pp_ex(p, TK_ASSIGN, "'=' in static declaration");
        c->child[1] = pp_parse_expr(p);
        pp_ex(p, TK_SEMI, "';' after static declaration");
        return c;
    }

    if (pp_at(p, TK_LPAREN)) {              /* static function */
        Node *f = node_new(NK_STATIC_FN, nm);
        f->name = pp_strndup(nm.start, nm.len);
        pp_parse_params(p, f);
        if (pp_ac(p, TK_COLON)) f->child[0] = pp_parse_type(p);
        f->child[3] = pp_parse_block(p);
        return f;
    }

    pp_ex(p, TK_ASSIGN, "'=' in static declaration");
    if (pp_at(p, TK_KW_STRUCT)) { Node *s = pp_parse_aggregate(p, NK_STRUCT, nm); pp_ac(p, TK_SEMI); return s; }
    if (pp_at(p, TK_KW_UNION))  { Node *s = pp_parse_aggregate(p, NK_UNION, nm);  pp_ac(p, TK_SEMI); return s; }
    if (pp_at(p, TK_KW_ENUM))   { Node *s = pp_parse_aggregate(p, NK_ENUM, nm);   pp_ac(p, TK_SEMI); return s; }

    Node *c = node_new(NK_STATIC_CONST, nm);
    c->name = pp_strndup(nm.start, nm.len);
    /* Same child layout as a let: optional type in child[0],
     * value in child[1]. A func literal binds to the name. */
    if (pp_at(p, TK_KW_FUNC)) {
        p->pos++;                       /* 'func' keyword          */
        Node *f = pp_parse_func_literal(p);
        f->name = c->name;              /* the literal takes the
                                           declared name            */
        pp_ex(p, TK_SEMI, "';' after static declaration");
        return f;
    }
    c->child[1] = pp_parse_expr(p);
    pp_ex(p, TK_SEMI, "';' after static declaration");
    return c;
}
