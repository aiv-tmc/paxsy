#include "common.h"
#include "parser_int.h"

/*
 * parser_stmt.c - statements and control flow: blocks, if/else (with
 * the if-let/if-func/if-static existence forms and the bare-
 * condition if expression), match (statement and expression), while,
 * do-while, for ranges (all boundary forms), labelled loops, select
 * arms, defer/volatile/asm and expression statements. Declarations are
 * accepted wherever statements are, so module and static-block
 * bodies can reuse this entry point. In value-position if/match/catch
 * blocks, the final expression statement supplies the block value;
 * its semicolon may be omitted immediately before the closing brace.
 */

Node *pp_parse_block(Parser *p) {
    Token t = *pp_ex(p, TK_LBRACE, "'{' to open a block");
    Node *b = node_new(NK_BLOCK, t);
    /* A statement failure unwinds to the frame below; pp_sync() skips
     * the broken statement and the loop resumes with the next one. */
    p->sync_depth++;
    while (!pp_at(p, TK_RBRACE) && !pp_at(p, TK_EOF)) {
        if (setjmp(p->sync_envs[p->sync_depth - 1]) == 0) {
            p->panicking = 0;
            node_add(b, pp_parse_stmt(p));
        } else {
            pp_sync(p);
        }
    }
    p->sync_depth--;
    pp_ex(p, TK_RBRACE, "'}' to close a block");
    return b;
}

Node *pp_parse_if(Parser *p, int in_expr) {
    Token t = *pp_pk(p, 0);
    pp_ac(p, TK_KW_IF);
    Node *n;
    if (pp_at(p, TK_KW_LET) || pp_at(p, TK_KW_FUNC) || pp_at(p, TK_KW_STATIC)) {
        n = node_new(NK_IF_EXISTS, t);      /* compile-time existence check */
        n->op = pp_pk(p, 0)->kind;
        p->pos++;
        Token nm = *pp_ex(p, TK_IDENT, "name in existence check");
        n->name = pp_strndup(nm.start, nm.len);
        n->child[0] = pp_parse_stmt(p);
        if (pp_ac(p, TK_KW_ELSE))
            n->child[1] = pp_parse_stmt(p);
        return n;
    }
    n = node_new(NK_IF, t);
    if (in_expr) n->flags |= NF_EXPR;
    p->block_next++;
    n->child[0] = pp_parse_expr(p);         /* '{' ahead is this if's block */
    p->block_next--;
    if (in_expr && !pp_at(p, TK_LBRACE)) {
        /* An if expression without a block is the bare condition
         * form: its value is the condition itself (Bool). */
        return n;
    }
    /* The body is one statement: a '{}' block or any single
     * statement, so 'else if' composes without a special case. */
    n->child[1] = pp_parse_stmt(p);
    if (pp_ac(p, TK_KW_ELSE))
        n->child[2] = pp_parse_stmt(p);
    return n;
}

static Node *pp_parse_for(Parser *p) {
    Token t = *pp_pk(p, 0);
    pp_ac(p, TK_KW_FOR);
    Node *f = node_new(NK_FOR, t);
    Node *r = node_new(NK_RANGE, t);
    Node *container = NULL;
    if (pp_at(p, TK_DOTDOT))
        pp_err(p, "for range requires a starting value before '..'");
    p->block_next++;
    r->child[0] = pp_parse_bitor(p);
    p->block_next--;
    if (pp_at(p, TK_GT)) {                 /* 5>.. : inclusive          */
        r->flags |= NF_INCLUSIVE;
        p->pos++;
    }
    if (pp_ac(p, TK_DOTDOT)) {
        if (pp_ac(p, TK_LT)) r->flags |= NF_EXCLUSIVE;  /* ..<10 */
        else if (pp_ac(p, TK_ASSIGN)) r->flags |= NF_INCLUSIVE;  /* ..=10 */
        if (!pp_at(p, TK_PIPE) && !pp_at(p, TK_LBRACE)) {
            p->block_next++;
            r->child[1] = pp_parse_bitor(p);    /* upper bound */
            p->block_next--;
        } else {
            pp_err(p, "for range requires an ending value after '..'");
        }
    } else if (r->child[0] &&
               (pp_at(p, TK_PIPE) || pp_at(p, TK_LBRACE))) {
        /* 'for expr |x| { ... }': iterate the container. The range
         * becomes 0..expr.len and the body gains an element binding;
         * both share the container subtree (the AST is an arena). */
        container = r->child[0];
        /* A plain counter loop: the lower bound must be numeric,
         * because the backend reads an array-typed bound as the
         * container of a view iteration. */
        Token zt = container->tok;
        zt.kind = TK_INT_LIT;
        zt.start = "0";
        zt.len = 1;
        r->child[0] = node_new(NK_LITERAL, zt);
        Node *len = node_new(NK_FIELD_ACCESS, container->tok);
        len->child[0] = container;
        len->name = "len";
        r->child[1] = len;
    } else
        pp_err(p, "'..' in for range");
    f->child[0] = r;
    if (pp_ac(p, TK_PIPE)) {
        if (!pp_at(p, TK_PIPE)) {
            Token v = *pp_ex(p, TK_IDENT, "loop variable");
            f->name = pp_strndup(v.start, v.len);
            if (pp_ac(p, TK_COLON)) f->child[2] = pp_parse_type(p);
        }
        pp_ex(p, TK_PIPE, "'|' after loop variable");
    }
    f->child[3] = pp_parse_stmt(p);
    if (container && f->name) {
        /* Rewire the named loop variable to the hidden counter and
         * bind the element inside the body. */
        char *elem = (char *)f->name;
        f->name = pp_strndup("__it", 4);
        Node *blk = node_new(NK_BLOCK, f->child[3]->tok);
        Node *let = node_new(NK_LET, f->child[3]->tok);
        let->name = elem;
        Node *idx = node_new(NK_INDEX, f->child[3]->tok);
        idx->child[0] = container;
        Node *it = node_new(NK_NAME, f->child[3]->tok);
        it->name = pp_strndup("__it", 4);
        idx->child[1] = it;
        let->child[1] = idx;
        node_add(blk, let);
        node_add(blk, f->child[3]);
        f->child[3] = blk;
    }
    return f;
}

static Node *pp_parse_pattern(Parser *p) {
    Node *n = pp_parse_unary(p);
    if (pp_at(p, TK_DOTDOT))
        pp_err(p, "range patterns are not allowed in match cases");
    return n;
}

Node *pp_parse_match(Parser *p, int in_expr) {
    Token t = *pp_pk(p, 0);
    pp_ac(p, TK_KW_MATCH);
    Node *m = node_new(NK_MATCH, t);
    if (in_expr) m->flags |= NF_EXPR;
    p->block_next++;
    m->child[0] = pp_parse_expr(p);         /* '{' ahead is the match body */
    p->block_next--;
    pp_ex(p, TK_LBRACE, "'{' to open match");
    while (!pp_at(p, TK_RBRACE) && !pp_at(p, TK_EOF)) {
        if (pp_ac(p, TK_KW_ELSE)) { m->child[2] = pp_parse_block(p); break; }
        Token ct = *pp_ex(p, TK_KW_CASE, "'case' or 'else' in match");
        Node *cse = node_new(NK_MATCH_CASE, ct);
        for (;;) { node_add(cse, pp_parse_pattern(p)); if (!pp_ac(p, TK_KW_OR)) break; }
        cse->child[1] = pp_parse_block(p);
        node_add(m, cse);
    }
    pp_ex(p, TK_RBRACE, "'}' to close match");
    if (in_expr && !m->child[2])
        pp_err(p, "match expression requires an 'else' branch");
    return m;
}

static Node *pp_parse_select(Parser *p) {
    Token t = *pp_pk(p, 0);
    pp_ac(p, TK_KW_SELECT);
    Node *s = node_new(NK_SELECT, t);
    pp_ex(p, TK_LBRACE, "'{' to open select");
    while (!pp_at(p, TK_RBRACE) && !pp_at(p, TK_EOF)) {
        if (pp_ac(p, TK_KW_ELSE)) { s->child[3] = pp_parse_block(p); break; }
        Token ot = *pp_ex(p, TK_KW_ON, "'on' or 'else' in select");
        Node *arm = node_new(NK_SELECT_ARM, ot);
        if (pp_ac(p, TK_KW_LET)) {
            Token v = *pp_ex(p, TK_IDENT, "variable after 'let'");
            arm->name = pp_strndup(v.start, v.len);
            pp_ex(p, TK_ASSIGN, "'=' in receive arm");
        }
        p->block_next++;
        if (pp_ac(p, TK_SENDRECV)) {         /* on [let x =] <- ch { } */
            arm->flags |= NF_RECV;
            arm->child[0] = pp_parse_expr(p);
        } else {                            /* on ch <- value { } */
            arm->child[1] = pp_parse_expr(p);
            pp_ex(p, TK_SENDRECV, "'<-' in select arm");
            arm->child[2] = pp_parse_expr(p);
        }
        p->block_next--;
        arm->child[3] = pp_parse_block(p);
        node_add(s, arm);
    }
    pp_ex(p, TK_RBRACE, "'}' to close select");
    return s;
}

Node *pp_parse_stmt(Parser *p) {
    Token t = *pp_pk(p, 0);
    switch (t.kind) {
    case TK_KW_USE:      return pp_parse_use(p);
    case TK_KW_MODULE:   return pp_parse_module(p);
    case TK_KW_STATIC: return pp_parse_static(p);
    case TK_KW_FUNC:     return pp_parse_func(p);
    case TK_KW_LET:      return pp_parse_let(p);
    case TK_LBRACE:      return pp_parse_block(p);
    case TK_KW_IF:       return pp_parse_if(p, 0);
    case TK_KW_WHILE: {
        p->pos++;
        Node *w = node_new(NK_WHILE, t);
        p->block_next++;
        w->child[0] = pp_parse_expr(p);
        p->block_next--;
        w->child[1] = pp_parse_stmt(p);
        return w;
    }
    case TK_KW_DO: {
        p->pos++;
        Node *w = node_new(NK_DOWHILE, t);
        w->child[0] = pp_parse_stmt(p);
        pp_ex(p, TK_KW_WHILE, "'while' after do-while body");
        p->block_next++;
        w->child[1] = pp_parse_expr(p);
        p->block_next--;
        pp_ex(p, TK_SEMI, "';' after do-while");
        return w;
    }
    case TK_KW_FOR:      return pp_parse_for(p);
    case TK_KW_MATCH:    return pp_parse_match(p, 0);
    case TK_KW_RETURN: {
        p->pos++;
        Node *r = node_new(NK_RETURN, t);
        if (!pp_at(p, TK_SEMI) && !pp_at(p, TK_RBRACE))
            r->child[0] = pp_parse_expr(p);
        pp_ex(p, TK_SEMI, "';' after return");
        return r;
    }
    case TK_KW_FREE: {
        p->pos++;
        Node *op = node_new(NK_UNARY, t);
        Node *stmt = node_new(NK_EXPR_STMT, t);
        op->op = TK_KW_FREE;
        op->child[0] = pp_parse_unary(p);
        stmt->child[0] = op;
        pp_ex(p, TK_SEMI, "';' after free");
        return stmt;
    }
    case TK_KW_DEFER: {
        p->pos++;
        Node *d = node_new(NK_DEFER, t);
        d->child[0] = pp_parse_stmt(p);
        return d;
    }
    case TK_KW_PASS:
        p->pos++;
        pp_ex(p, TK_SEMI, "';' after pass");
        return node_new(NK_PASS, t);
    case TK_KW_BREAK:
    case TK_KW_CONTINUE: {
        p->pos++;
        Node *b = node_new(t.kind == TK_KW_BREAK ? NK_BREAK : NK_CONTINUE, t);
        if (pp_at(p, TK_IDENT)) {
            b->name = pp_strndup(pp_pk(p, 0)->start, pp_pk(p, 0)->len);
            p->pos++;
        }
        pp_ex(p, TK_SEMI, "';' after break/continue");
        return b;
    }
    case TK_KW_ROUTINE: {
        p->pos++;
        Node *r = node_new(NK_ROUTINE, t);
        r->child[0] = pp_parse_block(p);
        return r;
    }
    case TK_KW_ASYNC:
        if (pp_pk(p, 1)->kind == TK_LBRACE) {
            p->pos++;
            Node *a = node_new(NK_ASYNC, t);
            a->child[0] = pp_parse_block(p);
            return a;
        }
        break;                              /* async call: expression stmt */
    case TK_KW_SELECT:   return pp_parse_select(p);
    case TK_KW_CLOSE: {
        p->pos++;
        Node *c = node_new(NK_CLOSE, t);
        c->child[0] = pp_parse_expr(p);
        pp_ex(p, TK_SEMI, "';' after close");
        return c;
    }
    case TK_KW_VOLATILE: {
        p->pos++;
        Node *v = node_new(NK_VOLATILE, t);
        v->child[0] = pp_parse_stmt(p);
        return v;
    }
    case TK_KW_ASM: {
        p->pos++;
        Node *a = node_new(NK_ASM, t);
        Token s = *pp_ex(p, TK_STRING_LIT, "assembly string literal");
        a->tok = s;
        pp_ex(p, TK_SEMI, "';' after asm statement");
        return a;
    }
    case TK_IDENT:                          /* labelled loop */
        if (pp_pk(p, 1)->kind == TK_COLON &&
            (pp_pk(p, 2)->kind == TK_KW_FOR ||
             pp_pk(p, 2)->kind == TK_KW_WHILE ||
             pp_pk(p, 2)->kind == TK_KW_DO)) {
            Token lb = *pp_pk(p, 0);
            p->pos += 2;
            Node *l = node_new(NK_LABEL, lb);
            l->name = pp_strndup(lb.start, lb.len);
            l->child[0] = pp_parse_stmt(p);
            return l;
        }
        break;
    default:
        break;
    }
    {
        /* Statement level includes assignment: an expression
         * statement may be a plain call, an assignment or a
         * compound assignment (+=, -=, ...). pp_parse_assign
         * covers all three shapes. */
        Node *e = pp_parse_assign(p);
        Node *s;
        if (!pp_ac(p, TK_SEMI)) {
            /* Only the final expression may omit its semicolon. In a
             * value-position block, the lowering pass uses that final
             * expression as the block result. */
            if (!pp_at(p, TK_RBRACE))
                pp_err_previous_help(p, "expected ';' after expression",
                                     "Insert ';' at the end of the previous statement.");
        }
        s = node_new(NK_EXPR_STMT, t);
        s->child[0] = e;
        return s;
    }
}
