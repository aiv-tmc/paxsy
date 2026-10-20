#include "internal.h"
#include <stdlib.h>
#include <string.h>

/*
 * Parses extern declarations in spec syntax:
 *   extern func sqrt(x: Real64): Real64;
 *   extern let buffer_size: Int32;
 *   extern {
 *       func init_system(): Void;
 *       let buffer_size: Int32;
 *   }
 * An extern declaration introduces a symbol with external linkage;
 * no definition is provided in this translation unit.
 */

/*
 * Parse a single extern function prototype after the 'func' keyword.
 * Returns an ExternDecl AST node or NULL after reporting an error.
 */
static ASTNode* parse_extern_func(Parser* p) {
    advance(p);
    if (!match(p, TOKEN_IDENTIFIER)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected function name");
        p->panic_mode = true;
        return NULL;
    }
    char* name = strdup_custom(p->tokens[p->current - 1].value);
    ExternDecl* ext = (ExternDecl*)make_node(p, NODE_EXTERN_DECL, sizeof(ExternDecl));
    if (!ext) { free(name); return NULL; }
    ext->name = name;
    ext->is_func = 1;
    ext->type = NULL;
    ext->params = NULL;
    ext->param_count = 0;
    if (!match(p, TOKEN_LPAREN)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '(' for extern func parameters");
        p->panic_mode = true;
        return (ASTNode*)ext;
    }
    if (peek(p) != TOKEN_RPAREN) {
        do {
            char* pname = NULL;
            ASTNode* ptype = NULL;
            uint32_t mods = 0;
            if (!parse_name_type(p, &pname, &ptype, &mods)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Invalid parameter in extern");
                break;
            }
            Param* param = (Param*)make_node(p, NODE_PARAM, sizeof(Param));
            if (!param) {
                free(pname);
                free_ast_node(ptype);
                break;
            }
            param->name = pname;
            param->type = ptype;
            param->modifiers = mods;
            param->is_volatile = (mods & MOD_VOLATILE) ? 1 : 0;
            param->is_comptime = 0;
            append_child(p, (void***)&ext->params, &ext->param_count, param);
        } while (match(p, TOKEN_COMMA) && peek(p) != TOKEN_RPAREN);
    }
    if (!match(p, TOKEN_RPAREN)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ')' after parameters");
        p->panic_mode = true;
    }
    if (!match(p, TOKEN_COLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ':' for extern func return type");
        p->panic_mode = true;
    }
    ext->type = parse_type_spec(p);
    if (!ext->type) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected return type");
        p->panic_mode = true;
    }
    if (!match(p, TOKEN_SEMICOLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ';' after extern func");
        p->panic_mode = true;
    }
    return (ASTNode*)ext;
}

/*
 * Parse a single extern variable declaration after the 'let' keyword.
 * Returns an ExternDecl AST node or NULL after reporting an error.
 */
static ASTNode* parse_extern_let(Parser* p) {
    advance(p);
    if (!match(p, TOKEN_IDENTIFIER)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected variable name");
        p->panic_mode = true;
        return NULL;
    }
    char* name = strdup_custom(p->tokens[p->current - 1].value);
    ExternDecl* ext = (ExternDecl*)make_node(p, NODE_EXTERN_DECL, sizeof(ExternDecl));
    if (!ext) { free(name); return NULL; }
    ext->name = name;
    ext->is_func = 0;
    ext->type = NULL;
    ext->params = NULL;
    ext->param_count = 0;
    if (!match(p, TOKEN_COLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ':' for extern let type");
        p->panic_mode = true;
        return (ASTNode*)ext;
    }
    ext->type = parse_type_spec(p);
    if (!ext->type) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected type");
        p->panic_mode = true;
    }
    if (!match(p, TOKEN_SEMICOLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ';' after extern let");
        p->panic_mode = true;
    }
    return (ASTNode*)ext;
}

/*
 * Parses an extern declaration: a single extern prototype or an
 * extern block collecting several prototypes in braces.
 */
ASTNode* parse_extern_decl(Parser* p) {
    advance(p);
    if (peek(p) == TOKEN_LBRACE) {
        advance(p);
        ExternBlock* blk = (ExternBlock*)make_node(p, NODE_EXTERN_BLOCK, sizeof(ExternBlock));
        if (!blk) return NULL;
        blk->items = NULL;
        blk->item_count = 0;
        while (peek(p) != TOKEN_RBRACE && peek(p) != TOKEN_EOF) {
            ASTNode* item = NULL;
            if (peek(p) == TOKEN_FUNC) {
                item = parse_extern_func(p);
            } else if (peek(p) == TOKEN_LET) {
                item = parse_extern_let(p);
            } else {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected 'func' or 'let' in extern block");
                p->panic_mode = true;
                synchronize_progress(p);
                continue;
            }
            if (item) {
                append_child(p, (void***)&blk->items, &blk->item_count, item);
            }
        }
        if (!match(p, TOKEN_RBRACE)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected '}' to close extern block");
            p->panic_mode = true;
        }
        return (ASTNode*)blk;
    }
    if (peek(p) == TOKEN_FUNC) {
        return parse_extern_func(p);
    }
    if (peek(p) == TOKEN_LET) {
        return parse_extern_let(p);
    }
    errhandler__report_error(p->line, p->column, "syntax",
                             "Expected 'func', 'let', or '{' after 'extern'");
    p->panic_mode = true;
    return NULL;
}
