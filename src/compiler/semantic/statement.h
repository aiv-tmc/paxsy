#ifndef STATEMENT_H
#define STATEMENT_H

#include "../parser/parser.h"
#include "symbol.h"

struct SemanticContext;

bool analyze_block(struct SemanticContext* ctx, Block* block);
bool analyze_if(struct SemanticContext* ctx, IfStmt* ifstmt);
bool analyze_if_exists(struct SemanticContext* ctx, IfExistsStmt* stmt);
bool analyze_match(struct SemanticContext* ctx, MatchStmt* match);
bool analyze_while(struct SemanticContext* ctx, WhileStmt* while_stmt);
bool analyze_do_while(struct SemanticContext* ctx, DoWhileStmt* do_while);
bool analyze_for(struct SemanticContext* ctx, ForStmt* for_stmt);
bool analyze_break(struct SemanticContext* ctx, BreakStmt* brk);
bool analyze_continue(struct SemanticContext* ctx, ContinueStmt* cont);
bool analyze_return(struct SemanticContext* ctx, ReturnStmt* ret);
bool analyze_defer(struct SemanticContext* ctx, DeferStmt* defer);
bool analyze_asm(struct SemanticContext* ctx, AsmStmt* asm_stmt);
bool analyze_volatile(struct SemanticContext* ctx, VolatileStmt* volatile_stmt);
bool analyze_comptime_statement(struct SemanticContext* ctx, ComptimeStmt* comp);
bool analyze_pass(struct SemanticContext* ctx, PassStmt* pass);
bool analyze_expr_statement(struct SemanticContext* ctx, ExprStmt* expr_stmt);
bool analyze_free_stmt(struct SemanticContext* ctx, FreeStmt* free_stmt);

bool analyze_statement(struct SemanticContext* ctx, ASTNode* node);

#endif
