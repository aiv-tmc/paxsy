#ifndef PARSER_INTERNAL_H
#define PARSER_INTERNAL_H

#include "parser.h"
#include "../../interfaces/errhandler/errhandler.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Parser state structure.
 * Contains token stream, current position, error recovery flags,
 * and a memory arena for AST nodes.
 */
typedef struct ParserArenaBlock {
    struct ParserArenaBlock* next;
    size_t capacity;
    size_t used;
    uint8_t data[];
} ParserArenaBlock;

typedef struct Parser {
    Token* tokens;          /* array of tokens from lexer */
    uint32_t token_count;   /* total number of tokens */
    uint32_t current;       /* index of next token to read */
    uint32_t line;          /* current line (for error reporting) */
    uint32_t column;        /* current column */
    bool panic_mode;        /* set when a syntax error requires recovery */
    bool had_error;         /* true if any syntax error was reported */
    uint8_t no_struct_init; /* nonzero in condition contexts: '{' opens a block, never a struct literal */

    ParserArenaBlock* arena; /* arena block list; blocks never move */
} Parser;

/*
 * If the lexer emitted a keyword as a plain identifier, resolve it
 * to the proper TokenType on the fly.  This makes the parser robust
 * regardless of how the lexer classifies words.
 */
static inline TokenType resolve_keyword(Token* tok) {
    if (tok->type != TOKEN_IDENTIFIER) return tok->type;
    const char* v = tok->value;

    /* declarations / control flow */
    if (strcmp(v, "use") == 0)       return TOKEN_USE;
    if (strcmp(v, "func") == 0)      return TOKEN_FUNC;
    if (strcmp(v, "let") == 0)       return TOKEN_LET;
    if (strcmp(v, "comptime") == 0)  return TOKEN_COMPTIME;
    if (strcmp(v, "if") == 0)        return TOKEN_IF;
    if (strcmp(v, "else") == 0)      return TOKEN_ELSE;
    if (strcmp(v, "match") == 0)     return TOKEN_MATCH;
    if (strcmp(v, "case") == 0)      return TOKEN_CASE;
    if (strcmp(v, "while") == 0)     return TOKEN_WHILE;
    if (strcmp(v, "do") == 0)        return TOKEN_DO;
    if (strcmp(v, "for") == 0)       return TOKEN_FOR;
    if (strcmp(v, "break") == 0)     return TOKEN_BREAK;
    if (strcmp(v, "continue") == 0)  return TOKEN_CONTINUE;
    if (strcmp(v, "return") == 0)    return TOKEN_RETURN;
    if (strcmp(v, "defer") == 0)     return TOKEN_DEFER;
    if (strcmp(v, "try") == 0)       return TOKEN_TRY;
    if (strcmp(v, "catch") == 0)     return TOKEN_CATCH;
    if (strcmp(v, "error") == 0)     return TOKEN_ERROR;
    if (strcmp(v, "volatile") == 0)  return TOKEN_VOLATILE;
    if (strcmp(v, "asm") == 0)       return TOKEN_ASM;
    if (strcmp(v, "alloc") == 0)     return TOKEN_ALLOC;
    if (strcmp(v, "free") == 0)      return TOKEN_FREE;
    if (strcmp(v, "pass") == 0)      return TOKEN_PASS;
    if (strcmp(v, "module") == 0)    return TOKEN_MODULE;
    if (strcmp(v, "extern") == 0)    return TOKEN_EXTERN;

    /* literals */
    if (strcmp(v, "none") == 0)      return TOKEN_NONE;
    if (strcmp(v, "true") == 0)      return TOKEN_TRUE;
    if (strcmp(v, "false") == 0)     return TOKEN_FALSE;

    /* operators */
    if (strcmp(v, "not") == 0)       return TOKEN_NOT;
    if (strcmp(v, "and") == 0)       return TOKEN_AND;
    if (strcmp(v, "or") == 0)        return TOKEN_OR;
    if (strcmp(v, "as") == 0)        return TOKEN_AS;

    /* built-in types */
    if (strcmp(v, "Int") == 0)       return TOKEN_INT;
    if (strcmp(v, "UInt") == 0)      return TOKEN_UINT;
    if (strcmp(v, "Real") == 0)      return TOKEN_REAL;
    if (strcmp(v, "Char") == 0)      return TOKEN_CHAR;
    if (strcmp(v, "Bool") == 0)      return TOKEN_BOOL;
    if (strcmp(v, "Void") == 0)      return TOKEN_VOID;
    if (strcmp(v, "Auto") == 0)      return TOKEN_AUTO;
    if (strcmp(v, "Int8") == 0)      return TOKEN_INT8;
    if (strcmp(v, "Int16") == 0)     return TOKEN_INT16;
    if (strcmp(v, "Int32") == 0)     return TOKEN_INT32;
    if (strcmp(v, "Int64") == 0)     return TOKEN_INT64;
    if (strcmp(v, "Int128") == 0)    return TOKEN_INT128;
    if (strcmp(v, "Int256") == 0)    return TOKEN_INT256;
    if (strcmp(v, "UInt8") == 0)     return TOKEN_UINT8;
    if (strcmp(v, "UInt16") == 0)    return TOKEN_UINT16;
    if (strcmp(v, "UInt32") == 0)    return TOKEN_UINT32;
    if (strcmp(v, "UInt64") == 0)    return TOKEN_UINT64;
    if (strcmp(v, "UInt128") == 0)   return TOKEN_UINT128;
    if (strcmp(v, "UInt256") == 0)   return TOKEN_UINT256;
    if (strcmp(v, "Real32") == 0)    return TOKEN_REAL32;
    if (strcmp(v, "Real64") == 0)    return TOKEN_REAL64;
    if (strcmp(v, "UTF8") == 0)      return TOKEN_UTF8;
    if (strcmp(v, "UTF16") == 0)     return TOKEN_UTF16;
    if (strcmp(v, "UTF32") == 0)     return TOKEN_UTF32;
    if (strcmp(v, "Struct") == 0)    return TOKEN_STRUCT;
    if (strcmp(v, "Union") == 0)     return TOKEN_UNION;
    if (strcmp(v, "Enum") == 0)      return TOKEN_ENUM;

    return TOKEN_IDENTIFIER;
}

/*
 * Inline helpers for token stream access.
 */
static inline TokenType peek(Parser* p) {
    return resolve_keyword(&p->tokens[p->current]);
}

static inline Token* advance(Parser* p) {
    if (p->current < p->token_count) {
        Token* tok = &p->tokens[p->current++];
        p->line = tok->line;
        p->column = tok->column;
        tok->type = resolve_keyword(tok);
        return tok;
    }
    return &p->tokens[p->token_count - 1];
}

static inline bool match(Parser* p, TokenType type) {
    if (peek(p) == type) {
        advance(p);
        return true;
    }
    return false;
}

static inline Token* expect(Parser* p, TokenType type, const char* msg) {
    if (peek(p) == type) {
        return advance(p);
    }
    errhandler__report_error(p->line, p->column, "parser", msg);
    p->panic_mode = true;
    return NULL;
}

/*
 * Synchronise after a syntax error: skip tokens until a recovery point.
 */
static inline void synchronize(Parser* p) {
    while (peek(p) != TOKEN_EOF) {
        TokenType t = peek(p);
        if (t == TOKEN_SEMICOLON || t == TOKEN_RBRACE || t == TOKEN_RBRACKET ||
            t == TOKEN_RPAREN || t == TOKEN_ELSE || t == TOKEN_CASE) {
            break;
        }
        advance(p);
    }
    TokenType t = peek(p);
    if (t == TOKEN_SEMICOLON || t == TOKEN_RBRACE || t == TOKEN_RBRACKET ||
        t == TOKEN_RPAREN) {
        advance(p);
    }
}

/*
 * Recover in a parse loop after a failed item: run synchronize() and, if it
 * made no progress (it stops at but does not consume else/case), consume one
 * token so the loop always terminates.
 */
static inline void synchronize_progress(Parser* p) {
    uint32_t before = p->current;
    synchronize(p);
    if (p->current == before && peek(p) != TOKEN_EOF) {
        advance(p);
    }
}

/*
 * Arena and AST node allocation.
 */
void* make_node(Parser* p, NodeType type, size_t size);
void append_child(Parser* p, void*** array, size_t* count, void* child);
char* strdup_custom(const char* s);
void* parser_alloc_raw(Parser* p, size_t size);
void parser_free_arena(ParserArenaBlock* blocks);
char* arena_strdup(Parser* p, const char* s, size_t len);

/*
 * Unified parser for the common pattern "name : Type".
 */
bool parse_name_type(Parser* p, char** out_name, ASTNode** out_type, uint32_t* out_mods);

/*
 * Parse a function signature: ( params ) : return_type.
 * Optionally consumes a name before '('.
 */
bool parse_function_signature(Parser* p, char** out_name, ASTNode*** out_params,
                              size_t* out_param_count, ASTNode** out_return_type,
                              uint32_t* out_mods, bool allow_mods,
                              char*** out_method_of, size_t* out_method_of_count);

/*
 * Free an AST node and all its children recursively.
 */
void free_ast_node(ASTNode* node);

/*
 * Top‑level parsing functions.
 */
ASTNode* parse_program(Parser* p);
ASTNode* parse_top_level_item(Parser* p);
ASTNode* parse_module_decl(Parser* p);
ASTNode* parse_use_decl(Parser* p);
ASTNode* parse_func_decl(Parser* p);
ASTNode* parse_let_decl(Parser* p);
ASTNode* parse_comptime_decl(Parser* p);
ASTNode* parse_extern_decl(Parser* p);

/*
 * Composite type definition parsers (composite.c).
 */
ASTNode* parse_struct_def(Parser* p);
ASTNode* parse_union_def(Parser* p);
ASTNode* parse_enum_def(Parser* p);

/*
 * Statement parsers.
 */
ASTNode* parse_block(Parser* p);
ASTNode* parse_statement(Parser* p);
ASTNode* parse_if_stmt(Parser* p);
ASTNode* parse_match_stmt(Parser* p);
ASTNode* parse_match_case(Parser* p);
ASTNode* parse_while_stmt(Parser* p);
ASTNode* parse_do_while_stmt(Parser* p);
ASTNode* parse_for_stmt(Parser* p);
ASTNode* parse_break_stmt(Parser* p);
ASTNode* parse_continue_stmt(Parser* p);
ASTNode* parse_return_stmt(Parser* p);
ASTNode* parse_defer_stmt(Parser* p);
ASTNode* parse_asm_stmt(Parser* p);
ASTNode* parse_volatile_stmt(Parser* p);
ASTNode* parse_comptime_stmt(Parser* p);
ASTNode* parse_pass_stmt(Parser* p);
ASTNode* parse_free_stmt(Parser* p);
ASTNode* parse_expr_stmt(Parser* p);

/*
 * Expression parsers.
 */
ASTNode* parse_assignment_expr(Parser* p);
ASTNode* parse_unary_expr(Parser* p);
ASTNode* parse_postfix_expr(Parser* p);
ASTNode* parse_primary_expr(Parser* p);
ASTNode* parse_range_expr(Parser* p);

/*
 * Parse a single case pattern: a literal, a named constant, or an
 * enum variant (possibly qualified). Returns an expression node.
 */
ASTNode* parse_pattern_expr(Parser* p);

/*
 * Type parser.
 */
ASTNode* parse_type_spec(Parser* p);
bool token_is_type_start(TokenType type);
bool token_is_builtin_type(TokenType type);

#endif
