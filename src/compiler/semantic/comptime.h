#ifndef COMPTIME_H
#define COMPTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct SemanticContext;
struct ASTNode;
struct Symbol;

/*
 * Evaluate a constant expression and store the result in out_val.
 * Returns true on success.
 */
bool eval_const_expr(struct SemanticContext* ctx, struct ASTNode* node, int64_t* out_val);

/*
 * Evaluate a comptime function call.
 * Returns true on success and stores the result.
 */
bool comptime_evaluate_call(struct SemanticContext* ctx, struct Symbol* func,
                            struct ASTNode** args, size_t arg_count, int64_t* result);

#endif
