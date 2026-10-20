#include "internal.h"
#include "../lexer/lexer.h"
#include "../lexer/escape.h"
#include <stdlib.h>
#include <string.h>

typedef enum {
    PREC_NONE = 0,
    PREC_ASSIGNMENT = 1,
    PREC_CAST = 2,
    PREC_SEND = 3,
    PREC_LOGICAL_OR = 4,
    PREC_LOGICAL_AND = 5,
    PREC_BITWISE_OR = 6,
    PREC_BITWISE_XOR = 7,
    PREC_BITWISE_AND = 8,
    PREC_EQUALITY = 9,
    PREC_RELATIONAL = 10,
    PREC_SHIFT = 11,
    PREC_ADDITIVE = 12,
    PREC_MULTIPLICATIVE = 13,
    PREC_UNARY = 14,
    PREC_POSTFIX = 15
} Precedence;

static ASTNode* parse_expression(Parser* p, Precedence min_prec);
static ASTNode* parse_call(Parser* p, ASTNode* callee);
static ASTNode* parse_index_or_slice(Parser* p, ASTNode* left);
static ASTNode* parse_struct_init(Parser* p, ASTNode* left);
static ASTNode* parse_if_expr_branch(Parser* p);
static ASTNode* parse_match_expr(Parser* p);

/*
 * Parse an if-expression branch: a single expression in braces.
 * The branch's value is the expression's value.
 */
static ASTNode* parse_if_expr_branch(Parser* p) {
    if (!match(p, TOKEN_LBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '{' for if expression branch");
        p->panic_mode = true;
        return NULL;
    }
    ASTNode* expr = parse_assignment_expr(p);
    if (!expr) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected expression in if expression branch");
        p->panic_mode = true;
        return NULL;
    }
    if (!match(p, TOKEN_RBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '}' after if expression branch");
        p->panic_mode = true;
        free_ast_node(expr);
        return NULL;
    }
    return expr;
}

/*
 * Parse a match expression:
 *   match subject { case pattern [or pattern] { expr } ... else { expr } }
 * The else branch is required: the expression must produce a value
 * on every path.
 */
static ASTNode* parse_match_expr(Parser* p) {
    advance(p); // consume 'match'
    uint8_t saved_nsi = p->no_struct_init;
    p->no_struct_init = 1;
    ASTNode* subject = parse_assignment_expr(p);
    p->no_struct_init = saved_nsi;
    if (!subject) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected match subject expression");
        p->panic_mode = true;
        return NULL;
    }
    if (!match(p, TOKEN_LBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '{' after match subject");
        p->panic_mode = true;
        free_ast_node(subject);
        return NULL;
    }
    MatchExpr* me = (MatchExpr*)make_node(p, NODE_MATCH_EXPR, sizeof(MatchExpr));
    if (!me) {
        free_ast_node(subject);
        return NULL;
    }
    me->subject = subject;
    me->cases = NULL;
    me->case_count = 0;
    me->else_value = NULL;
    while (peek(p) == TOKEN_CASE) {
        advance(p);
        MatchExprCase* mc = (MatchExprCase*)make_node(p, NODE_MATCH_EXPR_CASE, sizeof(MatchExprCase));
        if (!mc) {
            return (ASTNode*)me;
        }
        mc->patterns = NULL;
        mc->pattern_count = 0;
        do {
            ASTNode* pat = parse_pattern_expr(p);
            if (!pat) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected pattern in case");
                p->panic_mode = true;
                break;
            }
            append_child(p, (void***)&mc->patterns, &mc->pattern_count, pat);
        } while (match(p, TOKEN_OR));
        mc->value = parse_if_expr_branch(p);
        if (!mc->value) {
            return (ASTNode*)me;
        }
        append_child(p, (void***)&me->cases, &me->case_count, mc);
    }
    if (!match(p, TOKEN_ELSE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Match expression requires an else branch");
        p->panic_mode = true;
        /* Recover at the closing brace so the enclosing statement
           can continue after the match; the recovery is complete,
           so no further synchronisation is needed. */
        while (peek(p) != TOKEN_RBRACE && peek(p) != TOKEN_EOF) {
            advance(p);
        }
        match(p, TOKEN_RBRACE);
        p->panic_mode = false;
        return (ASTNode*)me;
    }
    me->else_value = parse_if_expr_branch(p);
    if (!me->else_value) {
        return (ASTNode*)me;
    }
    if (!match(p, TOKEN_RBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '}' to close match expression");
        p->panic_mode = true;
    }
    return (ASTNode*)me;
}

static ASTNode* parse_expression_from_substring(Parser* main_p, const char* str, size_t len) {
    char* buf = (char*)malloc(len + 1);
    if (!buf) {
        errhandler__report_error(main_p->line, main_p->column, "parser",
                                 "Failed to allocate temporary string");
        return NULL;
    }
    memcpy(buf, str, len);
    buf[len] = '\0';
    Lexer* temp_lexer = lexer__init_lexer(buf);
    if (!temp_lexer) {
        free(buf);
        errhandler__report_error(main_p->line, main_p->column, "parser",
                                 "Failed to create temporary lexer");
        return NULL;
    }
    lexer__tokenize(temp_lexer);
    Token* old_tokens = main_p->tokens;
    uint32_t old_count = main_p->token_count;
    uint32_t old_current = main_p->current;
    main_p->tokens = temp_lexer->tokens;
    main_p->token_count = (uint32_t)temp_lexer->token_count;
    main_p->current = 0;
    ASTNode* expr = parse_assignment_expr(main_p);
    main_p->tokens = old_tokens;
    main_p->token_count = old_count;
    main_p->current = old_current;
    lexer__free_lexer(temp_lexer);
    free(buf);
    return expr;
}

/*
 * Append a text segment to an interpolated string, decoding its escape
 * sequences. 'line'/'column' locate the segment's first byte in the source.
 */
static void append_text_segment(Parser* p, InterpolatedString* node,
                                const char* text_start, size_t text_len,
                                uint32_t line, uint32_t column) {
    uint32_t decoded_len = 0;
    char* decoded = escape__decode(text_start, (uint32_t)text_len,
                                   line, column, false, &decoded_len);
    if (!decoded) {
        errhandler__report_error(p->line, p->column, "parser",
                                 "Failed to allocate temporary string");
        p->panic_mode = true;
        return;
    }
    InterpolatedSegment* seg = (InterpolatedSegment*)parser_alloc_raw(p, sizeof(InterpolatedSegment));
    seg->kind = SEG_TEXT;
    seg->text = arena_strdup(p, decoded, decoded_len);
    seg->expr = NULL;
    free(decoded);
    append_child(p, (void***)&node->segments, &node->segment_count, seg);
}

static void parse_interpolated_string(Parser* p, const char* str, InterpolatedString* node) {
    /* Called right after advancing past the string token: p->line/p->column
     * hold the token's start position. */
    uint32_t tok_line = p->line;
    uint32_t tok_col = p->column;
    size_t len = strlen(str);
    size_t pos = 0;
    const char* text_start = str;
    size_t text_len = 0;
    while (pos < len) {
        if (str[pos] == '{') {
            if (text_len > 0) {
                append_text_segment(p, node, text_start, text_len,
                                    tok_line,
                                    tok_col + 1 + (uint32_t)(text_start - str));
                text_len = 0;
            }
            int depth = 1;
            size_t expr_start = pos + 1;
            pos++;
            while (pos < len && depth > 0) {
                if (str[pos] == '{') depth++;
                else if (str[pos] == '}') depth--;
                pos++;
            }
            if (depth != 0) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Unclosed brace in interpolated string");
                p->panic_mode = true;
                return;
            }
            size_t expr_len = pos - expr_start - 1;
            const char* expr_str = str + expr_start;
            ASTNode* expr_node = parse_expression_from_substring(p, expr_str, expr_len);
            if (!expr_node) return;
            InterpolatedSegment* seg = (InterpolatedSegment*)parser_alloc_raw(p, sizeof(InterpolatedSegment));
            seg->kind = SEG_EXPR;
            seg->text = NULL;
            seg->expr = expr_node;
            append_child(p, (void***)&node->segments, &node->segment_count, seg);
            text_start = str + pos;
            text_len = 0;
        } else if (str[pos] == '}') {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Unexpected '}' in interpolated string");
            p->panic_mode = true;
            return;
        } else {
            pos++;
            text_len++;
        }
    }
    if (text_len > 0) {
        append_text_segment(p, node, text_start, text_len,
                            tok_line,
                            tok_col + 1 + (uint32_t)(text_start - str));
    }
}

ASTNode* parse_assignment_expr(Parser* p) { return parse_expression(p, PREC_ASSIGNMENT); }

ASTNode* parse_unary_expr(Parser* p) {
    TokenType type = peek(p);
    if (type == TOKEN_TRY) {
        advance(p);
        ASTNode* expr = parse_unary_expr(p);
        if (!expr) {
            errhandler__report_error(p->line, p->column, "parser",
                                     "Expected expression after try");
            p->panic_mode = true;
            return NULL;
        }
        TryExpr* te = (TryExpr*)make_node(p, NODE_TRY_EXPR, sizeof(TryExpr));
        if (!te) { free_ast_node(expr); return NULL; }
        te->expr = expr;
        return (ASTNode*)te;
    }
    if (type == TOKEN_PLUS || type == TOKEN_MINUS || type == TOKEN_NOT ||
        type == TOKEN_TILDE || type == TOKEN_AMPERSAND || type == TOKEN_AT) {
        Token* tok = advance(p);
        UnaryOp op;
        switch (tok->type) {
            case TOKEN_PLUS:  op = UNARY_PLUS; break;
            case TOKEN_MINUS: op = UNARY_MINUS; break;
            case TOKEN_NOT:   op = UNARY_LOGICAL_NOT; break;
            case TOKEN_TILDE: op = UNARY_BIT_NOT; break;
            case TOKEN_AMPERSAND: op = UNARY_ADDR; break;
            case TOKEN_AT:    op = UNARY_DEREF; break;
            default: op = UNARY_PLUS; break;
        }
        ASTNode* operand = parse_unary_expr(p);
        if (!operand) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected operand after unary operator");
            p->panic_mode = true;
            return NULL;
        }
        UnaryExpr* u = (UnaryExpr*)make_node(p, NODE_UNARY, sizeof(UnaryExpr));
        if (!u) { free_ast_node(operand); return NULL; }
        u->op = op;
        u->operand = operand;
        return (ASTNode*)u;
    } else if (type == TOKEN_BANG) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Unexpected '!' operator (use 'not')");
        p->panic_mode = true;
        advance(p);
        return NULL;
    } else {
        return parse_postfix_expr(p);
    }
}

static ASTNode* parse_call(Parser* p, ASTNode* callee) {
    advance(p);
    CallExpr* call = (CallExpr*)make_node(p, NODE_CALL, sizeof(CallExpr));
    if (!call) { free_ast_node(callee); return NULL; }
    call->callee = callee;
    call->args = NULL;
    call->arg_count = 0;
    if (peek(p) != TOKEN_RPAREN) {
        do {
            ASTNode* arg = parse_assignment_expr(p);
            if (!arg) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected argument expression");
                break;
            }
            append_child(p, (void***)&call->args, &call->arg_count, arg);
        } while (match(p, TOKEN_COMMA) && peek(p) != TOKEN_RPAREN);
    }
    if (!match(p, TOKEN_RPAREN)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ')' after arguments");
        p->panic_mode = true;
    }
    return (ASTNode*)call;
}

/*
 * Parse a struct or union literal:
 *   Type {expr, ...}            positional, values in field order
 *   Type {.name = expr, ...}    named, fields in any order
 * 'left' is the type designator (an identifier or a module-qualified
 * name). Consumes the opening '{'. Returns a NODE_STRUCT_INIT node
 * or NULL after reporting an error.
 */
static ASTNode* parse_struct_init(Parser* p, ASTNode* left) {
    advance(p); // consume '{'
    StructInitExpr* init = (StructInitExpr*)make_node(p, NODE_STRUCT_INIT, sizeof(StructInitExpr));
    if (!init) { free_ast_node(left); return NULL; }
    init->struct_type = left;
    init->field_names = NULL;
    init->values = NULL;
    init->count = 0;
    init->named = (peek(p) == TOKEN_DOT) ? 1 : 0;
    while (peek(p) != TOKEN_RBRACE && peek(p) != TOKEN_EOF) {
        char* fname = NULL;
        if (init->named) {
            if (!match(p, TOKEN_DOT)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected '.' for named field");
                p->panic_mode = true;
                break;
            }
            if (!match(p, TOKEN_IDENTIFIER)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected field name");
                p->panic_mode = true;
                break;
            }
            fname = strdup_custom(p->tokens[p->current - 1].value);
            if (!match(p, TOKEN_EQUAL)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected '=' after field name");
                free(fname);
                p->panic_mode = true;
                break;
            }
        }
        ASTNode* val = parse_assignment_expr(p);
        if (!val) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected field value");
            free(fname);
            p->panic_mode = true;
            break;
        }
        if (init->named) {
            size_t name_count = init->count;
            append_child(p, (void***)&init->field_names, &name_count, fname);
        }
        append_child(p, (void***)&init->values, &init->count, val);
        if (!match(p, TOKEN_COMMA)) break;
        if (peek(p) == TOKEN_RBRACE) break;
    }
    if (!match(p, TOKEN_RBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '}' to close struct literal");
        p->panic_mode = true;
    }
    return (ASTNode*)init;
}

static ASTNode* parse_index_or_slice(Parser* p, ASTNode* left) {
    advance(p); // consume '['
    if (peek(p) == TOKEN_RANGE) {
        ASTNode* range = parse_range_expr(p);
        if (!range) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected range expression in slice");
            p->panic_mode = true;
            free_ast_node(left);
            return NULL;
        }
        if (!match(p, TOKEN_RBRACKET)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected ']' after slice");
            p->panic_mode = true;
            free_ast_node(left);
            free_ast_node(range);
            return NULL;
        }
        SliceExpr* slice = (SliceExpr*)make_node(p, NODE_SLICE, sizeof(SliceExpr));
        if (!slice) {
            free_ast_node(left);
            free_ast_node(range);
            return NULL;
        }
        slice->array = left;
        slice->range = range;
        return (ASTNode*)slice;
    }
    /* The first bound is parsed above relational precedence so that
       a postfix '>' is the range modifier, not a comparison: a[5>..] */
    ASTNode* first = parse_expression(p, PREC_SHIFT);
    if (!first) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected expression in brackets");
        p->panic_mode = true;
        free_ast_node(left);
        return NULL;
    }
    uint8_t exclude_start = 0;
    if (match(p, TOKEN_GREATER)) {
        exclude_start = 1;
    }
    if (peek(p) == TOKEN_RANGE) {
        advance(p);
        uint8_t exclude_end = 0;
        if (match(p, TOKEN_LESS)) {
            exclude_end = 1;
        }
        ASTNode* end_expr = NULL;
        if (peek(p) != TOKEN_RBRACKET) {
            end_expr = parse_assignment_expr(p);
            if (!end_expr) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected end expression in slice");
                p->panic_mode = true;
                free_ast_node(left);
                free_ast_node(first);
                return NULL;
            }
        }
        RangeExpr* range = (RangeExpr*)make_node(p, NODE_RANGE, sizeof(RangeExpr));
        if (!range) {
            free_ast_node(left);
            free_ast_node(first);
            free_ast_node(end_expr);
            return NULL;
        }
        range->start = first;
        range->end = end_expr;
        range->exclude_start = exclude_start;
        range->exclude_end = exclude_end;
        if (!match(p, TOKEN_RBRACKET)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected ']' after slice");
            p->panic_mode = true;
            free_ast_node(left);
            free_ast_node((ASTNode*)range);
            return NULL;
        }
        SliceExpr* slice = (SliceExpr*)make_node(p, NODE_SLICE, sizeof(SliceExpr));
        if (!slice) {
            free_ast_node(left);
            free_ast_node((ASTNode*)range);
            return NULL;
        }
        slice->array = left;
        slice->range = (ASTNode*)range;
        return (ASTNode*)slice;
    }
    if (!match(p, TOKEN_RBRACKET)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ']' after index expression");
        p->panic_mode = true;
        free_ast_node(left);
        free_ast_node(first);
        return NULL;
    }
    IndexExpr* idx = (IndexExpr*)make_node(p, NODE_INDEX, sizeof(IndexExpr));
    if (!idx) {
        free_ast_node(left);
        free_ast_node(first);
        return NULL;
    }
    idx->array = left;
    idx->index = first;
    return (ASTNode*)idx;
}

ASTNode* parse_postfix_expr(Parser* p) {
    ASTNode* left = parse_primary_expr(p);
    if (!left) return NULL;
    while (1) {
        TokenType type = peek(p);
        if (type == TOKEN_LPAREN) {
            /* Arguments are nested subexpressions: struct literals allowed. */
            uint8_t saved_nsi = p->no_struct_init;
            p->no_struct_init = 0;
            left = parse_call(p, left);
            p->no_struct_init = saved_nsi;
        } else if (type == TOKEN_LBRACKET) {
            /* Index and slice bounds are nested subexpressions. */
            uint8_t saved_nsi = p->no_struct_init;
            p->no_struct_init = 0;
            left = parse_index_or_slice(p, left);
            p->no_struct_init = saved_nsi;
        } else if (type == TOKEN_LBRACE && !p->no_struct_init &&
                   (left->type == NODE_IDENT || left->type == NODE_MODULE_ACCESS)) {
            left = parse_struct_init(p, left);
        } else if (type == TOKEN_DOT) {
            advance(p);
            if (!match(p, TOKEN_IDENTIFIER)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected member name after '.'");
                p->panic_mode = true;
                free_ast_node(left);
                return NULL;
            }
            char* member = strdup_custom(p->tokens[p->current - 1].value);
            MemberAccess* ma = (MemberAccess*)make_node(p, NODE_MEMBER_ACCESS, sizeof(MemberAccess));
            if (!ma) { free(member); free_ast_node(left); return NULL; }
            ma->object = left;
            ma->member = member;
            left = (ASTNode*)ma;
        } else if (type == TOKEN_CATCH) {
            advance(p);
            if (!match(p, TOKEN_PIPE)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected '|' after catch");
                p->panic_mode = true;
                free_ast_node(left);
                return NULL;
            }
            if (!match(p, TOKEN_IDENTIFIER)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected variable name in catch");
                p->panic_mode = true;
                free_ast_node(left);
                return NULL;
            }
            char* var = strdup_custom(p->tokens[p->current - 1].value);
            if (!match(p, TOKEN_PIPE)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected '|' after catch variable");
                p->panic_mode = true;
                free(var);
                free_ast_node(left);
                return NULL;
            }
            ASTNode* handler = parse_block(p);
            if (!handler) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected block after catch");
                p->panic_mode = true;
                free(var);
                free_ast_node(left);
                return NULL;
            }
            CatchExpr* ce = (CatchExpr*)make_node(p, NODE_CATCH_EXPR, sizeof(CatchExpr));
            if (!ce) {
                free(var);
                free_ast_node(left);
                free_ast_node(handler);
                return NULL;
            }
            ce->expr = left;
            ce->var_name = var;
            ce->handler = handler;
            left = (ASTNode*)ce;
        } else {
            break;
        }
    }
    return left;
}

ASTNode* parse_primary_expr(Parser* p) {
    TokenType type = peek(p);
    if (type == TOKEN_IDENTIFIER) {
        Token* tok = advance(p);
        char* name = strdup_custom(tok->value);
        if (match(p, TOKEN_COLON_COLON)) {
            char** path = NULL;
            size_t path_len = 0;
            append_child(p, (void***)&path, &path_len, name);
            while (1) {
                if (!match(p, TOKEN_IDENTIFIER)) {
                    errhandler__report_error(p->line, p->column, "parser",
                                             "Expected identifier after '::'");
                    break;
                }
                char* part = strdup_custom(p->tokens[p->current - 1].value);
                append_child(p, (void***)&path, &path_len, part);
                if (!match(p, TOKEN_COLON_COLON)) break;
            }
            if (path_len == 0) return NULL;
            char* symbol = path[path_len - 1];
            char** new_path = NULL;
            if (path_len > 1) {
                new_path = (char**)parser_alloc_raw(p, (path_len - 1) * sizeof(char*));
                if (!new_path) {
                    free(symbol);
                    for (size_t i = 0; i < path_len - 1; ++i) free(path[i]);
                    return NULL;
                }
                for (size_t i = 0; i < path_len - 1; ++i) new_path[i] = path[i];
            }
            ModuleAccess* ma = (ModuleAccess*)make_node(p, NODE_MODULE_ACCESS, sizeof(ModuleAccess));
            if (!ma) { free(symbol); return NULL; }
            ma->path = new_path;
            ma->path_len = path_len - 1;
            ma->symbol = symbol;
            return (ASTNode*)ma;
        } else {
            IdentExpr* id = (IdentExpr*)make_node(p, NODE_IDENT, sizeof(IdentExpr));
            if (!id) { free(name); return NULL; }
            id->name = name;
            return (ASTNode*)id;
        }
    } else if (type == TOKEN_NUMBER_LITERAL) {
        Token* tok = advance(p);
        LiteralExpr* lit = (LiteralExpr*)make_node(p, NODE_LITERAL, sizeof(LiteralExpr));
        if (!lit) return NULL;
        lit->kind = LIT_NUMBER;
        lit->raw_value = strdup_custom(tok->value);
        lit->string_val = NULL;
        return (ASTNode*)lit;
    } else if (type == TOKEN_STRING_LITERAL) {
        Token* tok = advance(p);
        LiteralExpr* lit = (LiteralExpr*)make_node(p, NODE_LITERAL, sizeof(LiteralExpr));
        if (!lit) return NULL;
        lit->kind = LIT_STRING;
        lit->string_val = strdup_custom(tok->value);
        lit->raw_value = NULL;
        return (ASTNode*)lit;
    } else if (type == TOKEN_INTERPOLATED_STRING) {
        Token* tok = advance(p);
        InterpolatedString* is = (InterpolatedString*)make_node(p, NODE_INTERPOLATED_STRING, sizeof(InterpolatedString));
        if (!is) return NULL;
        is->segments = NULL;
        is->segment_count = 0;
        parse_interpolated_string(p, tok->value, is);
        return (ASTNode*)is;
    } else if (type == TOKEN_CHAR_LITERAL) {
        Token* tok = advance(p);
        LiteralExpr* lit = (LiteralExpr*)make_node(p, NODE_LITERAL, sizeof(LiteralExpr));
        if (!lit) return NULL;
        lit->kind = LIT_CHAR;
        lit->string_val = strdup_custom(tok->value);
        lit->raw_value = NULL;
        return (ASTNode*)lit;
    } else if (type == TOKEN_TRUE || type == TOKEN_FALSE) {
        Token* tok = advance(p);
        LiteralExpr* lit = (LiteralExpr*)make_node(p, NODE_LITERAL, sizeof(LiteralExpr));
        if (!lit) return NULL;
        lit->kind = LIT_BOOL;
        lit->bool_val = (tok->type == TOKEN_TRUE) ? 1 : 0;
        lit->raw_value = NULL;
        return (ASTNode*)lit;
    } else if (type == TOKEN_NONE) {
        advance(p);
        LiteralExpr* lit = (LiteralExpr*)make_node(p, NODE_LITERAL, sizeof(LiteralExpr));
        if (!lit) return NULL;
        lit->kind = LIT_NONE;
        return (ASTNode*)lit;
    } else if (type == TOKEN_LPAREN) {
        advance(p);
        /* Parenthesised subexpressions may contain struct literals. */
        uint8_t saved_nsi = p->no_struct_init;
        p->no_struct_init = 0;
        ASTNode* expr = parse_assignment_expr(p);
        p->no_struct_init = saved_nsi;
        if (!match(p, TOKEN_RPAREN)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected ')' after expression");
            p->panic_mode = true;
            free_ast_node(expr);
            return NULL;
        }
        return expr;
    } else if (type == TOKEN_MATCH) {
        return parse_match_expr(p);
    } else if (type == TOKEN_IF) {
        advance(p);
        /* '{' after the condition opens the then branch, not a struct literal. */
        uint8_t saved_nsi = p->no_struct_init;
        p->no_struct_init = 1;
        ASTNode* cond = parse_assignment_expr(p);
        p->no_struct_init = saved_nsi;
        if (!cond) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected condition in if expression");
            p->panic_mode = true;
            return NULL;
        }
        /* Branches are single expressions in braces. With no branch
           at all the expression has type Bool (the condition's value). */
        ASTNode* then_expr = NULL;
        ASTNode* else_expr = NULL;
        if (peek(p) == TOKEN_LBRACE) {
            then_expr = parse_if_expr_branch(p);
            if (!then_expr) {
                free_ast_node(cond);
                return NULL;
            }
            if (match(p, TOKEN_ELSE)) {
                else_expr = parse_if_expr_branch(p);
                if (!else_expr) {
                    free_ast_node(cond);
                    free_ast_node(then_expr);
                    return NULL;
                }
            }
        }
        IfExpr* ie = (IfExpr*)make_node(p, NODE_IF_EXPR, sizeof(IfExpr));
        if (!ie) {
            free_ast_node(cond);
            free_ast_node(then_expr);
            free_ast_node(else_expr);
            return NULL;
        }
        ie->condition = cond;
        ie->then_expr = then_expr;
        ie->else_expr = else_expr;
        return (ASTNode*)ie;
    } else if (type == TOKEN_ERROR) {
        advance(p);
        ASTNode* code = parse_assignment_expr(p);
        if (!code) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected error code expression");
            p->panic_mode = true;
            return NULL;
        }
        ErrorExpr* ee = (ErrorExpr*)make_node(p, NODE_ERROR_EXPR, sizeof(ErrorExpr));
        if (!ee) { free_ast_node(code); return NULL; }
        ee->code = code;
        return (ASTNode*)ee;
    } else if (type == TOKEN_LBRACE) {
        /* Brace literal: comma-separated element expressions. */
        advance(p);
        ArrayLiteral* arr = (ArrayLiteral*)make_node(p, NODE_ARRAY_LITERAL, sizeof(ArrayLiteral));
        if (!arr) return NULL;
        arr->elements = NULL;
        arr->count = 0;
        while (peek(p) != TOKEN_RBRACE && peek(p) != TOKEN_EOF) {
            ASTNode* elem = parse_assignment_expr(p);
            if (!elem) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected expression in brace literal");
                p->panic_mode = true;
                break;
            }
            append_child(p, (void***)&arr->elements, &arr->count, elem);
            if (!match(p, TOKEN_COMMA)) break;
        }
        if (!match(p, TOKEN_RBRACE)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected '}' to close brace literal");
            p->panic_mode = true;
        }
        return (ASTNode*)arr;
    } else if (type == TOKEN_ALLOC) {
        /* Heap allocation: alloc T[N] allocates N elements of type T
           and yields a &T pointer to the first element, or the zero
           pointer on failure. The memory is uninitialised. */
        advance(p);
        AllocExpr* alloc = (AllocExpr*)make_node(p, NODE_ALLOC_EXPR, sizeof(AllocExpr));
        if (!alloc) return NULL;
        alloc->type = parse_type_spec(p);
        if (!alloc->type) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected element type after 'alloc'");
            p->panic_mode = true;
            return NULL;
        }
        /* The outermost bracket pair of T[N] is the element count, not
           part of the element type: alloc Int32[4] allocates four
           Int32 values, and alloc Int32[4][10] allocates ten Int32[4]. */
        if (alloc->type->type != NODE_ARRAY_TYPE || ((ArrayType*)alloc->type)->size == NULL) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected element count: alloc T[N]");
            p->panic_mode = true;
            return NULL;
        }
        {
            ArrayType* at = (ArrayType*)alloc->type;
            alloc->type = at->base_type;
            alloc->count = at->size;
            at->base_type = NULL;
            at->size = NULL;
        }
        return (ASTNode*)alloc;
    } else if (token_is_builtin_type(type)) {
        /* A type name in expression position: valid as a compile-time
           argument (comptime parameters, builtins). */
        return parse_type_spec(p);
    } else {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Unexpected token in expression");
        p->panic_mode = true;
        return NULL;
    }
}

ASTNode* parse_range_expr(Parser* p) {
    uint8_t exclude_start = 0, exclude_end = 0;
    ASTNode* start = NULL;
    ASTNode* end = NULL;
    if (peek(p) != TOKEN_RANGE) {
        /* The start is parsed above relational precedence so that a
           postfix '>' is the range modifier, not a comparison: 5>.. */
        start = parse_expression(p, PREC_SHIFT);
        if (!start) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected start of range");
            return NULL;
        }
        if (match(p, TOKEN_GREATER)) {
            exclude_start = 1;
        }
    }
    if (!match(p, TOKEN_RANGE)) {
        errhandler__report_error(p->line, p->column, "syntax", "Expected '..' in range");
        free_ast_node(start);
        return NULL;
    }
    if (match(p, TOKEN_LESS)) exclude_end = 1;
    if (peek(p) != TOKEN_RBRACKET && peek(p) != TOKEN_COMMA && peek(p) != TOKEN_PIPE &&
        peek(p) != TOKEN_SEMICOLON && peek(p) != TOKEN_RPAREN) {
        end = parse_assignment_expr(p);
        if (!end) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected end of range");
            free_ast_node(start);
            return NULL;
        }
    }
    RangeExpr* r = (RangeExpr*)make_node(p, NODE_RANGE, sizeof(RangeExpr));
    if (!r) {
        free_ast_node(start);
        free_ast_node(end);
        return NULL;
    }
    r->start = start;
    r->end = end;
    r->exclude_start = exclude_start;
    r->exclude_end = exclude_end;
    return (ASTNode*)r;
}

static ASTNode* parse_expression(Parser* p, Precedence min_prec) {
    ASTNode* left = parse_unary_expr(p);
    if (!left) return NULL;
    while (1) {
        TokenType type = peek(p);
        Precedence prec;
        bool right_assoc = false;
        switch (type) {
            case TOKEN_EQUAL:
            case TOKEN_PLUS_EQUAL: case TOKEN_MINUS_EQUAL: case TOKEN_STAR_EQUAL:
            case TOKEN_SLASH_EQUAL: case TOKEN_PERCENT_EQUAL: case TOKEN_DOLLAR_EQUAL:
            case TOKEN_AMPERSAND_EQUAL: case TOKEN_CARET_EQUAL: case TOKEN_LSHIFT_EQUAL:
            case TOKEN_RSHIFT_EQUAL: case TOKEN_LSHIFT_LOGICAL_EQUAL: case TOKEN_RSHIFT_LOGICAL_EQUAL:
                prec = PREC_ASSIGNMENT; right_assoc = true; break;
            case TOKEN_AS: prec = PREC_CAST; right_assoc = false; break;
            case TOKEN_ARROW: prec = PREC_SEND; right_assoc = false; break;
            case TOKEN_OR: prec = PREC_LOGICAL_OR; right_assoc = false; break;
            case TOKEN_AND: prec = PREC_LOGICAL_AND; right_assoc = false; break;
            case TOKEN_DOLLAR: prec = PREC_BITWISE_OR; right_assoc = false; break;
            case TOKEN_CARET: prec = PREC_BITWISE_XOR; right_assoc = false; break;
            case TOKEN_AMPERSAND: prec = PREC_BITWISE_AND; right_assoc = false; break;
            case TOKEN_EQUAL_EQUAL: case TOKEN_NOT_EQUAL:
                prec = PREC_EQUALITY; right_assoc = false; break;
            case TOKEN_LESS: case TOKEN_GREATER: case TOKEN_LESS_EQUAL: case TOKEN_GREATER_EQUAL:
                prec = PREC_RELATIONAL; right_assoc = false; break;
            case TOKEN_LSHIFT: case TOKEN_RSHIFT: case TOKEN_LSHIFT_LOGICAL: case TOKEN_RSHIFT_LOGICAL:
                prec = PREC_SHIFT; right_assoc = false; break;
            case TOKEN_PLUS: case TOKEN_MINUS:
                prec = PREC_ADDITIVE; right_assoc = false; break;
            case TOKEN_STAR: case TOKEN_SLASH: case TOKEN_PERCENT:
                prec = PREC_MULTIPLICATIVE; right_assoc = false; break;
            default: prec = PREC_NONE; break;
        }
        if (prec == PREC_NONE || prec < min_prec) break;
        if (type == TOKEN_AS) {
            advance(p);
            ASTNode* type_node = parse_type_spec(p);
            if (!type_node) { free_ast_node(left); return NULL; }
            CastExpr* cast = (CastExpr*)make_node(p, NODE_CAST, sizeof(CastExpr));
            if (!cast) { free_ast_node(left); free_ast_node(type_node); return NULL; }
            cast->expr = left;
            cast->target_type = type_node;
            left = (ASTNode*)cast;
            min_prec = (Precedence)(prec + 1);
            continue;
        }
        Token* op_tok = advance(p);
        Precedence next_min = right_assoc ? prec : (Precedence)(prec + 1);
        ASTNode* right = parse_expression(p, next_min);
        if (!right) { free_ast_node(left); return NULL; }
        if (prec == PREC_ASSIGNMENT) {
            AssignOp op;
            switch (op_tok->type) {
                case TOKEN_EQUAL: op = ASSIGN_NORMAL; break;
                case TOKEN_PLUS_EQUAL: op = ASSIGN_ADD; break;
                case TOKEN_MINUS_EQUAL: op = ASSIGN_SUB; break;
                case TOKEN_STAR_EQUAL: op = ASSIGN_MUL; break;
                case TOKEN_SLASH_EQUAL: op = ASSIGN_DIV; break;
                case TOKEN_PERCENT_EQUAL: op = ASSIGN_MOD; break;
                case TOKEN_DOLLAR_EQUAL: op = ASSIGN_BIT_OR; break;
                case TOKEN_AMPERSAND_EQUAL: op = ASSIGN_BIT_AND; break;
                case TOKEN_CARET_EQUAL: op = ASSIGN_BIT_XOR; break;
                case TOKEN_LSHIFT_EQUAL: op = ASSIGN_SHL; break;
                case TOKEN_RSHIFT_EQUAL: op = ASSIGN_SHR; break;
                case TOKEN_LSHIFT_LOGICAL_EQUAL: op = ASSIGN_SHL_LOG; break;
                case TOKEN_RSHIFT_LOGICAL_EQUAL: op = ASSIGN_SHR_LOG; break;
                default: op = ASSIGN_NORMAL; break;
            }
            AssignExpr* a = (AssignExpr*)make_node(p, NODE_ASSIGN, sizeof(AssignExpr));
            if (!a) { free_ast_node(left); free_ast_node(right); return NULL; }
            a->op = op; a->left = left; a->right = right;
            left = (ASTNode*)a;
        } else if (prec == PREC_SEND) {
            SendExpr* s = (SendExpr*)make_node(p, NODE_SEND, sizeof(SendExpr));
            if (!s) { free_ast_node(left); free_ast_node(right); return NULL; }
            s->target = left; s->message = right;
            left = (ASTNode*)s;
        } else {
            BinaryOp op;
            switch (op_tok->type) {
                case TOKEN_OR: op = BIN_LOGICAL_OR; break;
                case TOKEN_AND: op = BIN_LOGICAL_AND; break;
                case TOKEN_DOLLAR: op = BIN_OR; break;
                case TOKEN_CARET: op = BIN_XOR; break;
                case TOKEN_AMPERSAND: op = BIN_AND; break;
                case TOKEN_EQUAL_EQUAL: op = BIN_EQ; break;
                case TOKEN_NOT_EQUAL: op = BIN_NE; break;
                case TOKEN_LESS: op = BIN_LT; break;
                case TOKEN_GREATER: op = BIN_GT; break;
                case TOKEN_LESS_EQUAL: op = BIN_LE; break;
                case TOKEN_GREATER_EQUAL: op = BIN_GE; break;
                case TOKEN_LSHIFT: op = BIN_SHL; break;
                case TOKEN_RSHIFT: op = BIN_SHR; break;
                case TOKEN_LSHIFT_LOGICAL: op = BIN_SHL_LOG; break;
                case TOKEN_RSHIFT_LOGICAL: op = BIN_SHR_LOG; break;
                case TOKEN_PLUS: op = BIN_ADD; break;
                case TOKEN_MINUS: op = BIN_SUB; break;
                case TOKEN_STAR: op = BIN_MUL; break;
                case TOKEN_SLASH: op = BIN_DIV; break;
                case TOKEN_PERCENT: op = BIN_MOD; break;
                default: op = BIN_ADD; break;
            }
            BinaryExpr* b = (BinaryExpr*)make_node(p, NODE_BINARY, sizeof(BinaryExpr));
            if (!b) { free_ast_node(left); free_ast_node(right); return NULL; }
            b->op = op; b->left = left; b->right = right;
            left = (ASTNode*)b;
        }
    }
    return left;
}
