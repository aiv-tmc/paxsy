#include "internal.h"
#include <stdlib.h>
#include <string.h>

/*
 * Parses a block: { statement* }.
 */
ASTNode* parse_block(Parser* p) {
    if (!match(p, TOKEN_LBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax", "Expected '{'");
        p->panic_mode = true;
        return NULL;
    }

    Block* block = (Block*)make_node(p, NODE_BLOCK, sizeof(Block));
    if (!block) {
        return NULL;
    }
    block->statements = NULL;
    block->stmt_count = 0;

    while (peek(p) != TOKEN_RBRACE && peek(p) != TOKEN_EOF) {
        if (p->panic_mode) {
            synchronize_progress(p);
            p->panic_mode = false;
            if (peek(p) == TOKEN_EOF) {
                break;
            }
        }
        ASTNode* stmt = parse_statement(p);
        if (stmt) {
            append_child(p, (void***)&block->statements, &block->stmt_count, stmt);
        } else {
            if (peek(p) != TOKEN_RBRACE && peek(p) != TOKEN_EOF) {
                synchronize_progress(p);
                p->panic_mode = false;
            }
        }
    }

    if (!match(p, TOKEN_RBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '}' to close block");
        p->panic_mode = true;
    }
    return (ASTNode*)block;
}

/*
 * Dispatcher for statements.
 */
ASTNode* parse_statement(Parser* p) {
    /* A label prefix belongs to a loop statement: outer: for ... */
    if (peek(p) == TOKEN_IDENTIFIER && p->current + 2 < p->token_count &&
        p->tokens[p->current + 1].type == TOKEN_COLON) {
        TokenType third = p->tokens[p->current + 2].type;
        if (third == TOKEN_WHILE) return parse_while_stmt(p);
        if (third == TOKEN_DO) return parse_do_while_stmt(p);
        if (third == TOKEN_FOR) return parse_for_stmt(p);
    }
    switch (peek(p)) {
        case TOKEN_LBRACE:       return parse_block(p);
        case TOKEN_LET:          return parse_let_decl(p);
        case TOKEN_USE:          return parse_use_decl(p);
        case TOKEN_IF:           return parse_if_stmt(p);
        case TOKEN_MATCH:        return parse_match_stmt(p);
        case TOKEN_WHILE:        return parse_while_stmt(p);
        case TOKEN_DO:           return parse_do_while_stmt(p);
        case TOKEN_FOR:          return parse_for_stmt(p);
        case TOKEN_BREAK:        return parse_break_stmt(p);
        case TOKEN_CONTINUE:     return parse_continue_stmt(p);
        case TOKEN_RETURN:       return parse_return_stmt(p);
        case TOKEN_DEFER:        return parse_defer_stmt(p);
        case TOKEN_ASM:          return parse_asm_stmt(p);
        case TOKEN_VOLATILE:     return parse_volatile_stmt(p);
        case TOKEN_COMPTIME:     return parse_comptime_stmt(p);
        case TOKEN_PASS:         return parse_pass_stmt(p);
        case TOKEN_FREE:         return parse_free_stmt(p);
        /* An empty statement is discarded; unlike 'pass' it emits no NOP. */
        case TOKEN_SEMICOLON:    advance(p); return make_node(p, NODE_EMPTY_STMT, sizeof(PassStmt));
        default:                 return parse_expr_stmt(p);
    }
}

/*
 * Parses a compile-time existence check:
 *   if let x { ... } [else { ... }]
 *   if func f { ... } [else { ... }]
 *   if comptime X { ... } [else { ... }]
 * Only the taken branch is compiled; the other is discarded.
 */
static ASTNode* parse_if_exists(Parser* p) {
    uint8_t kind = EXISTS_LET;
    if (peek(p) == TOKEN_FUNC) {
        kind = EXISTS_FUNC;
    } else if (peek(p) == TOKEN_COMPTIME) {
        kind = EXISTS_COMPTIME;
    }
    advance(p); /* let / func / comptime */
    if (!match(p, TOKEN_IDENTIFIER)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected name after existence check keyword");
        p->panic_mode = true;
        return NULL;
    }
    char* name = strdup_custom(p->tokens[p->current - 1].value);
    IfExistsStmt* stmt = (IfExistsStmt*)make_node(p, NODE_IF_EXISTS, sizeof(IfExistsStmt));
    if (!stmt) { free(name); return NULL; }
    stmt->kind = kind;
    stmt->name = name;
    stmt->then_branch = parse_block(p);
    if (!stmt->then_branch) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected block after existence check");
        p->panic_mode = true;
        return (ASTNode*)stmt;
    }
    stmt->else_branch = NULL;
    if (match(p, TOKEN_ELSE)) {
        stmt->else_branch = parse_block(p);
        if (!stmt->else_branch) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected block after else");
            p->panic_mode = true;
        }
    }
    return (ASTNode*)stmt;
}

/*
 * Parses an if statement: if cond { ... } [ else if ... ] [ else { ... } ].
 */
ASTNode* parse_if_stmt(Parser* p) {
    advance(p); // consume TOKEN_IF

    /* Existence checks: 'if let x {', 'if func f {', 'if comptime X {' */
    if ((peek(p) == TOKEN_LET || peek(p) == TOKEN_FUNC || peek(p) == TOKEN_COMPTIME) &&
        p->current + 2 < p->token_count &&
        p->tokens[p->current + 1].type == TOKEN_IDENTIFIER &&
        p->tokens[p->current + 2].type == TOKEN_LBRACE) {
        return parse_if_exists(p);
    }

    /* '{' after the condition opens the then-block, not a struct literal. */
    uint8_t saved_nsi = p->no_struct_init;
    p->no_struct_init = 1;
    ASTNode* cond = parse_assignment_expr(p);
    p->no_struct_init = saved_nsi;
    if (!cond) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected condition expression");
        p->panic_mode = true;
        return NULL;
    }

    ASTNode* then_branch = parse_block(p);
    if (!then_branch) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected then block");
        p->panic_mode = true;
        free_ast_node(cond);
        return NULL;
    }

    IfStmt* stmt = (IfStmt*)make_node(p, NODE_IF_STMT, sizeof(IfStmt));
    if (!stmt) {
        free_ast_node(cond);
        free_ast_node(then_branch);
        return NULL;
    }
    stmt->condition = cond;
    stmt->then_branch = then_branch;
    stmt->else_if_conditions = NULL;
    stmt->else_if_branches = NULL;
    stmt->else_if_count = 0;
    stmt->else_branch = NULL;

    while (match(p, TOKEN_ELSE)) {
        if (peek(p) == TOKEN_IF) {
            advance(p); // consume IF
            uint8_t saved_nsi = p->no_struct_init;
            p->no_struct_init = 1;
            ASTNode* econd = parse_assignment_expr(p);
            p->no_struct_init = saved_nsi;
            if (!econd) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected condition in else-if");
                p->panic_mode = true;
                break;
            }
            ASTNode* ebody = parse_block(p);
            if (!ebody) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected block for else-if");
                free_ast_node(econd);
                p->panic_mode = true;
                break;
            }
            size_t cond_count = stmt->else_if_count;
            size_t branch_count = stmt->else_if_count;
            append_child(p, (void***)&stmt->else_if_conditions, &cond_count, econd);
            append_child(p, (void***)&stmt->else_if_branches, &branch_count, ebody);
            stmt->else_if_count = cond_count < branch_count ? cond_count : branch_count;
        } else {
            ASTNode* ebody = parse_block(p);
            if (!ebody) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected block for else");
                p->panic_mode = true;
                break;
            }
            stmt->else_branch = ebody;
            break;
        }
    }

    return (ASTNode*)stmt;
}

/*
 * Parses a match statement: match expr { case pattern { ... } ... [ else { ... } ] }.
 */
ASTNode* parse_match_stmt(Parser* p) {
    advance(p); // consume TOKEN_MATCH

    /* '{' after the value opens the match body, not a struct literal. */
    uint8_t saved_nsi = p->no_struct_init;
    p->no_struct_init = 1;
    ASTNode* expr = parse_assignment_expr(p);
    p->no_struct_init = saved_nsi;
    if (!expr) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected match expression");
        p->panic_mode = true;
        return NULL;
    }

    if (!match(p, TOKEN_LBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '{' after match expression");
        p->panic_mode = true;
        free_ast_node(expr);
        return NULL;
    }

    MatchStmt* stmt = (MatchStmt*)make_node(p, NODE_MATCH_STMT, sizeof(MatchStmt));
    if (!stmt) {
        free_ast_node(expr);
        return NULL;
    }
    stmt->expr = expr;
    stmt->cases = NULL;
    stmt->case_count = 0;
    stmt->else_case = NULL;

    while (peek(p) != TOKEN_RBRACE && peek(p) != TOKEN_EOF) {
        if (match(p, TOKEN_CASE)) {
            ASTNode* case_node = parse_match_case(p);
            if (case_node) {
                append_child(p, (void***)&stmt->cases, &stmt->case_count, case_node);
            } else {
                p->panic_mode = true;
                synchronize_progress(p);
            }
        } else if (match(p, TOKEN_ELSE)) {
            if (stmt->else_case) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Multiple else clauses in match");
                p->panic_mode = true;
            }
            ASTNode* body = parse_block(p);
            if (!body) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected block for else");
                p->panic_mode = true;
            } else {
                stmt->else_case = body;
            }
        } else {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected 'case' or 'else' in match");
            p->panic_mode = true;
            synchronize_progress(p);
        }
    }

    if (!match(p, TOKEN_RBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '}' to close match");
        p->panic_mode = true;
    }
    return (ASTNode*)stmt;
}

/*
 * Parses a match case: case pattern { ... }.
 */
/*
 * Parses a match case: case pattern [or pattern ...] { ... }.
 * Alternatives are joined by 'or'; each is a literal, a named
 * constant, or an enum variant.
 */
ASTNode* parse_match_case(Parser* p) {
    MatchCase* mc = (MatchCase*)make_node(p, NODE_MATCH_CASE, sizeof(MatchCase));
    if (!mc) {
        return NULL;
    }
    mc->patterns = NULL;
    mc->pattern_count = 0;
    do {
        ASTNode* pat = parse_pattern_expr(p);
        if (!pat) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected pattern in case");
            p->panic_mode = true;
            return NULL;
        }
        append_child(p, (void***)&mc->patterns, &mc->pattern_count, pat);
    } while (match(p, TOKEN_OR));

    mc->body = parse_block(p);
    if (!mc->body) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected block after case pattern");
        p->panic_mode = true;
        return NULL;
    }
    return (ASTNode*)mc;
}

/*
 * Parses a single case pattern: a literal, a named constant, or an
 * enum variant (possibly qualified), with optional unary sign.
 */
ASTNode* parse_pattern_expr(Parser* p) {
    return parse_unary_expr(p);
}

/*
 * Parses a while loop: [label:] while cond { ... }.
 */
ASTNode* parse_while_stmt(Parser* p) {
    char* label = NULL;
    if (peek(p) == TOKEN_IDENTIFIER && p->current + 1 < p->token_count &&
        p->tokens[p->current + 1].type == TOKEN_COLON) {
        label = strdup_custom(p->tokens[p->current].value);
        advance(p);
        advance(p); // consume colon
    }

    if (!match(p, TOKEN_WHILE)) {
        errhandler__report_error(p->line, p->column, "syntax", "Expected 'while'");
        p->panic_mode = true;
        free(label);
        return NULL;
    }

    /* '{' after the condition opens the loop body, not a struct literal. */
    uint8_t saved_nsi = p->no_struct_init;
    p->no_struct_init = 1;
    ASTNode* cond = parse_assignment_expr(p);
    p->no_struct_init = saved_nsi;
    if (!cond) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected while condition");
        p->panic_mode = true;
        free(label);
        return NULL;
    }

    ASTNode* body = parse_block(p);
    if (!body) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected while body block");
        p->panic_mode = true;
        free_ast_node(cond);
        free(label);
        return NULL;
    }

    WhileStmt* stmt = (WhileStmt*)make_node(p, NODE_WHILE_STMT, sizeof(WhileStmt));
    if (!stmt) {
        free_ast_node(cond);
        free_ast_node(body);
        free(label);
        return NULL;
    }
    stmt->label = label;
    stmt->condition = cond;
    stmt->body = body;
    return (ASTNode*)stmt;
}

/*
 * Parses a do-while loop: [label:] do { ... } while cond ;.
 */
ASTNode* parse_do_while_stmt(Parser* p) {
    char* label = NULL;
    if (peek(p) == TOKEN_IDENTIFIER && p->current + 1 < p->token_count &&
        p->tokens[p->current + 1].type == TOKEN_COLON) {
        label = strdup_custom(p->tokens[p->current].value);
        advance(p);
        advance(p);
    }

    if (!match(p, TOKEN_DO)) {
        errhandler__report_error(p->line, p->column, "syntax", "Expected 'do'");
        p->panic_mode = true;
        free(label);
        return NULL;
    }

    ASTNode* body = parse_block(p);
    if (!body) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected do-while body block");
        p->panic_mode = true;
        free(label);
        return NULL;
    }

    if (!match(p, TOKEN_WHILE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected 'while' after do body");
        p->panic_mode = true;
        free_ast_node(body);
        free(label);
        return NULL;
    }

    uint8_t saved_nsi = p->no_struct_init;
    p->no_struct_init = 1;
    ASTNode* cond = parse_assignment_expr(p);
    p->no_struct_init = saved_nsi;
    if (!cond) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected do-while condition");
        p->panic_mode = true;
        free_ast_node(body);
        free(label);
        return NULL;
    }

    if (!match(p, TOKEN_SEMICOLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ';' after do-while condition");
        p->panic_mode = true;
        free_ast_node(cond);
        free_ast_node(body);
        free(label);
        return NULL;
    }

    DoWhileStmt* stmt = (DoWhileStmt*)make_node(p, NODE_DO_WHILE_STMT, sizeof(DoWhileStmt));
    if (!stmt) {
        free_ast_node(cond);
        free_ast_node(body);
        free(label);
        return NULL;
    }
    stmt->label = label;
    stmt->body = body;
    stmt->condition = cond;
    return (ASTNode*)stmt;
}

/*
 * Parses a for loop: [label:] for range | var [ : Type ] | { ... }.
 */
ASTNode* parse_for_stmt(Parser* p) {
    char* label = NULL;
    if (peek(p) == TOKEN_IDENTIFIER && p->current + 1 < p->token_count &&
        p->tokens[p->current + 1].type == TOKEN_COLON) {
        label = strdup_custom(p->tokens[p->current].value);
        advance(p);
        advance(p);
    }

    if (!match(p, TOKEN_FOR)) {
        errhandler__report_error(p->line, p->column, "syntax", "Expected 'for'");
        p->panic_mode = true;
        free(label);
        return NULL;
    }

    ASTNode* range = parse_range_expr(p);
    if (!range) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected range expression");
        p->panic_mode = true;
        free(label);
        return NULL;
    }

    if (!match(p, TOKEN_PIPE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '|' after range in for");
        p->panic_mode = true;
        free_ast_node(range);
        free(label);
        return NULL;
    }

    if (!match(p, TOKEN_IDENTIFIER)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected variable name in for");
        p->panic_mode = true;
        free_ast_node(range);
        free(label);
        return NULL;
    }
    char* var_name = strdup_custom(p->tokens[p->current - 1].value);

    ASTNode* var_type = NULL;
    if (match(p, TOKEN_COLON)) {
        var_type = parse_type_spec(p);
        if (!var_type) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected variable type in for");
            p->panic_mode = true;
        }
    }

    if (!match(p, TOKEN_PIPE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '|' after variable in for");
        p->panic_mode = true;
        free(var_name);
        free_ast_node(var_type);
        free_ast_node(range);
        free(label);
        return NULL;
    }

    ASTNode* body = parse_block(p);
    if (!body) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected for body block");
        p->panic_mode = true;
        free(var_name);
        free_ast_node(var_type);
        free_ast_node(range);
        free(label);
        return NULL;
    }

    ForStmt* stmt = (ForStmt*)make_node(p, NODE_FOR_STMT, sizeof(ForStmt));
    if (!stmt) {
        free(var_name);
        free_ast_node(var_type);
        free_ast_node(range);
        free_ast_node(body);
        free(label);
        return NULL;
    }
    stmt->label = label;
    stmt->range = range;
    stmt->var_name = var_name;
    stmt->var_type = var_type;
    stmt->body = body;
    return (ASTNode*)stmt;
}

/*
 * Parses a break statement: break [label] ;.
 */
ASTNode* parse_break_stmt(Parser* p) {
    advance(p); // consume TOKEN_BREAK
    char* label = NULL;
    if (peek(p) == TOKEN_IDENTIFIER) {
        label = strdup_custom(p->tokens[p->current].value);
        advance(p);
    }
    if (!match(p, TOKEN_SEMICOLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ';' after break");
        p->panic_mode = true;
        free(label);
        return NULL;
    }
    BreakStmt* stmt = (BreakStmt*)make_node(p, NODE_BREAK_STMT, sizeof(BreakStmt));
    if (!stmt) {
        free(label);
        return NULL;
    }
    stmt->label = label;
    return (ASTNode*)stmt;
}

/*
 * Parses a continue statement: continue [label] ;.
 */
ASTNode* parse_continue_stmt(Parser* p) {
    advance(p); // consume TOKEN_CONTINUE
    char* label = NULL;
    if (peek(p) == TOKEN_IDENTIFIER) {
        label = strdup_custom(p->tokens[p->current].value);
        advance(p);
    }
    if (!match(p, TOKEN_SEMICOLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ';' after continue");
        p->panic_mode = true;
        free(label);
        return NULL;
    }
    ContinueStmt* stmt = (ContinueStmt*)make_node(p, NODE_CONTINUE_STMT, sizeof(ContinueStmt));
    if (!stmt) {
        free(label);
        return NULL;
    }
    stmt->label = label;
    return (ASTNode*)stmt;
}

/*
 * Parses a return statement: return [expr] ;.
 */
ASTNode* parse_return_stmt(Parser* p) {
    advance(p); // consume TOKEN_RETURN
    ASTNode* value = NULL;
    if (peek(p) != TOKEN_SEMICOLON) {
        value = parse_assignment_expr(p);
        if (!value) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected return expression");
            p->panic_mode = true;
            return NULL;
        }
    }
    if (!match(p, TOKEN_SEMICOLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ';' after return");
        p->panic_mode = true;
        free_ast_node(value);
        return NULL;
    }
    ReturnStmt* stmt = (ReturnStmt*)make_node(p, NODE_RETURN_STMT, sizeof(ReturnStmt));
    if (!stmt) {
        free_ast_node(value);
        return NULL;
    }
    stmt->value = value;
    return (ASTNode*)stmt;
}

/*
 * Parses a defer statement: defer ( block | statement ).
 */
ASTNode* parse_defer_stmt(Parser* p) {
    advance(p); // consume TOKEN_DEFER

    ASTNode* stmt = NULL;
    if (peek(p) == TOKEN_LBRACE) {
        stmt = parse_block(p);
    } else {
        stmt = parse_statement(p);
        if (!stmt) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected statement after defer");
            p->panic_mode = true;
        }
    }
    if (!stmt) {
        return NULL;
    }

    DeferStmt* d = (DeferStmt*)make_node(p, NODE_DEFER_STMT, sizeof(DeferStmt));
    if (!d) {
        free_ast_node(stmt);
        return NULL;
    }
    d->stmt = stmt;
    return (ASTNode*)d;
}

/*
 * Parses an asm statement: asm string ;.
 */
ASTNode* parse_asm_stmt(Parser* p) {
    advance(p); // consume TOKEN_ASM

    if (peek(p) != TOKEN_STRING_LITERAL) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected string literal for asm");
        p->panic_mode = true;
        return NULL;
    }
    char* code_str = strdup_custom(p->tokens[p->current].value);
    advance(p);

    LiteralExpr* lit = (LiteralExpr*)make_node(p, NODE_LITERAL, sizeof(LiteralExpr));
    if (!lit) {
        free(code_str);
        return NULL;
    }
    lit->kind = LIT_STRING;
    lit->string_val = code_str;
    lit->raw_value = NULL;

    if (!match(p, TOKEN_SEMICOLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ';' after asm");
        p->panic_mode = true;
        free_ast_node((ASTNode*)lit);
        return NULL;
    }

    AsmStmt* stmt = (AsmStmt*)make_node(p, NODE_ASM_STMT, sizeof(AsmStmt));
    if (!stmt) {
        free_ast_node((ASTNode*)lit);
        return NULL;
    }
    stmt->code = (ASTNode*)lit;
    return (ASTNode*)stmt;
}

/*
 * Parses a volatile statement: volatile ( block | statement ).
 */
ASTNode* parse_volatile_stmt(Parser* p) {
    advance(p); // consume TOKEN_VOLATILE

    ASTNode* stmt = NULL;
    if (peek(p) == TOKEN_LBRACE) {
        stmt = parse_block(p);
    } else {
        stmt = parse_statement(p);
        if (!stmt) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected statement after volatile");
            p->panic_mode = true;
        }
    }
    if (!stmt) {
        return NULL;
    }

    VolatileStmt* v = (VolatileStmt*)make_node(p, NODE_VOLATILE_STMT, sizeof(VolatileStmt));
    if (!v) {
        free_ast_node(stmt);
        return NULL;
    }
    v->stmt = stmt;
    return (ASTNode*)v;
}

/*
 * Parses a comptime statement: comptime [name] ( block | statement ).
 */
ASTNode* parse_comptime_stmt(Parser* p) {
    advance(p); // consume TOKEN_COMPTIME

    char* name = NULL;
    if (peek(p) == TOKEN_IDENTIFIER) {
        name = strdup_custom(p->tokens[p->current].value);
        advance(p);
    }

    ASTNode* stmt = NULL;
    if (peek(p) == TOKEN_LBRACE) {
        stmt = parse_block(p);
    } else {
        stmt = parse_statement(p);
        if (!stmt) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected statement after comptime");
            p->panic_mode = true;
        }
    }
    if (!stmt) {
        free(name);
        return NULL;
    }

    ComptimeStmt* cs = (ComptimeStmt*)make_node(p, NODE_COMPTIME_STMT, sizeof(ComptimeStmt));
    if (!cs) {
        free(name);
        free_ast_node(stmt);
        return NULL;
    }
    cs->name = name;
    cs->stmt = stmt;
    return (ASTNode*)cs;
}

/*
 * Parses a pass statement: pass ;.
 */
ASTNode* parse_pass_stmt(Parser* p) {
    advance(p); // consume TOKEN_PASS

    if (!match(p, TOKEN_SEMICOLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ';' after pass");
        p->panic_mode = true;
        return NULL;
    }
    PassStmt* stmt = (PassStmt*)make_node(p, NODE_PASS_STMT, sizeof(PassStmt));
    return (ASTNode*)stmt;
}

/*
 * Parses a free statement: free expression ;.
 */
ASTNode* parse_free_stmt(Parser* p) {
    advance(p); // consume TOKEN_FREE

    ASTNode* expr = parse_assignment_expr(p);
    if (!expr) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected expression after free");
        p->panic_mode = true;
        return NULL;
    }

    if (!match(p, TOKEN_SEMICOLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ';' after free expression");
        p->panic_mode = true;
        free_ast_node(expr);
        return NULL;
    }

    FreeStmt* stmt = (FreeStmt*)make_node(p, NODE_FREE_STMT, sizeof(FreeStmt));
    if (!stmt) {
        free_ast_node(expr);
        return NULL;
    }
    stmt->expr = expr;
    return (ASTNode*)stmt;
}

/*
 * Parses an expression statement: expression ;.
 */
ASTNode* parse_expr_stmt(Parser* p) {
    ASTNode* expr = parse_assignment_expr(p);
    if (!expr) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected expression");
        p->panic_mode = true;
        return NULL;
    }
    if (!match(p, TOKEN_SEMICOLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ';' after expression");
        p->panic_mode = true;
        free_ast_node(expr);
        return NULL;
    }
    ExprStmt* stmt = (ExprStmt*)make_node(p, NODE_EXPR_STMT, sizeof(ExprStmt));
    if (!stmt) {
        free_ast_node(expr);
        return NULL;
    }
    stmt->expr = expr;
    return (ASTNode*)stmt;
}
