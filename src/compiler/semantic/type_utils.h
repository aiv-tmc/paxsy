#ifndef TYPE_UTILS_H
#define TYPE_UTILS_H

#include "type.h"
#include "semantic.h"

/*
 * Check that expression is compatible with expected type.
 * Reports an error with the given context.
 * Returns true if compatible.
 */
bool type_utils_check_expression_type(SemanticContext* ctx, ASTNode* expr,
                                      Type* expected, const char* context);

/*
 * Check that expression has an integer type.
 * Reports an error with the given context.
 * Returns true if integer.
 */
bool type_utils_check_expression_is_integer(SemanticContext* ctx, ASTNode* expr,
                                            const char* context);

/*
 * Check that expression has a pointer type.
 * Reports an error with the given context.
 * Returns true if pointer.
 */
bool type_utils_check_expression_is_pointer(SemanticContext* ctx, ASTNode* expr,
                                            const char* context);

/*
 * Resolve a type from an AST node and verify that it is valid.
 * Reports an error with the given context.
 * Returns the resolved type or NULL on error.
 */
Type* type_utils_resolve_and_check(SemanticContext* ctx, ASTNode* type_node,
                                   const char* context);

#endif
