#ifndef DECLARATION_H
#define DECLARATION_H

#include "../parser/parser.h"
#include "symbol.h"
#include "type.h"

struct SemanticContext;

bool analyze_module_declaration(struct SemanticContext* ctx, ModuleDecl* mod);
bool analyze_use_declaration(struct SemanticContext* ctx, UseDecl* use);
bool analyze_func_declaration(struct SemanticContext* ctx, FuncDecl* func);
bool analyze_let_declaration(struct SemanticContext* ctx, LetDecl* let);
bool analyze_comptime_declaration(struct SemanticContext* ctx, ComptimeDecl* comp);
bool analyze_extern_declaration(struct SemanticContext* ctx, ExternDecl* ext);
bool analyze_extern_block(struct SemanticContext* ctx, ExternBlock* blk);

/*
 * Analyze a function body in a fresh function scope: seed the
 * parameters, analyze the block or shorthand expression, and check
 * that a non-Void function returns on all paths. Shared by ordinary
 * functions, methods, and template specializations.
 */
bool analyze_function_body(struct SemanticContext* ctx, FuncDecl* func,
                           ParamInfo* param_list, Type* ret_type);

#endif
