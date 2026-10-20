#include "expression.h"
#include "semantic.h"
#include "statement.h"
#include "builtins.h"
#include "comptime.h"
#include "comptime_params.h"
#include "type_utils.h"
#include "../../interfaces/errhandler/errhandler.h"
#include "../../interfaces/utils/common.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

/*
 * Resolve an identifier and mark it as used.
 */
static Symbol* resolve_ident(SemanticContext* ctx, const char* name, uint32_t line, uint32_t col) {
    Symbol* sym = scope_lookup(ctx->current_scope, name);
    if (!sym) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, line, col, 1,
                                    "semantic", "Undeclared identifier '%s'", name);
        return NULL;
    }
    sym->is_used = true;
    if (!sym->is_initialized && sym->kind == SYMBOL_VARIABLE) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, line, col, 1,
                                    "semantic", "Variable '%s' used before initialization", name);
        return NULL;
    }
    return sym;
}

/*
 * Dispatch analysis of an expression node.
 */
Type* analyze_expression(SemanticContext* ctx, ASTNode* node) {
    if (!node) return NULL;
    switch (node->type) {
        case NODE_IDENT:               return analyze_ident(ctx, (IdentExpr*)node);
        case NODE_LITERAL:             return analyze_literal(ctx, (LiteralExpr*)node);
        case NODE_UNARY:               return analyze_unary(ctx, (UnaryExpr*)node);
        case NODE_BINARY:              return analyze_binary(ctx, (BinaryExpr*)node);
        case NODE_ASSIGN:              return analyze_assign(ctx, (AssignExpr*)node);
        case NODE_SEND:                return analyze_send(ctx, (SendExpr*)node);
        case NODE_CALL:                return analyze_call(ctx, (CallExpr*)node);
        case NODE_MEMBER_ACCESS:       return analyze_member_access(ctx, (MemberAccess*)node);
        case NODE_MODULE_ACCESS:       return analyze_module_access(ctx, (ModuleAccess*)node);
        case NODE_INDEX:               return analyze_index(ctx, (IndexExpr*)node);
        case NODE_SLICE:               return analyze_slice(ctx, (SliceExpr*)node);
        case NODE_STRUCT_INIT:         return analyze_struct_init(ctx, (StructInitExpr*)node);
        case NODE_RANGE:               return analyze_range(ctx, (RangeExpr*)node);
        case NODE_IF_EXPR:             return analyze_if_expression(ctx, (IfExpr*)node);
        case NODE_TRY_EXPR:            return analyze_try_expression(ctx, (TryExpr*)node);
        case NODE_CATCH_EXPR:          return analyze_catch_expression(ctx, (CatchExpr*)node);
        case NODE_ERROR_EXPR:          return analyze_error_expression(ctx, (ErrorExpr*)node);
        case NODE_INTERPOLATED_STRING: return analyze_interpolated_string(ctx, (InterpolatedString*)node);
        case NODE_CAST:                return analyze_cast(ctx, (CastExpr*)node);
        case NODE_ARRAY_LITERAL:       return analyze_array_literal(ctx, (ArrayLiteral*)node);
        case NODE_MATCH_EXPR:          return analyze_match_expr(ctx, (MatchExpr*)node);
        case NODE_ALLOC_EXPR:          return analyze_alloc_expr(ctx, (AllocExpr*)node);
        default:
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, node->line, node->column, 1,
                                        "semantic", "Invalid expression node");
            return NULL;
    }
}

/*
 * Analyze an identifier expression.
 */
Type* analyze_ident(SemanticContext* ctx, IdentExpr* id) {
    Symbol* sym = resolve_ident(ctx, id->name, id->base.line, id->base.column);
    if (!sym) return NULL;
    if (sym->type) return type_copy(sym->type);
    if (sym->kind == SYMBOL_VARIABLE && !sym->type) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, id->base.line, id->base.column, 1,
                                    "semantic", "Variable '%s' has no type yet", id->name);
        return NULL;
    }
    if (sym->kind == SYMBOL_CONSTANT) {
        return type_basic(TYPE_BASIC_INT64);
    }
    return NULL;
}

/*
 * Select the smallest signed integer type for a value.
 */
static Type* select_signed_integer_type(int64_t value) {
    if (value >= -128 && value <= 127) return type_basic(TYPE_BASIC_INT8);
    if (value >= -32768 && value <= 32767) return type_basic(TYPE_BASIC_INT16);
    if (value >= -2147483648LL && value <= 2147483647LL) return type_basic(TYPE_BASIC_INT32);
    if (value >= -9223372036854775807LL - 1 && value <= 9223372036854775807LL) return type_basic(TYPE_BASIC_INT64);
    return type_basic(TYPE_BASIC_INT64);
}

/*
 * Select the smallest unsigned integer type for a value.
 */
static Type* select_unsigned_integer_type(uint64_t value) {
    if (value <= 255) return type_basic(TYPE_BASIC_UINT8);
    if (value <= 65535) return type_basic(TYPE_BASIC_UINT16);
    if (value <= 4294967295ULL) return type_basic(TYPE_BASIC_UINT32);
    if (value <= 18446744073709551615ULL) return type_basic(TYPE_BASIC_UINT64);
    return type_basic(TYPE_BASIC_UINT64);
}

/*
 * Analyze a literal expression.
 */
Type* analyze_literal(SemanticContext* ctx, LiteralExpr* lit) {
    (void)ctx;
    switch (lit->kind) {
        case LIT_NUMBER: {
            const char* str = lit->raw_value;
            size_t len = strlen(str);
            bool is_unsigned = false;
            bool is_float = false;
            /*
             * A trailing U/f is a suffix only where the lexer accepts one:
             * U on decimal integers, f on decimal floats and hex floats.
             * A hex integer may legitimately end in F (an uppercase digit).
             */
            bool is_hex = (strncmp(str, "0x", 2) == 0 || strncmp(str, "0X", 2) == 0);
            bool is_hex_float = is_hex &&
                (strchr(str, '.') != NULL || strchr(str, 'p') != NULL);
            if (len > 0) {
                char last = str[len-1];
                if (last == 'U' || last == 'u') {
                    is_unsigned = true;
                } else if ((last == 'f' || last == 'F') && (!is_hex || is_hex_float)) {
                    is_float = true;
                }
            }
            if (is_float) {
                char* end;
                double val = strtod(str, &end);
                if (val == 0.0 && end == str) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, lit->base.line, lit->base.column, 1,
                                                "semantic", "Invalid floating literal");
                    return NULL;
                }
                if (is_float) {
                    if (val >= -3.4e38 && val <= 3.4e38) {
                        return type_basic(TYPE_BASIC_REAL32);
                    } else {
                        return type_basic(TYPE_BASIC_REAL64);
                    }
                }
            } else {
                char* end;
                int base = 10;
                if (strncmp(str, "0x", 2) == 0 || strncmp(str, "0X", 2) == 0) {
                    base = 16;
                } else if (strncmp(str, "0b", 2) == 0 || strncmp(str, "0B", 2) == 0) {
                    base = 2;
                } else if (strncmp(str, "0o", 2) == 0 || strncmp(str, "0O", 2) == 0) {
                    base = 8;
                }
                errno = 0;
                long long signed_val = strtoll(str, &end, base);
                if (errno == ERANGE) {
                    errno = 0;
                    unsigned long long unsigned_val = strtoull(str, &end, base);
                    if (errno == ERANGE) {
                        return type_basic(TYPE_BASIC_INT64);
                    }
                    if (is_unsigned) {
                        return select_unsigned_integer_type(unsigned_val);
                    } else {
                        if (str[0] == '-') {
                            errhandler__report_error_ex(ERROR_LEVEL_ERROR, lit->base.line, lit->base.column, 1,
                                                        "semantic", "Negative literal too large for any signed integer type");
                            return NULL;
                        }
                        return select_unsigned_integer_type(unsigned_val);
                    }
                }
                if (str[0] == '-') {
                    if (is_unsigned) {
                        errhandler__report_error_ex(ERROR_LEVEL_ERROR, lit->base.line, lit->base.column, 1,
                                                    "semantic", "Unsigned suffix on negative literal");
                        return NULL;
                    }
                    return select_signed_integer_type(signed_val);
                } else {
                    if (is_unsigned) {
                        return select_unsigned_integer_type((uint64_t)signed_val);
                    } else {
                        return select_signed_integer_type(signed_val);
                    }
                }
            }
            break;
        }
        case LIT_STRING: {
            size_t len = strlen(lit->string_val);
            int64_t size = (int64_t)(len + 1);
            return type_array(type_basic(TYPE_BASIC_UTF8), size);
        }
        case LIT_CHAR:    return type_basic(TYPE_BASIC_UTF8);
        case LIT_BOOL:    return type_basic(TYPE_BASIC_BOOL);
        case LIT_NONE:    return type_nullable(type_basic(TYPE_BASIC_VOID));
        default:          return type_basic(TYPE_BASIC_VOID);
    }
    return type_basic(TYPE_BASIC_VOID);
}

/*
 * Analyze a unary expression.
 */
Type* analyze_unary(SemanticContext* ctx, UnaryExpr* unary) {
    Type* operand = analyze_expression(ctx, unary->operand);
    if (!operand) return NULL;
    Type* result = NULL;
    switch (unary->op) {
        case UNARY_PLUS:
        case UNARY_MINUS:
            if (type_is_numeric(operand)) {
                result = type_copy(operand);
            } else {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, unary->base.line, unary->base.column, 1,
                                            "semantic", "Unary +/- requires numeric operand");
            }
            break;
        case UNARY_LOGICAL_NOT:
            result = type_basic(TYPE_BASIC_BOOL);
            break;
        case UNARY_BIT_NOT:
            if (type_is_integer(operand)) {
                result = type_copy(operand);
            } else {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, unary->base.line, unary->base.column, 1,
                                            "semantic", "Bitwise not requires integer operand");
            }
            break;
        case UNARY_ADDR:
            result = type_pointer(type_copy(operand));
            break;
        case UNARY_DEREF:
            if (operand->kind == TYPE_KIND_POINTER) {
                result = type_copy(operand->u.pointer.base);
            } else {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, unary->base.line, unary->base.column, 1,
                                            "semantic", "Dereference requires pointer operand");
            }
            break;
        default:
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, unary->base.line, unary->base.column, 1,
                                        "semantic", "Unsupported unary operator");
            break;
    }
    type_free(operand);
    return result;
}

/*
 * Returns true when the expression is an integer constant with the
 * value zero — the only operand a pointer may be compared with
 * (the spec compares a pointer with the integer literal 0).
 */
static bool is_zero_literal(SemanticContext* ctx, ASTNode* expr) {
    int64_t val = 0;
    return eval_const_expr(ctx, expr, &val) && val == 0;
}

/*
 * Analyze a binary expression.
 */
Type* analyze_binary(SemanticContext* ctx, BinaryExpr* binary) {
    Type* left = analyze_expression(ctx, binary->left);
    Type* right = analyze_expression(ctx, binary->right);
    if (!left || !right) { type_free(left); type_free(right); return NULL; }
    Type* result = NULL;
    switch (binary->op) {
        case BIN_ADD:
        case BIN_SUB:
        case BIN_MUL:
        case BIN_DIV:
            if (type_is_numeric(left) && type_is_numeric(right)) {
                if (type_is_real(left) || type_is_real(right)) {
                    result = type_basic(TYPE_BASIC_REAL64);
                } else {
                    result = type_basic(TYPE_BASIC_INT32);
                }
            } else {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, binary->base.line, binary->base.column, 1,
                                            "semantic", "Arithmetic operands must be numeric");
            }
            break;
        case BIN_MOD:
            if (!type_is_integer(left) || !type_is_integer(right)) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, binary->base.line, binary->base.column, 1,
                                            "semantic", "Modulo operator requires integer operands");
                type_free(left);
                type_free(right);
                return NULL;
            }
            result = type_copy(left);
            break;
        case BIN_EQ:
        case BIN_NE:
        case BIN_GT:
        case BIN_LT:
        case BIN_GE:
        case BIN_LE:
            if (type_is_numeric(left) && type_is_numeric(right)) {
                result = type_basic(TYPE_BASIC_BOOL);
            } else if (left->kind == TYPE_KIND_POINTER && right->kind == TYPE_KIND_POINTER) {
                result = type_basic(TYPE_BASIC_BOOL);
            } else if (left->kind == TYPE_KIND_POINTER && is_zero_literal(ctx, binary->right)) {
                result = type_basic(TYPE_BASIC_BOOL);
            } else if (right->kind == TYPE_KIND_POINTER && is_zero_literal(ctx, binary->left)) {
                result = type_basic(TYPE_BASIC_BOOL);
            } else if (left->kind == TYPE_KIND_BASIC && right->kind == TYPE_KIND_BASIC &&
                       left->u.basic == TYPE_BASIC_BOOL && right->u.basic == TYPE_BASIC_BOOL) {
                result = type_basic(TYPE_BASIC_BOOL);
            } else {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, binary->base.line, binary->base.column, 1,
                                            "semantic", "Incomparable operands for relational operator");
            }
            break;
        case BIN_AND:
        case BIN_OR:
        case BIN_XOR:
            if (type_is_integer(left) && type_is_integer(right)) {
                result = type_basic(TYPE_BASIC_INT32);
            } else {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, binary->base.line, binary->base.column, 1,
                                            "semantic", "Bitwise operands must be integers");
            }
            break;
        case BIN_SHL:
        case BIN_SHR:
        case BIN_SHL_LOG:
        case BIN_SHR_LOG:
            if (type_is_integer(left) && type_is_integer(right)) {
                result = type_copy(left);
            } else {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, binary->base.line, binary->base.column, 1,
                                            "semantic", "Shift operands must be integers");
            }
            break;
        case BIN_LOGICAL_AND:
        case BIN_LOGICAL_OR:
            result = type_basic(TYPE_BASIC_BOOL);
            break;
        default:
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, binary->base.line, binary->base.column, 1,
                                        "semantic", "Unsupported binary operator");
            break;
    }
    type_free(left);
    type_free(right);
    return result;
}

/*
 * Analyze an assignment expression.
 */
Type* analyze_assign(SemanticContext* ctx, AssignExpr* assign) {
    Type* left_type = analyze_expression(ctx, assign->left);
    if (!left_type) return NULL;

    if (assign->op != ASSIGN_NORMAL) {
        BinaryOp bin_op;
        switch (assign->op) {
            case ASSIGN_ADD: bin_op = BIN_ADD; break;
            case ASSIGN_SUB: bin_op = BIN_SUB; break;
            case ASSIGN_MUL: bin_op = BIN_MUL; break;
            case ASSIGN_DIV: bin_op = BIN_DIV; break;
            case ASSIGN_MOD: bin_op = BIN_MOD; break;
            case ASSIGN_BIT_AND: bin_op = BIN_AND; break;
            case ASSIGN_BIT_OR:  bin_op = BIN_OR; break;
            case ASSIGN_BIT_XOR: bin_op = BIN_XOR; break;
            case ASSIGN_SHL: bin_op = BIN_SHL; break;
            case ASSIGN_SHR: bin_op = BIN_SHR; break;
            case ASSIGN_SHL_LOG: bin_op = BIN_SHL_LOG; break;
            case ASSIGN_SHR_LOG: bin_op = BIN_SHR_LOG; break;
            default:
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, assign->base.line, assign->base.column, 1,
                                            "semantic", "Unsupported compound assignment operator");
                type_free(left_type);
                return NULL;
        }
        BinaryExpr temp = {0};
        temp.left = assign->left;
        temp.right = assign->right;
        temp.op = bin_op;
        temp.base = assign->base;
        Type* op_result = analyze_binary(ctx, &temp);
        if (!op_result) {
            type_free(left_type);
            return NULL;
        }
        type_free(op_result);
    }

    Type* right_type = analyze_expression(ctx, assign->right);
    if (!right_type) { type_free(left_type); return NULL; }

    if (assign->left->type == NODE_IDENT) {
        IdentExpr* id = (IdentExpr*)assign->left;
        Symbol* sym = scope_lookup(ctx->current_scope, id->name);
        if (sym && sym->kind == SYMBOL_VARIABLE && sym->type == type_Auto && !sym->is_initialized) {
            sym->type = type_copy(right_type);
            sym->is_initialized = true;
            type_free(left_type);
            return right_type;
        }
    }

    if (!type_compatible(right_type, left_type)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, assign->right->line, assign->right->column, 1,
                                    "semantic", "Assignment type mismatch: cannot assign %s to %s",
                                    type_to_string(right_type), type_to_string(left_type));
        type_free(left_type);
        type_free(right_type);
        return NULL;
    }
    if (assign->left->type == NODE_IDENT) {
        IdentExpr* id = (IdentExpr*)assign->left;
        Symbol* sym = scope_lookup(ctx->current_scope, id->name);
        if (sym && sym->kind == SYMBOL_VARIABLE) {
            sym->is_initialized = true;
        }
    }
    type_free(right_type);
    return left_type;
}

/*
 * Analyze a send expression.
 */
Type* analyze_send(SemanticContext* ctx, SendExpr* send) {
    Type* target = analyze_expression(ctx, send->target);
    if (!target) return NULL;
    type_free(target);
    Type* msg = analyze_expression(ctx, send->message);
    if (!msg) return NULL;
    type_free(msg);
    return type_basic(TYPE_BASIC_VOID);
}

/*
 * Analyze a call expression.
 */
Type* analyze_call(SemanticContext* ctx, CallExpr* call) {
    /* A call to a comptime-parameter function is specialized with
       the call's arguments. */
    if (call->callee->type == NODE_IDENT) {
        Symbol* sym = scope_lookup(ctx->current_scope, ((IdentExpr*)call->callee)->name);
        if (sym && sym->kind == SYMBOL_FUNCTION && sym->extra.func.is_template) {
            return specialize_template_call(ctx, sym, call);
        }
    }
    Type* callee_type = analyze_expression(ctx, call->callee);
    if (!callee_type) return NULL;

    bool is_builtin = false;
    Symbol* builtin_sym = NULL;
    if (call->callee->type == NODE_MODULE_ACCESS) {
        ModuleAccess* ma = (ModuleAccess*)call->callee;
        if (ma->path_len == 2 && strcmp(ma->path[0], "std") == 0 && strcmp(ma->path[1], "type") == 0) {
            builtin_sym = builtins_get_symbol(ma->symbol);
            if (builtin_sym && builtin_sym->is_comptime_builtin) {
                is_builtin = true;
            }
        }
    } else if (call->callee->type == NODE_IDENT) {
        IdentExpr* id = (IdentExpr*)call->callee;
        Symbol* sym = scope_lookup(ctx->current_scope, id->name);
        if (sym && sym->is_comptime_builtin) {
            builtin_sym = sym;
            is_builtin = true;
        }
    }

    if (is_builtin) {
        if (call->arg_count != 1) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, call->base.line, call->base.column, 1,
                                        "semantic", "Builtin function '%s' expects exactly 1 argument",
                                        builtin_sym->name);
            type_free(callee_type);
            return NULL;
        }
        ASTNode* arg = call->args[0];
        bool is_type_node = (arg->type == NODE_BASIC_TYPE || arg->type == NODE_POINTER_TYPE ||
                             arg->type == NODE_ARRAY_TYPE || arg->type == NODE_TUPLE_TYPE ||
                             arg->type == NODE_NULLABLE_TYPE || arg->type == NODE_ERROR_TYPE ||
                             arg->type == NODE_FUNC_TYPE || arg->type == NODE_IDENT_TYPE ||
                             arg->type == NODE_STRUCT_TYPE || arg->type == NODE_UNION_TYPE ||
                             arg->type == NODE_ENUM_TYPE);
        if (!is_type_node) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, arg->line, arg->column, 1,
                                        "semantic", "Argument to builtin must be a type");
            type_free(callee_type);
            return NULL;
        }
        type_free(callee_type);
        return type_basic(TYPE_BASIC_INT64);
    }

    if (callee_type->kind != TYPE_KIND_FUNCTION) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, call->callee->line, call->callee->column, 1,
                                    "semantic", "Calling non-function");
        type_free(callee_type);
        return NULL;
    }
    if (call->arg_count != callee_type->u.function.param_count) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, call->base.line, call->base.column, 1,
                                    "semantic", "Argument count mismatch: expected %zu, got %zu",
                                    callee_type->u.function.param_count, call->arg_count);
        type_free(callee_type);
        return NULL;
    }
    for (size_t i = 0; i < call->arg_count; ++i) {
        Type* arg_type = analyze_expression(ctx, call->args[i]);
        if (!arg_type) { type_free(callee_type); return NULL; }
        Type* param_type = callee_type->u.function.param_types[i];
        if (!type_compatible(arg_type, param_type)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, call->args[i]->line, call->args[i]->column, 1,
                                        "semantic", "Argument %zu type mismatch: expected %s, got %s",
                                        i+1, type_to_string(param_type), type_to_string(arg_type));
            type_free(arg_type);
            type_free(callee_type);
            return NULL;
        }
        type_free(arg_type);
    }
    Type* ret = type_copy(callee_type->u.function.return_type);
    type_free(callee_type);
    return ret;
}

/*
 * Analyze a member access expression.
 */
Type* analyze_member_access(SemanticContext* ctx, MemberAccess* ma) {
    Type* obj = analyze_expression(ctx, ma->object);
    if (!obj) return NULL;

    if (obj->kind == TYPE_KIND_STRUCT || obj->kind == TYPE_KIND_UNION) {
        Symbol* type_sym = obj->u.named;
        if (!type_sym) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, ma->base.line, ma->base.column, 1,
                                        "semantic", "Invalid type for member access");
            type_free(obj);
            return NULL;
        }
        /* A type designator (Point or geometry::Point) yields unbound
           method types; anything else is an instance and binds the
           receiver. */
        bool object_is_type = false;
        if (ma->object->type == NODE_IDENT) {
            Symbol* osym = scope_lookup(ctx->current_scope, ((IdentExpr*)ma->object)->name);
            object_is_type = (osym && osym->kind == SYMBOL_TYPE);
        } else if (ma->object->type == NODE_MODULE_ACCESS) {
            object_is_type = true;
        }
        FieldInfo* field = type_sym->extra.fields;
        while (field) {
            if (strcmp(field->name, ma->member) == 0) {
                Type* result = type_copy(field->type);
                type_free(obj);
                return result;
            }
            field = field->next;
        }
        MethodInfo* method = type_sym->extra.methods;
        while (method) {
            if (strcmp(method->name, ma->member) == 0) {
                FuncDecl* decl = method->decl;
                Type** param_types = NULL;
                size_t param_count = decl->param_count;
                if (param_count > 0) {
                    param_types = (Type**)calloc(param_count, sizeof(Type*));
                    if (!param_types) {
                        type_free(obj);
                        return NULL;
                    }
                    for (size_t i = 0; i < param_count; ++i) {
                        Param* p = (Param*)decl->params[i];
                        Type* ptype = resolve_type_from_ast(ctx, p->type);
                        if (!ptype) {
                            for (size_t j = 0; j < i; ++j) type_free(param_types[j]);
                            free(param_types);
                            type_free(obj);
                            return NULL;
                        }
                        param_types[i] = ptype;
                    }
                }
                Type* ret_type = NULL;
                if (decl->return_type) {
                    ret_type = resolve_type_from_ast(ctx, decl->return_type);
                    if (!ret_type) {
                        for (size_t i = 0; i < param_count; ++i) type_free(param_types[i]);
                        free(param_types);
                        type_free(obj);
                        return NULL;
                    }
                } else {
                    ret_type = type_Void;
                }
                Type* func_type = type_function(param_types, param_count, ret_type, false);
                /* Instance method call: bind the receiver. When the
                   method's first parameter has the object's type, it
                   is the receiver and is bound to the object, so the
                   callable type drops it. */
                if (!object_is_type && param_count > 0 &&
                    type_equal(param_types[0], obj)) {
                    Type** bound = NULL;
                    if (param_count > 1) {
                        bound = (Type**)calloc(param_count - 1, sizeof(Type*));
                        if (!bound) {
                            type_free(func_type);
                            type_free(obj);
                            return NULL;
                        }
                        for (size_t i = 1; i < param_count; ++i) {
                            bound[i - 1] = param_types[i];
                        }
                    }
                    type_free(param_types[0]);
                    free(param_types);
                    func_type->u.function.param_types = bound;
                    func_type->u.function.param_count = param_count - 1;
                }
                type_free(obj);
                return func_type;
            }
            method = method->next;
        }

        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ma->base.line, ma->base.column, 1,
                                    "semantic", "Member '%s' not found in struct/union", ma->member);
        type_free(obj);
        return NULL;
    } else if (obj->kind == TYPE_KIND_ENUM) {
        Symbol* enum_sym = obj->u.named;
        if (!enum_sym) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, ma->base.line, ma->base.column, 1,
                                        "semantic", "Invalid enum type");
            type_free(obj);
            return NULL;
        }
        FieldInfo* field = enum_sym->extra.fields;
        while (field) {
            if (strcmp(field->name, ma->member) == 0) {
                Type* result = type_copy(obj);
                type_free(obj);
                return result;
            }
            field = field->next;
        }
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ma->base.line, ma->base.column, 1,
                                    "semantic", "Enum constant '%s' not found", ma->member);
        type_free(obj);
        return NULL;
    } else {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ma->base.line, ma->base.column, 1,
                                    "semantic", "Member access on non-struct/union/enum");
        type_free(obj);
        return NULL;
    }
}

/*
 * Analyze a module access expression.
 */
Type* analyze_module_access(SemanticContext* ctx, ModuleAccess* ma) {
    char full_path[256] = {0};
    for (size_t i = 0; i < ma->path_len; ++i) {
        if (i > 0) strcat(full_path, "::");
        strcat(full_path, ma->path[i]);
    }

    Symbol* mod_sym = scope_lookup(ctx->current_scope, full_path);
    if (!mod_sym || mod_sym->kind != SYMBOL_MODULE) {
        if (strcmp(full_path, "std::type") == 0) {
            Scope* builtin_scope = builtins_get_scope();
            if (builtin_scope) {
                Symbol* target = scope_lookup_local(builtin_scope, ma->symbol);
                if (target && target->is_public) {
                    target->is_used = true;
                    if (target->type) return type_copy(target->type);
                    return type_basic(TYPE_BASIC_VOID);
                }
            }
        }
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ma->base.line, ma->base.column, 1,
                                    "semantic", "Module '%s' not found", full_path);
        return NULL;
    }
    Scope* mod_scope = mod_sym->extra.module_scope;
    if (!mod_scope) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ma->base.line, ma->base.column, 1,
                                    "semantic", "Module '%s' has no scope", full_path);
        return NULL;
    }
    Symbol* target = scope_lookup_local(mod_scope, ma->symbol);
    if (!target) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ma->base.line, ma->base.column, 1,
                                    "semantic", "Symbol '%s' not found in module '%s'", ma->symbol, full_path);
        return NULL;
    }
    if (!target->is_public) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ma->base.line, ma->base.column, 1,
                                    "semantic", "Symbol '%s' is not public in module '%s'", ma->symbol, full_path);
        return NULL;
    }
    target->is_used = true;
    if (target->type) return type_copy(target->type);
    return type_basic(TYPE_BASIC_VOID);
}

/*
 * Analyze an index expression.
 */
Type* analyze_index(SemanticContext* ctx, IndexExpr* idx) {
    Type* arr = analyze_expression(ctx, idx->array);
    if (!arr) return NULL;
    if (arr->kind == TYPE_KIND_TUPLE) {
        int64_t index_val;
        if (!eval_const_expr(ctx, idx->index, &index_val)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, idx->index->line, idx->index->column, 1,
                                        "semantic", "Tuple index must be a constant integer");
            type_free(arr);
            return NULL;
        }
        if (index_val < 0 || (size_t)index_val >= arr->u.tuple.count) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, idx->index->line, idx->index->column, 1,
                                        "semantic", "Tuple index out of bounds");
            type_free(arr);
            return NULL;
        }
        Type* result = type_copy(arr->u.tuple.elements[index_val]);
        type_free(arr);
        return result;
    }
    if (arr->kind != TYPE_KIND_ARRAY && arr->kind != TYPE_KIND_POINTER) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, idx->array->line, idx->array->column, 1,
                                    "semantic", "Indexing non-array/pointer/tuple");
        type_free(arr);
        return NULL;
    }
    Type* index_type = analyze_expression(ctx, idx->index);
    if (!index_type) { type_free(arr); return NULL; }
    if (!type_is_integer(index_type)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, idx->index->line, idx->index->column, 1,
                                    "semantic", "Index must be integer");
        type_free(arr); type_free(index_type);
        return NULL;
    }
    Type* result = (arr->kind == TYPE_KIND_ARRAY) ? type_copy(arr->u.array.base)
                                                  : type_copy(arr->u.pointer.base);
    type_free(arr);
    type_free(index_type);
    return result;
}

/*
 * Analyze a slice expression.
 */
Type* analyze_slice(SemanticContext* ctx, SliceExpr* slice) {
    Type* arr = analyze_expression(ctx, slice->array);
    if (!arr) return NULL;
    if (arr->kind != TYPE_KIND_ARRAY && arr->kind != TYPE_KIND_POINTER) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, slice->array->line, slice->array->column, 1,
                                    "semantic", "Slicing non-array/pointer");
        type_free(arr);
        return NULL;
    }
    Type* range_type = analyze_expression(ctx, slice->range);
    if (!range_type) { type_free(arr); return NULL; }
    type_free(range_type);

    int64_t slice_size = -1;
    if (slice->range->type == NODE_RANGE) {
        RangeExpr* r = (RangeExpr*)slice->range;
        int64_t start_val = 0, end_val = 0;
        bool start_const = r->start ? eval_const_expr(ctx, r->start, &start_val) : true;
        bool end_const = r->end ? eval_const_expr(ctx, r->end, &end_val) : true;
        if (arr->kind == TYPE_KIND_ARRAY && start_const && end_const) {
            int64_t arr_size = arr->u.array.size_value;
            if (arr_size >= 0) {
                int64_t s = start_const ? start_val : 0;
                int64_t e = end_const ? end_val : arr_size;
                if (r->exclude_start) s++;
                if (!r->exclude_end) e++;
                if (e > arr_size) e = arr_size;
                if (s < 0) s = 0;
                slice_size = e - s;
                if (slice_size < 0) slice_size = 0;
            }
        }
    }

    Type* elem_type = (arr->kind == TYPE_KIND_ARRAY) ? arr->u.array.base : arr->u.pointer.base;
    Type* result = type_array(type_copy(elem_type), slice_size);
    type_free(arr);
    return result;
}

/*
 * Analyze a struct initializer expression.
 */
Type* analyze_struct_init(SemanticContext* ctx, StructInitExpr* init) {
    Type* struct_type = analyze_expression(ctx, init->struct_type);
    if (!struct_type) return NULL;
    if (struct_type->kind != TYPE_KIND_STRUCT && struct_type->kind != TYPE_KIND_UNION) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, init->struct_type->line, init->struct_type->column, 1,
                                    "semantic", "Struct initializer for non-struct");
        type_free(struct_type);
        return NULL;
    }
    Symbol* type_sym = struct_type->u.named;
    if (!type_sym) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, init->base.line, init->base.column, 1,
                                    "semantic", "Invalid struct type");
        type_free(struct_type);
        return NULL;
    }

    FieldInfo* fields = type_sym->extra.fields;
    size_t field_count = 0;
    while (fields) { field_count++; fields = fields->next; }

    bool ok = true;
    if (init->named) {
        for (size_t i = 0; i < init->count; ++i) {
            const char* fname = (const char*)init->field_names[i];
            FieldInfo* f = type_sym->extra.fields;
            bool found = false;
            while (f) {
                if (strcmp(f->name, fname) == 0) { found = true; break; }
                f = f->next;
            }
            if (!found) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, init->base.line, init->base.column, 1,
                                            "semantic", "Field '%s' not found", fname);
                ok = false;
                break;
            }
            Type* val_type = analyze_expression(ctx, init->values[i]);
            if (!val_type) { ok = false; break; }
            if (!type_compatible(val_type, f->type)) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, init->values[i]->line, init->values[i]->column, 1,
                                            "semantic", "Field '%s' type mismatch", fname);
                type_free(val_type);
                ok = false;
                break;
            }
            type_free(val_type);
        }
        if (ok) {
            FieldInfo* f = type_sym->extra.fields;
            while (f) {
                bool mentioned = false;
                for (size_t i = 0; i < init->count; ++i) {
                    if (strcmp(init->field_names[i], f->name) == 0) { mentioned = true; break; }
                }
                if (!mentioned) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, init->base.line, init->base.column, 1,
                                                "semantic", "Field '%s' is not initialized", f->name);
                    ok = false;
                    break;
                }
                f = f->next;
            }
        }
    } else {
        if (init->count != field_count) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, init->base.line, init->base.column, 1,
                                        "semantic", "Field count mismatch: expected %zu, got %zu",
                                        field_count, init->count);
            ok = false;
        } else {
            FieldInfo* f = type_sym->extra.fields;
            for (size_t i = 0; i < init->count; ++i) {
                Type* val_type = analyze_expression(ctx, init->values[i]);
                if (!val_type) { ok = false; break; }
                if (!type_compatible(val_type, f->type)) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, init->values[i]->line, init->values[i]->column, 1,
                                                "semantic", "Field %zu type mismatch", i+1);
                    type_free(val_type);
                    ok = false;
                    break;
                }
                type_free(val_type);
                f = f->next;
            }
        }
    }

    if (!ok) {
        type_free(struct_type);
        return NULL;
    }
    Type* result = type_copy(struct_type);
    type_free(struct_type);
    return result;
}

/*
 * Analyze a range expression.
 */
Type* analyze_range(SemanticContext* ctx, RangeExpr* range) {
    Type* start = analyze_expression(ctx, range->start);
    Type* end = analyze_expression(ctx, range->end);
    if (!start || !end) { type_free(start); type_free(end); return NULL; }
    if (!type_is_integer(start) || !type_is_integer(end)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, range->base.line, range->base.column, 1,
                                    "semantic", "Range bounds must be integers");
        type_free(start); type_free(end);
        return NULL;
    }
    type_free(start); type_free(end);
    return type_array(type_basic(TYPE_BASIC_INT32), -1);
}

/*
 * Analyze an if expression.
 */
Type* analyze_if_expression(SemanticContext* ctx, IfExpr* ifexpr) {
    Type* cond = analyze_expression(ctx, ifexpr->condition);
    if (!cond) return NULL;
    if (!type_compatible(cond, type_Bool)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, ifexpr->condition->line, ifexpr->condition->column, 1,
                                    "semantic", "If condition must be bool-compatible");
        type_free(cond);
        return NULL;
    }
    type_free(cond);
    if (!ifexpr->then_expr) {
        /* Branchless form: the value of the expression is the
           condition's value, so the type is Bool. */
        return type_Bool;
    }
    Type* then_type = analyze_expression(ctx, ifexpr->then_expr);
    if (!then_type) return NULL;
    Type* else_type = NULL;
    if (ifexpr->else_expr) {
        else_type = analyze_expression(ctx, ifexpr->else_expr);
        if (!else_type) { type_free(then_type); return NULL; }
    }
    Type* result;
    if (else_type) {
        if (type_equal(then_type, else_type)) {
            result = then_type;
            type_free(else_type);
        } else {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, ifexpr->base.line, ifexpr->base.column, 1,
                                        "semantic", "If expression branches have incompatible types");
            type_free(then_type);
            type_free(else_type);
            return NULL;
        }
    } else {
        result = type_Bool;
        type_free(then_type);
    }
    return result;
}

/*
 * Analyze a try expression.
 */
Type* analyze_try_expression(SemanticContext* ctx, TryExpr* tryexpr) {
    Type* inner = analyze_expression(ctx, tryexpr->expr);
    if (!inner) return NULL;
    if (inner->kind != TYPE_KIND_ERROR) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, tryexpr->expr->line, tryexpr->expr->column, 1,
                                    "semantic", "Try applied to non-error type");
        type_free(inner);
        return NULL;
    }
    Type* result = type_copy(inner->u.error.base);
    type_free(inner);
    return result;
}

/*
 * Analyze a catch expression.
 */
Type* analyze_catch_expression(SemanticContext* ctx, CatchExpr* catchexpr) {
    Type* expr_type = analyze_expression(ctx, catchexpr->expr);
    if (!expr_type) return NULL;
    if (expr_type->kind != TYPE_KIND_ERROR) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, catchexpr->expr->line, catchexpr->expr->column, 1,
                                    "semantic", "Catch applied to non-error type");
        type_free(expr_type);
        return NULL;
    }
    Type* base = expr_type->u.error.base;
    analyze_statement(ctx, catchexpr->handler);
    type_free(expr_type);
    return type_copy(base);
}

/*
 * Analyze an error expression.
 */
Type* analyze_error_expression(SemanticContext* ctx, ErrorExpr* errorexpr) {
    Type* code_type = analyze_expression(ctx, errorexpr->code);
    if (!code_type) return NULL;
    if (!type_is_integer(code_type)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, errorexpr->code->line, errorexpr->code->column, 1,
                                    "semantic", "Error code must be integer");
        type_free(code_type);
        return NULL;
    }
    type_free(code_type);
    return type_error(type_Void);
}

/*
 * Analyze a cast expression.
 */
Type* analyze_cast(SemanticContext* ctx, CastExpr* cast) {
    Type* expr_type = analyze_expression(ctx, cast->expr);
    if (!expr_type) return NULL;
    Type* target_type = resolve_type_from_ast(ctx, cast->target_type);
    if (!target_type) {
        type_free(expr_type);
        return NULL;
    }
    if (ctx->extra_warnings) {
        /* optional warning */
    }
    type_free(expr_type);
    return target_type;
}

/*
 * Analyze a brace literal in tuple position: {expr, ...} where a
 * tuple is expected. Each element is analyzed independently and the
 * result is the tuple of the element types.
 */
Type* analyze_brace_as_tuple(SemanticContext* ctx, ArrayLiteral* arr) {
    Type** elems = (Type**)calloc(arr->count, sizeof(Type*));
    if (!elems) return NULL;
    bool ok = true;
    for (size_t i = 0; i < arr->count; ++i) {
        Type* t = analyze_expression(ctx, arr->elements[i]);
        if (!t) { ok = false; break; }
        elems[i] = t;
    }
    if (!ok) {
        for (size_t i = 0; i < arr->count; ++i) type_free(elems[i]);
        free(elems);
        return NULL;
    }
    return type_tuple(elems, arr->count);
}

/*
 * Analyze a match expression: each pattern must be compatible with
 * the subject type, and every case value and the else value must
 * have the same type. The result is that common type.
 */
Type* analyze_match_expr(SemanticContext* ctx, MatchExpr* me) {
    if (!me->else_value) {
        /* The parser reported the missing else branch already. */
        return NULL;
    }
    Type* subject_type = analyze_expression(ctx, me->subject);
    if (!subject_type) return NULL;
    bool ok = true;
    Type* result = NULL;
    for (size_t i = 0; i < me->case_count; ++i) {
        MatchExprCase* mc = (MatchExprCase*)me->cases[i];
        for (size_t j = 0; j < mc->pattern_count; ++j) {
            Type* pat_type = analyze_expression(ctx, mc->patterns[j]);
            if (!pat_type) { ok = false; continue; }
            if (!type_compatible(pat_type, subject_type)) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, mc->patterns[j]->line,
                                            mc->patterns[j]->column, 1,
                                            "semantic", "Pattern type incompatible with matched expression");
                type_free(pat_type);
                ok = false;
                continue;
            }
            type_free(pat_type);
        }
        Type* vtype = analyze_expression(ctx, mc->value);
        if (!vtype) { ok = false; continue; }
        if (!result) {
            result = vtype;
        } else if (!type_equal(result, vtype)) {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, mc->value->line, mc->value->column, 1,
                                        "semantic", "Match expression branches have incompatible types");
            type_free(vtype);
            ok = false;
        } else {
            type_free(vtype);
        }
    }
    Type* else_type = analyze_expression(ctx, me->else_value);
    if (!else_type) {
        ok = false;
    } else if (!result) {
        result = else_type;
    } else if (!type_equal(result, else_type)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, me->else_value->line, me->else_value->column, 1,
                                    "semantic", "Match expression else branch has incompatible type");
        type_free(else_type);
        ok = false;
    } else {
        type_free(else_type);
    }
    type_free(subject_type);
    if (!ok) {
        type_free(result);
        return NULL;
    }
    return result;
}

/*
 * Analyze an interpolated string.
 */
Type* analyze_interpolated_string(SemanticContext* ctx, InterpolatedString* node) {
    bool ok = true;
    for (size_t i = 0; i < node->segment_count; ++i) {
        InterpolatedSegment* seg = node->segments[i];
        if (seg->kind == SEG_EXPR) {
            Type* t = analyze_expression(ctx, seg->expr);
            if (!t) {
                ok = false;
                continue;
            }
            type_free(t);
        }
    }
    if (!ok) return NULL;
    return type_array(type_basic(TYPE_BASIC_UTF8), -1);
}

/*
 * Analyze an array literal.
 */
Type* analyze_array_literal(SemanticContext* ctx, ArrayLiteral* arr) {
    if (arr->count == 0) {
        return type_array(type_basic(TYPE_BASIC_INT32), 0);
    }
    Type** element_types = (Type**)calloc(arr->count, sizeof(Type*));
    if (!element_types) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, arr->base.line, arr->base.column, 1,
                                    "semantic", "Out of memory");
        return NULL;
    }
    bool ok = true;
    for (size_t i = 0; i < arr->count; i++) {
        Type* t = analyze_expression(ctx, arr->elements[i]);
        if (!t) { ok = false; break; }
        element_types[i] = t;
    }
    if (!ok) {
        for (size_t i = 0; i < arr->count; i++) {
            type_free(element_types[i]);
        }
        free(element_types);
        return NULL;
    }

    Type* common = NULL;
    bool all_numeric = true;
    for (size_t i = 0; i < arr->count; i++) {
        if (!type_is_numeric(element_types[i])) {
            all_numeric = false;
            break;
        }
    }
    if (all_numeric) {
        bool has_float = false;
        for (size_t i = 0; i < arr->count; i++) {
            if (type_is_real(element_types[i])) {
                has_float = true;
                break;
            }
        }
        if (has_float) {
            common = type_basic(TYPE_BASIC_REAL64);
        } else {
            int max_width = 0;
            bool any_unsigned = false;
            for (size_t i = 0; i < arr->count; i++) {
                Type* t = element_types[i];
                if (t->kind != TYPE_KIND_BASIC) {
                    common = type_basic(TYPE_BASIC_INT64);
                    break;
                }
                int width = 0;
                bool unsig = false;
                BasicTypeKind b = t->u.basic;
                switch (b) {
                    case TYPE_BASIC_INT8: width = 8; break;
                    case TYPE_BASIC_INT16: width = 16; break;
                    case TYPE_BASIC_INT32: width = 32; break;
                    case TYPE_BASIC_INT64: width = 64; break;
                    case TYPE_BASIC_INT128: width = 128; break;
                    case TYPE_BASIC_INT256: width = 256; break;
                    case TYPE_BASIC_UINT8: width = 8; unsig = true; break;
                    case TYPE_BASIC_UINT16: width = 16; unsig = true; break;
                    case TYPE_BASIC_UINT32: width = 32; unsig = true; break;
                    case TYPE_BASIC_UINT64: width = 64; unsig = true; break;
                    case TYPE_BASIC_UINT128: width = 128; unsig = true; break;
                    case TYPE_BASIC_UINT256: width = 256; unsig = true; break;
                    default: width = 64; break;
                }
                if (width > max_width) max_width = width;
                if (unsig) any_unsigned = true;
            }
            if (common) {
                /* already set */
            } else {
                if (any_unsigned) {
                    switch (max_width) {
                        case 8: common = type_basic(TYPE_BASIC_UINT8); break;
                        case 16: common = type_basic(TYPE_BASIC_UINT16); break;
                        case 32: common = type_basic(TYPE_BASIC_UINT32); break;
                        case 64: common = type_basic(TYPE_BASIC_UINT64); break;
                        case 128: common = type_basic(TYPE_BASIC_UINT128); break;
                        case 256: common = type_basic(TYPE_BASIC_UINT256); break;
                        default: common = type_basic(TYPE_BASIC_UINT64); break;
                    }
                } else {
                    switch (max_width) {
                        case 8: common = type_basic(TYPE_BASIC_INT8); break;
                        case 16: common = type_basic(TYPE_BASIC_INT16); break;
                        case 32: common = type_basic(TYPE_BASIC_INT32); break;
                        case 64: common = type_basic(TYPE_BASIC_INT64); break;
                        case 128: common = type_basic(TYPE_BASIC_INT128); break;
                        case 256: common = type_basic(TYPE_BASIC_INT256); break;
                        default: common = type_basic(TYPE_BASIC_INT64); break;
                    }
                }
            }
        }
        for (size_t i = 0; i < arr->count; i++) {
            type_free(element_types[i]);
        }
        free(element_types);
        return type_array(common, (int64_t)arr->count);
    } else {
        common = element_types[0];
        bool same = true;
        for (size_t i = 1; i < arr->count; i++) {
            if (!type_equal(common, element_types[i])) {
                same = false;
                break;
            }
        }
        if (same) {
            Type* result = type_array(type_copy(common), (int64_t)arr->count);
            for (size_t i = 0; i < arr->count; i++) {
                type_free(element_types[i]);
            }
            free(element_types);
            return result;
        } else {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, arr->base.line, arr->base.column, 1,
                                        "semantic", "Array literal elements have incompatible types");
            for (size_t i = 0; i < arr->count; i++) {
                type_free(element_types[i]);
            }
            free(element_types);
            return NULL;
        }
    }
}

/*
 * Analyze an alloc expression: alloc T[N] yields &T. The element
 * count N must be an integer expression.
 */
Type* analyze_alloc_expr(SemanticContext* ctx, AllocExpr* alloc) {
    Type* elem = resolve_type_from_ast(ctx, alloc->type);
    if (!elem) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, alloc->base.line, alloc->base.column, 1,
                                    "semantic", "Invalid element type in alloc");
        return NULL;
    }
    Type* count_type = analyze_expression(ctx, alloc->count);
    if (!count_type) {
        type_free(elem);
        return NULL;
    }
    if (!type_is_integer(count_type)) {
        errhandler__report_error_ex(ERROR_LEVEL_ERROR, alloc->count->line, alloc->count->column, 1,
                                    "semantic", "alloc element count must be an integer, got %s",
                                    type_to_string(count_type));
        type_free(count_type);
        type_free(elem);
        return NULL;
    }
    type_free(count_type);
    return type_pointer(elem);
}
