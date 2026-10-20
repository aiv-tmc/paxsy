#include "type_utils.h"
#include "expression.h"
#include "../../interfaces/errhandler/errhandler.h"

bool type_utils_check_expression_type(SemanticContext* ctx, ASTNode* expr,
                                      Type* expected, const char* context) {
    Type* actual = analyze_expression(ctx, expr);
    if (!actual) return false;
    if (!type_compatible(actual, expected)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, expr->line, expr->column, 1,
                                    "semantic", "%s: expected %s, got %s",
                                    context, type_to_string(expected), type_to_string(actual));
        type_free(actual);
        return false;
    }
    type_free(actual);
    return true;
}

bool type_utils_check_expression_is_integer(SemanticContext* ctx, ASTNode* expr,
                                            const char* context) {
    Type* t = analyze_expression(ctx, expr);
    if (!t) return false;
    bool ok = type_is_integer(t);
    if (!ok) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, expr->line, expr->column, 1,
                                    "semantic", "%s: expected integer, got %s",
                                    context, type_to_string(t));
    }
    type_free(t);
    return ok;
}

bool type_utils_check_expression_is_pointer(SemanticContext* ctx, ASTNode* expr,
                                            const char* context) {
    Type* t = analyze_expression(ctx, expr);
    if (!t) return false;
    bool ok = type_is_pointer(t);
    if (!ok) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, expr->line, expr->column, 1,
                                    "semantic", "%s: expected pointer, got %s",
                                    context, type_to_string(t));
    }
    type_free(t);
    return ok;
}

Type* type_utils_resolve_and_check(SemanticContext* ctx, ASTNode* type_node,
                                   const char* context) {
    Type* t = resolve_type_from_ast(ctx, type_node);
    if (!t) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, type_node->line, type_node->column, 1,
                                    "semantic", "%s: invalid type", context);
    }
    return t;
}
