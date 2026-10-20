#include "internal.h"
#include <stdlib.h>

/*
 * Entry point: parses the token stream into a Program AST node.
 */
Program* parse(Token* tokens, size_t token_count) {
    Parser parser;
    parser.tokens = tokens;
    parser.token_count = (uint32_t)token_count;
    parser.current = 0;
    parser.line = 1;
    parser.column = 1;
    parser.panic_mode = false;
    parser.had_error = false;
    parser.no_struct_init = 0;

    parser.arena = NULL;

    ASTNode* prog_node = parse_program(&parser);
    if (!prog_node || parser.had_error) {
        /* Nodes live in the arena; strings are freed by free_ast_node */
        if (prog_node) free_ast_node(prog_node);
        parser_free_arena(parser.arena);
        return NULL;
    }
    /* The arena outlives the parser: hand it to the Program node */
    ((Program*)prog_node)->arena_blocks = parser.arena;
    return (Program*)prog_node;
}

/*
 * Parses the whole program: a sequence of top-level items.
 */
ASTNode* parse_program(Parser* p) {
    Program* prog = (Program*)make_node(p, NODE_PROGRAM, sizeof(Program));
    if (!prog) {
        return NULL;
    }
    prog->items = NULL;
    prog->item_count = 0;

    while (peek(p) != TOKEN_EOF) {
        if (p->panic_mode) {
            synchronize_progress(p);
            p->panic_mode = false;
            if (peek(p) == TOKEN_EOF) {
                break;
            }
        }
        ASTNode* item = parse_top_level_item(p);
        if (item) {
            append_child(p, (void***)&prog->items, &prog->item_count, item);
        } else {
            if (peek(p) != TOKEN_EOF) {
                synchronize_progress(p);
                p->panic_mode = false;
            }
        }
    }
    return (ASTNode*)prog;
}

/*
 * Dispatches top-level declarations.
 */
ASTNode* parse_top_level_item(Parser* p) {
    switch (peek(p)) {
        case TOKEN_MODULE:   return parse_module_decl(p);
        case TOKEN_USE:      return parse_use_decl(p);
        case TOKEN_FUNC:     return parse_func_decl(p);
        case TOKEN_LET:      return parse_let_decl(p);
        case TOKEN_COMPTIME: return parse_comptime_decl(p);
        case TOKEN_EXTERN:   return parse_extern_decl(p);
        default:
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Unexpected token at top level");
            p->panic_mode = true;
            return NULL;
    }
}
