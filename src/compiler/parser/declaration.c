#include "internal.h"
#include <stdlib.h>
#include <string.h>

/*
* Parses a module declaration.
* Consumes the 'module' keyword, then optionally reads an identifier for the module name.
* Expects an opening brace '{'.
* Parses top-level items inside the module body until a closing brace '}' or EOF is reached.
* Returns a ModuleDecl AST node.
*/
ASTNode* parse_module_decl(Parser* p) {
   advance(p);
   char* name = NULL;
   if (match(p, TOKEN_IDENTIFIER)) {
       name = strdup_custom(p->tokens[p->current - 1].value);
   }
   if (!match(p, TOKEN_LBRACE)) {
       errhandler__report_error(p->line, p->column, "syntax",
                                "Expected '{' after module name or after 'module'");
       p->panic_mode = true;
       free(name);
       return NULL;
   }
   ModuleDecl* mod = (ModuleDecl*)make_node(p, NODE_MODULE, sizeof(ModuleDecl));
   if (!mod) { free(name); return NULL; }
   mod->name = name;
   mod->body = NULL;
   mod->body_count = 0;
   while (peek(p) != TOKEN_RBRACE && peek(p) != TOKEN_EOF) {
       if (p->panic_mode) {
           synchronize_progress(p);
           p->panic_mode = false;
           if (peek(p) == TOKEN_EOF) break;
       }
       ASTNode* item = parse_top_level_item(p);
       if (item) {
           append_child(p, (void***)&mod->body, &mod->body_count, item);
       } else {
           if (peek(p) != TOKEN_RBRACE && peek(p) != TOKEN_EOF) {
               synchronize_progress(p);
               p->panic_mode = false;
           }
       }
   }
   if (!match(p, TOKEN_RBRACE)) {
       errhandler__report_error(p->line, p->column, "syntax",
                                "Expected '}' to close module");
       p->panic_mode = true;
   }
   return (ASTNode*)mod;
}

/*
* Parses a use declaration.
* Consumes the 'use' keyword.
* Reads the first identifier as the start of the module path.
* While '::' is found, appends subsequent identifiers to the path.
* The two spec forms are 'use module;' and 'use module::symbol;'.
* Expects a semicolon at the end.
* Returns a UseDecl AST node.
*/
ASTNode* parse_use_decl(Parser* p) {
   advance(p);
   UseDecl* use = (UseDecl*)make_node(p, NODE_USE, sizeof(UseDecl));
   if (!use) return NULL;
   use->path = NULL; use->path_len = 0;
   if (!match(p, TOKEN_IDENTIFIER)) {
       errhandler__report_error(p->line, p->column, "parser",
                                "Expected identifier in use path");
       p->panic_mode = true;
       return (ASTNode*)use;
   }
   char* part = strdup_custom(p->tokens[p->current - 1].value);
   append_child(p, (void***)&use->path, &use->path_len, part);
   while (1) {
       if (!match(p, TOKEN_COLON_COLON)) break;
       if (!match(p, TOKEN_IDENTIFIER)) {
           errhandler__report_error(p->line, p->column, "parser",
                                    "Expected identifier in use path");
           p->panic_mode = true;
           return (ASTNode*)use;
       }
       char* part2 = strdup_custom(p->tokens[p->current - 1].value);
       append_child(p, (void***)&use->path, &use->path_len, part2);
   }
   if (!match(p, TOKEN_SEMICOLON)) {
       errhandler__report_error(p->line, p->column, "parser",
                                "Expected ';' after use declaration");
       p->panic_mode = true;
   }
   return (ASTNode*)use;
}

/*
* Parses a function declaration.
* Optionally consumes a 'comptime' keyword if present before 'func'.
* Consumes the 'func' keyword.
* Parses the function signature: name, parameters, return type.
* Then parses the function body, which can be:
*   - a block enclosed in braces,
*   - a single expression preceded by '=' and terminated with ';',
*   - or a prototype terminated with ';'.
* Returns a FuncDecl AST node.
*/
ASTNode* parse_func_decl(Parser* p) {
   /* The FuncDecl node is created after the whole body is parsed, so
      remember the declaration start for accurate diagnostics. */
   uint32_t start_line = p->tokens[p->current].line;
   uint32_t start_col = p->tokens[p->current].column;
   uint8_t is_comptime = 0;
   if (peek(p) == TOKEN_COMPTIME) {
       advance(p);
       is_comptime = 1;
   }
   if (!match(p, TOKEN_FUNC)) {
       errhandler__report_error(p->line, p->column, "syntax", "Expected 'func'");
       return NULL;
   }
   char* name = NULL;
   ASTNode** params = NULL;
   size_t param_count = 0;
   ASTNode* ret_type = NULL;
   uint32_t mods = 0;
   char** method_of = NULL;
   size_t method_of_count = 0;
   if (!parse_function_signature(p, &name, &params, &param_count, &ret_type, &mods, true,
                                 &method_of, &method_of_count)) {
       free(name);
       return NULL;
   }
   ASTNode* body = NULL;
   ASTNode* expr_body = NULL;
   if (peek(p) == TOKEN_LBRACE) {
       body = parse_block(p);
       if (!body) {
           errhandler__report_error(p->line, p->column, "syntax",
                                    "Expected function body block");
           return NULL;
       }
   } else if (match(p, TOKEN_EQUAL)) {
       expr_body = parse_assignment_expr(p);
       if (!expr_body) {
           errhandler__report_error(p->line, p->column, "syntax",
                                    "Expected expression after '='");
           return NULL;
       }
       if (!match(p, TOKEN_SEMICOLON)) {
           errhandler__report_error(p->line, p->column, "syntax",
                                    "Expected ';' after expression body");
           return NULL;
       }
   } else if (match(p, TOKEN_SEMICOLON)) {
   } else {
       errhandler__report_error(p->line, p->column, "syntax",
                                "Expected function body, '=' expression, or ';' prototype");
       return NULL;
   }
   FuncDecl* func = (FuncDecl*)make_node(p, NODE_FUNC_DECL, sizeof(FuncDecl));
   if (!func) {
       free(name);
       return NULL;
   }
   func->base.line = start_line;
   func->base.column = start_col;
   func->name = name;
   func->modifiers = mods;
   func->is_comptime = is_comptime;
   func->method_of = method_of;
   func->method_of_count = method_of_count;
   func->params = params;
   func->param_count = param_count;
   func->return_type = ret_type;
   func->body = body;
   func->expr_body = expr_body;
   return (ASTNode*)func;
}

/*
* Parses a let declaration.
* Consumes the 'let' keyword.
* Handles single variable declarations and tuple declarations in parentheses.
* For each variable, parses an optional array size in brackets, an optional type annotation after ':',
* and an optional initializer after '='.
* Expects a semicolon at the end.
* Returns a LetDecl AST node.
*/
ASTNode* parse_let_decl(Parser* p) {
   advance(p);
   LetDecl* let = (LetDecl*)make_node(p, NODE_LET_DECL, sizeof(LetDecl));
   if (!let) return NULL;
   let->names = NULL;
   let->types = NULL;
   let->count = 0;
   let->initializer = NULL;
   let->modifiers = 0;
   let->array_size = NULL;
   let->inits = NULL;
   let->init_count = 0;
   if (match(p, TOKEN_LPAREN)) {
       while (peek(p) != TOKEN_RPAREN && peek(p) != TOKEN_EOF) {
           char* name = NULL;
           ASTNode* type = NULL;
           uint32_t mods = 0;
           if (!parse_name_type(p, &name, &type, &mods)) {
               errhandler__report_error(p->line, p->column, "syntax",
                                        "Invalid variable declaration in tuple");
               break;
           }
           size_t name_count = let->count;
           size_t type_count = let->count;
           append_child(p, (void***)&let->names, &name_count, name);
           append_child(p, (void***)&let->types, &type_count, type);
           let->count = name_count < type_count ? name_count : type_count;
           ASTNode* init = NULL;
           if (match(p, TOKEN_EQUAL)) {
               init = parse_assignment_expr(p);
               if (!init) {
                   errhandler__report_error(p->line, p->column, "syntax",
                                            "Expected expression after '=' in tuple let");
                   p->panic_mode = true;
               }
           }
           append_child(p, (void***)&let->inits, &let->init_count, init);
           if (!match(p, TOKEN_COMMA)) break;
       }
       if (!match(p, TOKEN_RPAREN)) {
           errhandler__report_error(p->line, p->column, "syntax",
                                    "Expected ')' to close tuple pattern");
           p->panic_mode = true;
       }
   } else {
       if (!match(p, TOKEN_IDENTIFIER)) {
           errhandler__report_error(p->line, p->column, "syntax",
                                    "Expected variable name");
           p->panic_mode = true;
           return (ASTNode*)let;
       }
       char* name = strdup_custom(p->tokens[p->current - 1].value);
       size_t name_count = 0;
       append_child(p, (void***)&let->names, &name_count, name);
       let->count = name_count;
       if (match(p, TOKEN_LBRACKET)) {
           if (peek(p) != TOKEN_RBRACKET) {
               let->array_size = parse_assignment_expr(p);
               if (!let->array_size) {
                   errhandler__report_error(p->line, p->column, "syntax",
                                            "Expected array size expression");
                   p->panic_mode = true;
               }
           }
           if (!match(p, TOKEN_RBRACKET)) {
               errhandler__report_error(p->line, p->column, "syntax",
                                        "Expected ']' after array size");
               p->panic_mode = true;
           }
       }
       ASTNode* type = NULL;
       if (match(p, TOKEN_COLON)) {
           type = parse_type_spec(p);
           if (!type) {
               errhandler__report_error(p->line, p->column, "syntax",
                                        "Expected type after ':'");
           }
       }
       size_t type_count = 0;
       append_child(p, (void***)&let->types, &type_count, type);
   }
   if (match(p, TOKEN_EQUAL)) {
       let->initializer = parse_assignment_expr(p);
       if (!let->initializer) {
           errhandler__report_error(p->line, p->column, "syntax",
                                    "Expected expression after '='");
           p->panic_mode = true;
       }
   }
   if (!match(p, TOKEN_SEMICOLON)) {
       errhandler__report_error(p->line, p->column, "syntax",
                                "Expected ';' after let declaration");
       p->panic_mode = true;
   }
   return (ASTNode*)let;
}

/*
* Parses a comptime declaration.
* Consumes the 'comptime' keyword.
* If the next token is 'func', parses a compile-time function:
*   reads the signature, then expects a body block, a '=' expression, or a ';' prototype.
* Otherwise, reads an identifier as the declaration name.
* Expects '=' after the name.
* If the right-hand side is 'Struct', 'Union', or 'Enum', parses it as a type definition.
* Otherwise, parses the right-hand side as a value expression.
* Expects a semicolon at the end.
* Returns a FuncDecl or ComptimeDecl AST node.
*/
ASTNode* parse_comptime_decl(Parser* p) {
   advance(p);
   if (peek(p) == TOKEN_FUNC) {
       advance(p);
       char* name = NULL;
       ASTNode** params = NULL;
       size_t param_count = 0;
       ASTNode* ret_type = NULL;
       uint32_t mods = 0;
       char** method_of = NULL;
       size_t method_of_count = 0;
       if (!parse_function_signature(p, &name, &params, &param_count, &ret_type, &mods, false,
                                     &method_of, &method_of_count)) {
           free(name);
           return NULL;
       }
       if (method_of_count > 0) {
           errhandler__report_error(p->line, p->column, "syntax",
                                    "comptime functions cannot be methods");
           for (size_t i = 0; i < method_of_count; ++i) free(method_of[i]);
           free(name);
           return NULL;
       }
       if (mods & (MOD_EXTERN | MOD_VOLATILE)) {
           errhandler__report_error(p->line, p->column, "syntax",
                                    "comptime functions cannot have 'extern' or 'volatile'");
       }
       ASTNode* body = NULL;
       ASTNode* expr_body = NULL;
       if (peek(p) == TOKEN_LBRACE) {
           body = parse_block(p);
           if (!body) {
               errhandler__report_error(p->line, p->column, "syntax",
                                        "Expected function body");
               return NULL;
           }
       } else if (match(p, TOKEN_EQUAL)) {
           expr_body = parse_assignment_expr(p);
           if (!expr_body) {
               errhandler__report_error(p->line, p->column, "syntax",
                                        "Expected expression after '='");
               return NULL;
           }
           if (!match(p, TOKEN_SEMICOLON)) {
               errhandler__report_error(p->line, p->column, "syntax",
                                        "Expected ';' after expression body");
               return NULL;
           }
       } else if (match(p, TOKEN_SEMICOLON)) {
       } else {
           errhandler__report_error(p->line, p->column, "syntax",
                                    "Expected function body, '=' expression, or ';' prototype");
           return NULL;
       }
       FuncDecl* func = (FuncDecl*)make_node(p, NODE_FUNC_DECL, sizeof(FuncDecl));
       if (!func) {
           free(name);
           return NULL;
       }
       func->name = name;
       func->modifiers = mods;
       func->is_comptime = 1;
       func->method_of = NULL;
       func->method_of_count = 0;
       func->params = params;
       func->param_count = param_count;
       func->return_type = ret_type;
       func->body = body;
       func->expr_body = expr_body;
       return (ASTNode*)func;
   }
   if (!match(p, TOKEN_IDENTIFIER)) {
       errhandler__report_error(p->line, p->column, "syntax",
                                "Expected comptime name");
       p->panic_mode = true;
       return NULL;
   }
   char* name = strdup_custom(p->tokens[p->current - 1].value);
   ComptimeDecl* decl = (ComptimeDecl*)make_node(p, NODE_COMPTIME_DECL, sizeof(ComptimeDecl));
   if (!decl) { free(name); return NULL; }
   decl->name = name;
   decl->type_def = NULL;
   decl->value = NULL;
   if (!match(p, TOKEN_EQUAL)) {
       errhandler__report_error(p->line, p->column, "syntax",
                                "Expected '=' in comptime declaration");
       p->panic_mode = true;
       return (ASTNode*)decl;
   }
   if (token_is_type_start(peek(p))) {
       decl->type_def = parse_type_spec(p);
   } else {
       decl->value = parse_assignment_expr(p);
       if (!decl->value) {
           errhandler__report_error(p->line, p->column, "syntax",
                                    "Expected expression after '='");
       }
   }
   if (!match(p, TOKEN_SEMICOLON)) {
       errhandler__report_error(p->line, p->column, "syntax",
                                "Expected ';' after comptime declaration");
       p->panic_mode = true;
   }
   return (ASTNode*)decl;
}

