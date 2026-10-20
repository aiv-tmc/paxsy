#ifndef PARSER_H
#define PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../lexer/lexer.h"
#include "../semantic/type.h"

typedef enum {
    ASSIGN_NORMAL,
    ASSIGN_ADD,
    ASSIGN_SUB,
    ASSIGN_MUL,
    ASSIGN_DIV,
    ASSIGN_MOD,
    ASSIGN_BIT_AND,
    ASSIGN_BIT_OR,
    ASSIGN_BIT_XOR,
    ASSIGN_SHL,
    ASSIGN_SHR,
    ASSIGN_SHL_LOG,
    ASSIGN_SHR_LOG
} AssignOp;

typedef enum {
    NODE_PROGRAM,
    NODE_MODULE,
    NODE_USE,
    NODE_FUNC_DECL,
    NODE_LET_DECL,
    NODE_COMPTIME_DECL,
    NODE_EXTERN_DECL,
    NODE_EXTERN_BLOCK,
    NODE_BLOCK,
    NODE_IF_STMT,
    NODE_IF_EXISTS,
    NODE_MATCH_STMT,
    NODE_MATCH_CASE,
    NODE_MATCH_EXPR,
    NODE_MATCH_EXPR_CASE,
    NODE_WHILE_STMT,
    NODE_DO_WHILE_STMT,
    NODE_FOR_STMT,
    NODE_BREAK_STMT,
    NODE_CONTINUE_STMT,
    NODE_RETURN_STMT,
    NODE_DEFER_STMT,
    NODE_ASM_STMT,
    NODE_VOLATILE_STMT,
    NODE_COMPTIME_STMT,
    NODE_PASS_STMT,
    NODE_EMPTY_STMT,
    NODE_EXPR_STMT,
    NODE_IDENT,
    NODE_LITERAL,
    NODE_UNARY,
    NODE_BINARY,
    NODE_ASSIGN,
    NODE_SEND,
    NODE_CALL,
    NODE_MEMBER_ACCESS,
    NODE_MODULE_ACCESS,
    NODE_INDEX,
    NODE_SLICE,
    NODE_STRUCT_INIT,
    NODE_RANGE,
    NODE_IF_EXPR,
    NODE_TRY_EXPR,
    NODE_CATCH_EXPR,
    NODE_ERROR_EXPR,
    NODE_INTERPOLATED_STRING,
    NODE_CAST,
    NODE_FAMILY_TYPE,
    NODE_BASIC_TYPE,
    NODE_POINTER_TYPE,
    NODE_ARRAY_TYPE,
    NODE_TUPLE_TYPE,
    NODE_NULLABLE_TYPE,
    NODE_ERROR_TYPE,
    NODE_CHAN_TYPE,
    NODE_FUNC_TYPE,
    NODE_IDENT_TYPE,
    NODE_STRUCT_TYPE,
    NODE_UNION_TYPE,
    NODE_ENUM_TYPE,
    NODE_ENUM_CONST,
    NODE_PARAM,
    NODE_FIELD,
    NODE_ARRAY_LITERAL,
    NODE_ALLOC_EXPR,
    NODE_FREE_STMT
} NodeType;

typedef enum {
    UNARY_PLUS,
    UNARY_MINUS,
    UNARY_LOGICAL_NOT,
    UNARY_BIT_NOT,
    UNARY_ADDR,
    UNARY_DEREF
} UnaryOp;

typedef enum {
    BIN_ADD,
    BIN_SUB,
    BIN_MUL,
    BIN_DIV,
    BIN_MOD,
    BIN_EQ,
    BIN_NE,
    BIN_GT,
    BIN_LT,
    BIN_GE,
    BIN_LE,
    BIN_AND,
    BIN_OR,
    BIN_XOR,
    BIN_SHL,
    BIN_SHR,
    BIN_SHL_LOG,
    BIN_SHR_LOG,
    BIN_LOGICAL_AND,
    BIN_LOGICAL_OR
} BinaryOp;

typedef enum {
    LIT_NUMBER,
    LIT_FLOAT,
    LIT_STRING,
    LIT_CHAR,
    LIT_BOOL,
    LIT_NONE
} LiteralKind;

typedef enum {
    SEG_TEXT,
    SEG_EXPR
} InterpolatedSegmentKind;

typedef struct ASTNode {
    NodeType type;
    uint32_t line;
    uint32_t column;
} ASTNode;

typedef struct Program {
    ASTNode base;
    struct ASTNode** items;
    size_t item_count;
    void* arena_blocks; /* owns the parser arena holding every node */
} Program;

typedef struct ModuleDecl {
    ASTNode base;
    char* name;
    struct ASTNode** body;
    size_t body_count;
} ModuleDecl;

typedef struct UseDecl {
    ASTNode base;
    char** path;
    size_t path_len;
} UseDecl;

typedef struct FuncDecl {
    ASTNode base;
    char* name;
    uint32_t modifiers;
    uint8_t is_comptime;
    char** method_of;        /* struct names of a method qualifier, NULL for a plain function */
    size_t method_of_count;
    struct ASTNode** params;
    size_t param_count;
    struct ASTNode* return_type;
    struct ASTNode* body;
    struct ASTNode* expr_body;
} FuncDecl;

typedef struct LetDecl {
    ASTNode base;
    char** names;
    struct ASTNode** types;
    size_t count;
    struct ASTNode* initializer;
    uint32_t modifiers;
    struct ASTNode* array_size;
    struct ASTNode** inits;
    size_t init_count;
} LetDecl;

typedef struct ComptimeDecl {
    ASTNode base;
    char* name;
    struct ASTNode* type_def;
    struct ASTNode* value;
    uint32_t modifiers;
} ComptimeDecl;

typedef struct ExternDecl {
    ASTNode base;
    char* name;
    bool is_func;
    struct ASTNode* type;
    struct ASTNode** params;
    size_t param_count;
} ExternDecl;

typedef struct ExternBlock {
    ASTNode base;
    struct ASTNode** items;
    size_t item_count;
} ExternBlock;

typedef struct Block {
    ASTNode base;
    struct ASTNode** statements;
    size_t stmt_count;
} Block;

typedef struct IfStmt {
    ASTNode base;
    struct ASTNode* condition;
    struct ASTNode* then_branch;
    struct ASTNode** else_if_conditions;
    struct ASTNode** else_if_branches;
    size_t else_if_count;
    struct ASTNode* else_branch;
} IfStmt;

/* Kind of a compile-time existence check: 'if let', 'if func', 'if comptime'. */
typedef enum {
    EXISTS_LET,
    EXISTS_FUNC,
    EXISTS_COMPTIME
} IfExistsKind;

typedef struct IfExistsStmt {
    ASTNode base;
    uint8_t kind;               /* IfExistsKind */
    char* name;
    struct ASTNode* then_branch;
    struct ASTNode* else_branch;    /* may be NULL */
} IfExistsStmt;

typedef struct MatchStmt {
    ASTNode base;
    struct ASTNode* expr;
    struct ASTNode** cases;
    size_t case_count;
    struct ASTNode* else_case;
} MatchStmt;

typedef struct MatchCase {
    ASTNode base;
    struct ASTNode** patterns;  /* alternatives joined by 'or' */
    size_t pattern_count;
    struct ASTNode* body;
} MatchCase;

typedef struct MatchExprCase {
    ASTNode base;
    struct ASTNode** patterns;
    size_t pattern_count;
    struct ASTNode* value;      /* value-producing expression in braces */
} MatchExprCase;

typedef struct MatchExpr {
    ASTNode base;
    struct ASTNode* subject;
    struct ASTNode** cases;     /* MatchExprCase* */
    size_t case_count;
    struct ASTNode* else_value; /* required in the expression form */
} MatchExpr;

typedef struct WhileStmt {
    ASTNode base;
    char* label;
    struct ASTNode* condition;
    struct ASTNode* body;
} WhileStmt;

typedef struct DoWhileStmt {
    ASTNode base;
    char* label;
    struct ASTNode* body;
    struct ASTNode* condition;
} DoWhileStmt;

typedef struct ForStmt {
    ASTNode base;
    char* label;
    char* var_name;
    struct ASTNode* var_type;
    struct ASTNode* range;
    struct ASTNode* body;
} ForStmt;

typedef struct BreakStmt {
    ASTNode base;
    char* label;
} BreakStmt;

typedef struct ContinueStmt {
    ASTNode base;
    char* label;
} ContinueStmt;

typedef struct ReturnStmt {
    ASTNode base;
    struct ASTNode* value;
} ReturnStmt;

typedef struct DeferStmt {
    ASTNode base;
    struct ASTNode* stmt;
} DeferStmt;

typedef struct AsmStmt {
    ASTNode base;
    struct ASTNode* code;
} AsmStmt;

typedef struct VolatileStmt {
    ASTNode base;
    struct ASTNode* stmt;
} VolatileStmt;

typedef struct ComptimeStmt {
    ASTNode base;
    char* name;
    struct ASTNode* stmt;
} ComptimeStmt;

typedef struct PassStmt {
    ASTNode base;
} PassStmt;

typedef struct ExprStmt {
    ASTNode base;
    struct ASTNode* expr;
} ExprStmt;

typedef struct IdentExpr {
    ASTNode base;
    char* name;
} IdentExpr;

typedef struct LiteralExpr {
    ASTNode base;
    LiteralKind kind;
    char* raw_value;
    char* string_val;
    bool bool_val;
} LiteralExpr;

typedef struct UnaryExpr {
    ASTNode base;
    UnaryOp op;
    struct ASTNode* operand;
} UnaryExpr;

typedef struct BinaryExpr {
    ASTNode base;
    BinaryOp op;
    struct ASTNode* left;
    struct ASTNode* right;
} BinaryExpr;

typedef struct AssignExpr {
    ASTNode base;
    AssignOp op;
    struct ASTNode* left;
    struct ASTNode* right;
} AssignExpr;

typedef struct SendExpr {
    ASTNode base;
    struct ASTNode* target;
    struct ASTNode* message;
} SendExpr;

typedef struct CallExpr {
    ASTNode base;
    struct ASTNode* callee;
    struct ASTNode** args;
    size_t arg_count;
} CallExpr;

typedef struct MemberAccess {
    ASTNode base;
    struct ASTNode* object;
    char* member;
} MemberAccess;

typedef struct ModuleAccess {
    ASTNode base;
    char** path;
    size_t path_len;
    char* symbol;
} ModuleAccess;

typedef struct IndexExpr {
    ASTNode base;
    struct ASTNode* array;
    struct ASTNode* index;
} IndexExpr;

typedef struct SliceExpr {
    ASTNode base;
    struct ASTNode* array;
    struct ASTNode* range;
} SliceExpr;

typedef struct StructInitExpr {
    ASTNode base;
    struct ASTNode* struct_type;
    char** field_names;
    struct ASTNode** values;
    size_t count;
    bool named;
} StructInitExpr;

typedef struct RangeExpr {
    ASTNode base;
    struct ASTNode* start;
    struct ASTNode* end;
    bool exclude_start;
    bool exclude_end;
} RangeExpr;

typedef struct IfExpr {
    ASTNode base;
    struct ASTNode* condition;
    struct ASTNode* then_expr;
    struct ASTNode* else_expr;
} IfExpr;

typedef struct TryExpr {
    ASTNode base;
    struct ASTNode* expr;
} TryExpr;

typedef struct CatchExpr {
    ASTNode base;
    struct ASTNode* expr;
    char* var_name;
    struct ASTNode* handler;
} CatchExpr;

typedef struct ErrorExpr {
    ASTNode base;
    struct ASTNode* code;
} ErrorExpr;

typedef struct InterpolatedSegment {
    InterpolatedSegmentKind kind;
    char* text;
    struct ASTNode* expr;
} InterpolatedSegment;

typedef struct InterpolatedString {
    ASTNode base;
    InterpolatedSegment** segments;
    size_t segment_count;
} InterpolatedString;

typedef struct CastExpr {
    ASTNode base;
    struct ASTNode* expr;
    struct ASTNode* target_type;
} CastExpr;

typedef struct ArrayLiteral {
    ASTNode base;
    struct ASTNode** elements;
    size_t count;
} ArrayLiteral;

typedef struct AllocExpr {
    ASTNode base;
    struct ASTNode* type;   /* element type */
    struct ASTNode* count;  /* element count expression */
} AllocExpr;

typedef struct FreeStmt {
    ASTNode base;
    struct ASTNode* expr;
} FreeStmt;

typedef struct ChanType {
    ASTNode base;
    struct ASTNode* element;
} ChanType;

typedef struct FamilyType {
    ASTNode base;
    FamilyTypeKind kind;
} FamilyType;

typedef struct BasicType {
    ASTNode base;
    BasicTypeKind kind;
} BasicType;

typedef struct PointerType {
    ASTNode base;
    struct ASTNode* base_type;
} PointerType;

typedef struct ArrayType {
    ASTNode base;
    struct ASTNode* base_type;
    struct ASTNode* size;
} ArrayType;

typedef struct TupleType {
    ASTNode base;
    struct ASTNode** elements;
    size_t count;
} TupleType;

typedef struct NullableType {
    ASTNode base;
    struct ASTNode* base_type;
} NullableType;

typedef struct ErrorType {
    ASTNode base;
    struct ASTNode* base_type;
} ErrorType;

typedef struct FuncType {
    ASTNode base;
    struct ASTNode** param_types;
    size_t param_count;
    struct ASTNode* return_type;
} FuncType;

typedef struct IdentType {
    ASTNode base;
    char* name;
} IdentType;

typedef struct StructType {
    ASTNode base;
    struct ASTNode** fields;
    size_t field_count;
    struct ASTNode** methods;
    size_t method_count;
} StructType;

typedef struct UnionType {
    ASTNode base;
    struct ASTNode** fields;
    size_t field_count;
} UnionType;

typedef struct EnumType {
    ASTNode base;
    struct ASTNode** constants;
    size_t const_count;
} EnumType;

typedef struct EnumConst {
    ASTNode base;
    char* name;
    struct ASTNode* value;
} EnumConst;

typedef struct Param {
    ASTNode base;
    char* name;
    struct ASTNode* type;   /* NULL for a comptime parameter */
    uint32_t modifiers;
    uint8_t is_volatile;
    uint8_t is_comptime;    /* comptime parameter: type inferred at the call site */
} Param;

typedef struct Field {
    ASTNode base;
    char* name;
    struct ASTNode* type;
} Field;

#define MOD_EXTERN      (1U << 0)
#define MOD_VOLATILE    (1U << 1)
#define MOD_PUBLIC      (1U << 2)

Program* parse(Token* tokens, size_t count);
void parser_free_ast(Program* prog);

#endif
