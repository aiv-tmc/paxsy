#ifndef EXPRESSION_H
#define EXPRESSION_H

#include "type.h"

struct ASTNode;
struct IdentExpr;
struct LiteralExpr;
struct UnaryExpr;
struct BinaryExpr;
struct AssignExpr;
struct SendExpr;
struct CallExpr;
struct MemberAccess;
struct ModuleAccess;
struct IndexExpr;
struct SliceExpr;
struct StructInitExpr;
struct RangeExpr;
struct IfExpr;
struct TryExpr;
struct CatchExpr;
struct ErrorExpr;
struct InterpolatedString;
struct CastExpr;
struct ArrayLiteral;
struct MatchExpr;
struct AllocExpr;

struct SemanticContext;

Type* analyze_expression(struct SemanticContext* ctx, struct ASTNode* node);

Type* analyze_ident(struct SemanticContext* ctx, struct IdentExpr* id);
Type* analyze_literal(struct SemanticContext* ctx, struct LiteralExpr* lit);
Type* analyze_unary(struct SemanticContext* ctx, struct UnaryExpr* unary);
Type* analyze_binary(struct SemanticContext* ctx, struct BinaryExpr* binary);
Type* analyze_assign(struct SemanticContext* ctx, struct AssignExpr* assign);
Type* analyze_send(struct SemanticContext* ctx, struct SendExpr* send);
Type* analyze_call(struct SemanticContext* ctx, struct CallExpr* call);
Type* analyze_member_access(struct SemanticContext* ctx, struct MemberAccess* ma);
Type* analyze_module_access(struct SemanticContext* ctx, struct ModuleAccess* ma);
Type* analyze_index(struct SemanticContext* ctx, struct IndexExpr* idx);
Type* analyze_slice(struct SemanticContext* ctx, struct SliceExpr* slice);
Type* analyze_struct_init(struct SemanticContext* ctx, struct StructInitExpr* init);
Type* analyze_range(struct SemanticContext* ctx, struct RangeExpr* range);
Type* analyze_if_expression(struct SemanticContext* ctx, struct IfExpr* ifexpr);
Type* analyze_try_expression(struct SemanticContext* ctx, struct TryExpr* tryexpr);
Type* analyze_catch_expression(struct SemanticContext* ctx, struct CatchExpr* catchexpr);
Type* analyze_error_expression(struct SemanticContext* ctx, struct ErrorExpr* errorexpr);
Type* analyze_cast(struct SemanticContext* ctx, struct CastExpr* cast);
Type* analyze_brace_as_tuple(struct SemanticContext* ctx, struct ArrayLiteral* arr);
Type* analyze_match_expr(struct SemanticContext* ctx, struct MatchExpr* me);
Type* analyze_interpolated_string(struct SemanticContext* ctx, struct InterpolatedString* node);
Type* analyze_array_literal(struct SemanticContext* ctx, struct ArrayLiteral* arr);
Type* analyze_alloc_expr(struct SemanticContext* ctx, struct AllocExpr* alloc);

#endif
