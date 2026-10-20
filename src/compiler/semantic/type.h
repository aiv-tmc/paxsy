#ifndef TYPE_H
#define TYPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct ASTNode;
struct Symbol;
struct SemanticContext;

/*
 * Basic type kinds.
 */
typedef enum {
    TYPE_BASIC_INT8,
    TYPE_BASIC_INT16,
    TYPE_BASIC_INT32,
    TYPE_BASIC_INT64,
    TYPE_BASIC_INT128,
    TYPE_BASIC_INT256,
    TYPE_BASIC_UINT8,
    TYPE_BASIC_UINT16,
    TYPE_BASIC_UINT32,
    TYPE_BASIC_UINT64,
    TYPE_BASIC_UINT128,
    TYPE_BASIC_UINT256,
    TYPE_BASIC_REAL32,
    TYPE_BASIC_REAL64,
    TYPE_BASIC_UTF8,
    TYPE_BASIC_UTF16,
    TYPE_BASIC_UTF32,
    TYPE_BASIC_BOOL,
    TYPE_BASIC_VOID,
    TYPE_BASIC_AUTO
} BasicTypeKind;

/*
 * Family type kinds.
 */
typedef enum {
    TYPE_FAMILY_INT,
    TYPE_FAMILY_UINT,
    TYPE_FAMILY_REAL,
    TYPE_FAMILY_CHAR,
} FamilyTypeKind;

/*
 * Type kind enumeration.
 */
typedef enum {
    TYPE_KIND_BASIC,
    TYPE_KIND_FAMILY,
    TYPE_KIND_POINTER,
    TYPE_KIND_ARRAY,
    TYPE_KIND_TUPLE,
    TYPE_KIND_NULLABLE,
    TYPE_KIND_ERROR,
    TYPE_KIND_CHAN,
    TYPE_KIND_FUNCTION,
    TYPE_KIND_STRUCT,
    TYPE_KIND_UNION,
    TYPE_KIND_ENUM,
    TYPE_KIND_IDENT,
} TypeKind;

struct Type;

/*
 * Type structure.
 */
typedef struct Type {
    TypeKind kind;
    bool is_const;
    int64_t cached_size;
    int64_t cached_align;
    union {
        BasicTypeKind basic;
        FamilyTypeKind family;
        struct {
            struct Type* base;
        } pointer;
        struct {
            struct Type* base;
            int64_t size_value;
        } array;
        struct {
            struct Type** elements;
            size_t count;
        } tuple;
        struct {
            struct Type* base;
        } nullable;
        struct {
            struct Type* base;
        } error;
        struct {
            struct Type* base;
        } chan;
        struct {
            struct Type** param_types;
            size_t param_count;
            struct Type* return_type;
            bool return_is_error;
        } function;
        struct Symbol* named;
        struct {
            char* name;
        } ident;
    } u;
} Type;

/* Global type singletons */
extern Type* type_Int32;
extern Type* type_Bool;
extern Type* type_Void;
extern Type* type_Auto;
extern Type* type_PtrVoid;

/* Type constructors */
Type* type_basic(BasicTypeKind kind);
Type* type_family(FamilyTypeKind kind);
Type* type_pointer(Type* base);
Type* type_array(Type* base, int64_t size);
Type* type_tuple(Type** elements, size_t count);
Type* type_nullable(Type* base);
Type* type_error(Type* base);
Type* type_chan(Type* base);
Type* type_function(Type** param_types, size_t param_count, Type* return_type, bool return_is_error);
Type* type_named(struct Symbol* sym);
Type* type_ident(const char* name);

/* Type operations */
void type_free(Type* t);
Type* type_copy(const Type* t);
bool type_equal(const Type* a, const Type* b);
bool type_compatible(const Type* from, const Type* to);

/* Type predicates */
bool type_is_numeric(const Type* t);
bool type_is_integer(const Type* t);
bool type_is_real(const Type* t);
bool type_is_family(const Type* t);
bool type_is_pointer(const Type* t);
bool type_is_array(const Type* t);
bool type_is_struct(const Type* t);
bool type_is_union(const Type* t);
bool type_is_enum(const Type* t);
bool type_is_float(const Type* t);
bool type_is_signed_integer(const Type* t);
bool type_is_unsigned_integer(const Type* t);

/* Type utilities */
Type* type_family_to_concrete(FamilyTypeKind family);
const char* type_to_string(const Type* t);
Type* resolve_type_from_ast(struct SemanticContext* ctx, struct ASTNode* node);
int64_t type_sizeof(const Type* t);
int64_t type_alignof(const Type* t);

#endif
