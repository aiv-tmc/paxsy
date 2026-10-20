#include "type.h"
#include "symbol.h"
#include "semantic.h"
#include "comptime.h"
#include "../parser/parser.h"
#include "../../interfaces/errhandler/errhandler.h"
#include "../../interfaces/utils/common.h"
#include <string.h>
#include <stdlib.h>

static Type* basic_type_cache[TYPE_BASIC_AUTO + 1] = {0};
Type* type_Int32 = NULL;
Type* type_Bool = NULL;
Type* type_Void = NULL;
Type* type_Auto = NULL;
Type* type_PtrVoid = NULL;

/* Allocate a type structure */
static Type* alloc_type(TypeKind kind) {
    Type* t = (Type*)memory_allocate_zero(sizeof(Type));
    if (t) {
        t->kind = kind;
        t->cached_size = -1;
        t->cached_align = -1;
    }
    return t;
}

/* Initialize type cache */
static void init_type_cache(void) {
    static bool initialized = false;
    if (initialized) return;
    initialized = true; /* set before the recursive type_basic/type_pointer calls below */
    for (int i = 0; i <= TYPE_BASIC_AUTO; i++) {
        basic_type_cache[i] = alloc_type(TYPE_KIND_BASIC);
        if (basic_type_cache[i]) basic_type_cache[i]->u.basic = (BasicTypeKind)i;
    }
    type_Int32 = basic_type_cache[TYPE_BASIC_INT32];
    type_Bool = basic_type_cache[TYPE_BASIC_BOOL];
    type_Void = basic_type_cache[TYPE_BASIC_VOID];
    type_Auto = basic_type_cache[TYPE_BASIC_AUTO];
    type_PtrVoid = type_pointer(type_Void);
}

/* Create a basic type */
Type* type_basic(BasicTypeKind kind) {
    init_type_cache();
    if (kind >= 0 && kind <= TYPE_BASIC_AUTO) {
        return basic_type_cache[kind];
    }
    Type* t = alloc_type(TYPE_KIND_BASIC);
    if (t) t->u.basic = kind;
    return t;
}

/* Create a family type */
Type* type_family(FamilyTypeKind kind) {
    Type* t = alloc_type(TYPE_KIND_FAMILY);
    if (t) t->u.family = kind;
    return t;
}

/* Create a pointer type */
Type* type_pointer(Type* base) {
    if (base == type_Void) {
        init_type_cache();
        if (!type_PtrVoid) type_PtrVoid = alloc_type(TYPE_KIND_POINTER);
        if (type_PtrVoid) type_PtrVoid->u.pointer.base = type_Void;
        return type_PtrVoid;
    }
    Type* t = alloc_type(TYPE_KIND_POINTER);
    if (t) t->u.pointer.base = base;
    return t;
}

/* Create an array type */
Type* type_array(Type* base, int64_t size) {
    Type* t = alloc_type(TYPE_KIND_ARRAY);
    if (t) {
        t->u.array.base = base;
        t->u.array.size_value = size;
    }
    return t;
}

/* Create a tuple type */
Type* type_tuple(Type** elements, size_t count) {
    Type* t = alloc_type(TYPE_KIND_TUPLE);
    if (t) { t->u.tuple.elements = elements; t->u.tuple.count = count; }
    return t;
}

/* Create a nullable type */
Type* type_nullable(Type* base) {
    Type* t = alloc_type(TYPE_KIND_NULLABLE);
    if (t) t->u.nullable.base = base;
    return t;
}

/* Create an error type */
Type* type_error(Type* base) {
    Type* t = alloc_type(TYPE_KIND_ERROR);
    if (t) t->u.error.base = base;
    return t;
}

/* Create a channel type for the given element type */
Type* type_chan(Type* base) {
    Type* t = alloc_type(TYPE_KIND_CHAN);
    if (t) t->u.chan.base = base;
    return t;
}

/* Create a function type */
Type* type_function(Type** param_types, size_t param_count, Type* return_type, bool return_is_error) {
    Type* t = alloc_type(TYPE_KIND_FUNCTION);
    if (t) {
        t->u.function.param_types = param_types;
        t->u.function.param_count = param_count;
        t->u.function.return_type = return_type;
        t->u.function.return_is_error = return_is_error;
    }
    return t;
}

/* Create a named type */
Type* type_named(Symbol* sym) {
    Type* t = alloc_type(TYPE_KIND_STRUCT);
    if (t) t->u.named = sym;
    return t;
}

/* Create an identifier type */
Type* type_ident(const char* name) {
    Type* t = alloc_type(TYPE_KIND_IDENT);
    if (t) t->u.ident.name = (char*)name;
    return t;
}

/* Free a type */
void type_free(Type* t) {
    if (!t) return;
    if (t == type_Int32 || t == type_Bool || t == type_Void || t == type_Auto || t == type_PtrVoid) return;
    if (t->kind == TYPE_KIND_BASIC) {
        BasicTypeKind b = t->u.basic;
        if (b >= 0 && b <= TYPE_BASIC_AUTO && basic_type_cache[b] == t) return;
    }
    switch (t->kind) {
        case TYPE_KIND_POINTER:
            type_free(t->u.pointer.base);
            break;
        case TYPE_KIND_ARRAY:
            type_free(t->u.array.base);
            break;
        case TYPE_KIND_TUPLE:
            for (size_t i = 0; i < t->u.tuple.count; ++i)
                type_free(t->u.tuple.elements[i]);
            free(t->u.tuple.elements);
            break;
        case TYPE_KIND_NULLABLE:
            type_free(t->u.nullable.base);
            break;
        case TYPE_KIND_ERROR:
            type_free(t->u.error.base);
            break;
        case TYPE_KIND_CHAN:
            type_free(t->u.chan.base);
            break;
        case TYPE_KIND_FUNCTION:
            for (size_t i = 0; i < t->u.function.param_count; ++i)
                type_free(t->u.function.param_types[i]);
            free(t->u.function.param_types);
            type_free(t->u.function.return_type);
            break;
        case TYPE_KIND_IDENT:
            break;
        default:
            break;
    }
    free(t);
}

/* Copy a type */
Type* type_copy(const Type* t) {
    if (!t) return NULL;
    if (t == type_Int32 || t == type_Bool || t == type_Void || t == type_Auto || t == type_PtrVoid)
        return (Type*)t;
    Type* c = alloc_type(t->kind);
    if (!c) return NULL;
    c->cached_size = t->cached_size;
    c->cached_align = t->cached_align;
    switch (t->kind) {
        case TYPE_KIND_BASIC:
            c->u.basic = t->u.basic;
            break;
        case TYPE_KIND_FAMILY:
            c->u.family = t->u.family;
            break;
        case TYPE_KIND_POINTER:
            c->u.pointer.base = type_copy(t->u.pointer.base);
            break;
        case TYPE_KIND_ARRAY:
            c->u.array.base = type_copy(t->u.array.base);
            c->u.array.size_value = t->u.array.size_value;
            break;
        case TYPE_KIND_TUPLE:
            c->u.tuple.count = t->u.tuple.count;
            c->u.tuple.elements = (Type**)calloc(c->u.tuple.count, sizeof(Type*));
            if (c->u.tuple.elements) {
                for (size_t i = 0; i < c->u.tuple.count; ++i)
                    c->u.tuple.elements[i] = type_copy(t->u.tuple.elements[i]);
            }
            break;
        case TYPE_KIND_NULLABLE:
            c->u.nullable.base = type_copy(t->u.nullable.base);
            break;
        case TYPE_KIND_ERROR:
            c->u.error.base = type_copy(t->u.error.base);
            break;
        case TYPE_KIND_CHAN:
            c->u.chan.base = type_copy(t->u.chan.base);
            break;
        case TYPE_KIND_FUNCTION:
            c->u.function.param_count = t->u.function.param_count;
            c->u.function.param_types = (Type**)calloc(c->u.function.param_count, sizeof(Type*));
            if (c->u.function.param_types) {
                for (size_t i = 0; i < c->u.function.param_count; ++i)
                    c->u.function.param_types[i] = type_copy(t->u.function.param_types[i]);
            }
            c->u.function.return_type = type_copy(t->u.function.return_type);
            c->u.function.return_is_error = t->u.function.return_is_error;
            break;
        case TYPE_KIND_STRUCT:
        case TYPE_KIND_UNION:
        case TYPE_KIND_ENUM:
            c->u.named = t->u.named;
            break;
        case TYPE_KIND_IDENT:
            c->u.ident.name = t->u.ident.name;
            break;
        default:
            break;
    }
    return c;
}

/* Compare two types for equality */
bool type_equal(const Type* a, const Type* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TYPE_KIND_BASIC:
            return a->u.basic == b->u.basic;
        case TYPE_KIND_FAMILY:
            return a->u.family == b->u.family;
        case TYPE_KIND_POINTER:
            return type_equal(a->u.pointer.base, b->u.pointer.base);
        case TYPE_KIND_ARRAY:
            return type_equal(a->u.array.base, b->u.array.base) &&
                   a->u.array.size_value == b->u.array.size_value;
        case TYPE_KIND_TUPLE:
            if (a->u.tuple.count != b->u.tuple.count) return false;
            for (size_t i = 0; i < a->u.tuple.count; ++i)
                if (!type_equal(a->u.tuple.elements[i], b->u.tuple.elements[i]))
                    return false;
            return true;
        case TYPE_KIND_NULLABLE:
            return type_equal(a->u.nullable.base, b->u.nullable.base);
        case TYPE_KIND_ERROR:
            return type_equal(a->u.error.base, b->u.error.base);
        case TYPE_KIND_CHAN:
            return type_equal(a->u.chan.base, b->u.chan.base);
        case TYPE_KIND_FUNCTION:
            if (a->u.function.param_count != b->u.function.param_count ||
                a->u.function.return_is_error != b->u.function.return_is_error)
                return false;
            for (size_t i = 0; i < a->u.function.param_count; ++i)
                if (!type_equal(a->u.function.param_types[i], b->u.function.param_types[i]))
                    return false;
            return type_equal(a->u.function.return_type, b->u.function.return_type);
        case TYPE_KIND_STRUCT:
        case TYPE_KIND_UNION:
        case TYPE_KIND_ENUM:
            return a->u.named == b->u.named;
        case TYPE_KIND_IDENT:
            return strcmp(a->u.ident.name, b->u.ident.name) == 0;
        default:
            return false;
    }
}

/* Check type compatibility for assignment */
bool type_compatible(const Type* from, const Type* to) {
    if (!from || !to) return false;
    if (type_is_family(from) || type_is_family(to))
        return false;
    if (type_equal(from, to)) return true;
    if (type_is_numeric(from) && type_is_numeric(to)) return true;
    if (from->kind == TYPE_KIND_POINTER && to->kind == TYPE_KIND_POINTER) return true;
    if (from->kind == TYPE_KIND_ARRAY && to->kind == TYPE_KIND_ARRAY) {
        return type_equal(from->u.array.base, to->u.array.base) &&
               from->u.array.size_value == to->u.array.size_value;
    }
    if (from->kind == TYPE_KIND_TUPLE && to->kind == TYPE_KIND_TUPLE) {
        if (from->u.tuple.count != to->u.tuple.count) return false;
        for (size_t i = 0; i < from->u.tuple.count; ++i) {
            if (!type_compatible(from->u.tuple.elements[i], to->u.tuple.elements[i]))
                return false;
        }
        return true;
    }
    if (to->kind == TYPE_KIND_NULLABLE) {
        if (from->kind == TYPE_KIND_NULLABLE && type_equal(from->u.nullable.base, type_Void))
            return true;
        if (type_equal(from, to->u.nullable.base))
            return true;
    }
    return false;
}

/* Predicates */
bool type_is_numeric(const Type* t) {
    if (!t) return false;
    if (t->kind == TYPE_KIND_BASIC) {
        BasicTypeKind b = t->u.basic;
        return (b >= TYPE_BASIC_INT8 && b <= TYPE_BASIC_INT256) ||
               (b >= TYPE_BASIC_UINT8 && b <= TYPE_BASIC_UINT256) ||
               (b >= TYPE_BASIC_REAL32 && b <= TYPE_BASIC_REAL64);
    }
    return false;
}

bool type_is_integer(const Type* t) {
    if (!t || t->kind != TYPE_KIND_BASIC) return false;
    BasicTypeKind b = t->u.basic;
    return (b >= TYPE_BASIC_INT8 && b <= TYPE_BASIC_INT256) ||
           (b >= TYPE_BASIC_UINT8 && b <= TYPE_BASIC_UINT256);
}

bool type_is_real(const Type* t) {
    if (!t || t->kind != TYPE_KIND_BASIC) return false;
    BasicTypeKind b = t->u.basic;
    return (b == TYPE_BASIC_REAL32 || b == TYPE_BASIC_REAL64);
}

bool type_is_family(const Type* t) {
    return t && t->kind == TYPE_KIND_FAMILY;
}

bool type_is_pointer(const Type* t) { return t && t->kind == TYPE_KIND_POINTER; }
bool type_is_array(const Type* t) { return t && t->kind == TYPE_KIND_ARRAY; }
bool type_is_struct(const Type* t) { return t && t->kind == TYPE_KIND_STRUCT; }
bool type_is_union(const Type* t) { return t && t->kind == TYPE_KIND_UNION; }
bool type_is_enum(const Type* t) { return t && t->kind == TYPE_KIND_ENUM; }
bool type_is_float(const Type* t) { return type_is_real(t); }
bool type_is_signed_integer(const Type* t) {
    if (!t || t->kind != TYPE_KIND_BASIC) return false;
    BasicTypeKind b = t->u.basic;
    return (b >= TYPE_BASIC_INT8 && b <= TYPE_BASIC_INT256);
}
bool type_is_unsigned_integer(const Type* t) {
    if (!t || t->kind != TYPE_KIND_BASIC) return false;
    BasicTypeKind b = t->u.basic;
    return (b >= TYPE_BASIC_UINT8 && b <= TYPE_BASIC_UINT256);
}

/* Convert family to concrete type */
Type* type_family_to_concrete(FamilyTypeKind family) {
    switch (family) {
        case TYPE_FAMILY_INT:   return type_Int32;
        case TYPE_FAMILY_UINT:  return type_basic(TYPE_BASIC_UINT32);
        case TYPE_FAMILY_REAL:  return type_basic(TYPE_BASIC_REAL64);
        case TYPE_FAMILY_CHAR:  return type_basic(TYPE_BASIC_UTF8);
        default: return NULL;
    }
}

/*
 * Convert a type to a human-readable string for diagnostics.
 * Returns a pointer to one of several rotating static buffers, so a
 * single diagnostic may embed multiple type strings; the pointer is
 * invalidated by later calls and must not be freed.
 */
const char* type_to_string(const Type* t) {
    enum { NUM_BUFS = 4, BUF_SIZE = 256 };
    static char bufs[NUM_BUFS][BUF_SIZE];
    static unsigned next_buf = 0;
    char* buf = bufs[next_buf];
    next_buf = (next_buf + 1) % NUM_BUFS;
    if (!t) return "void";
    switch (t->kind) {
        case TYPE_KIND_BASIC:
            switch (t->u.basic) {
                case TYPE_BASIC_INT8: return "Int8";
                case TYPE_BASIC_INT16: return "Int16";
                case TYPE_BASIC_INT32: return "Int32";
                case TYPE_BASIC_INT64: return "Int64";
                case TYPE_BASIC_INT128: return "Int128";
                case TYPE_BASIC_INT256: return "Int256";
                case TYPE_BASIC_UINT8: return "UInt8";
                case TYPE_BASIC_UINT16: return "UInt16";
                case TYPE_BASIC_UINT32: return "UInt32";
                case TYPE_BASIC_UINT64: return "UInt64";
                case TYPE_BASIC_UINT128: return "UInt128";
                case TYPE_BASIC_UINT256: return "UInt256";
                case TYPE_BASIC_REAL32: return "Real32";
                case TYPE_BASIC_REAL64: return "Real64";
                case TYPE_BASIC_UTF8: return "UTF8";
                case TYPE_BASIC_UTF16: return "UTF16";
                case TYPE_BASIC_UTF32: return "UTF32";
                case TYPE_BASIC_BOOL: return "Bool";
                case TYPE_BASIC_VOID: return "Void";
                case TYPE_BASIC_AUTO: return "Auto";
                default: return "unknown basic";
            }
        case TYPE_KIND_FAMILY:
            switch (t->u.family) {
                case TYPE_FAMILY_INT: return "Int";
                case TYPE_FAMILY_UINT: return "UInt";
                case TYPE_FAMILY_REAL: return "Real";
                case TYPE_FAMILY_CHAR: return "Char";
                default: return "unknown family";
            }
        case TYPE_KIND_POINTER:
            snprintf(buf, BUF_SIZE, "&%s", type_to_string(t->u.pointer.base));
            return buf;
        case TYPE_KIND_ARRAY:
            snprintf(buf, BUF_SIZE, "[%lld]%s",
                     (long long)t->u.array.size_value,
                     type_to_string(t->u.array.base));
            return buf;
        case TYPE_KIND_TUPLE:
            snprintf(buf, BUF_SIZE, "tuple(%zu)", t->u.tuple.count);
            return buf;
        case TYPE_KIND_NULLABLE:
            snprintf(buf, BUF_SIZE, "%s?", type_to_string(t->u.nullable.base));
            return buf;
        case TYPE_KIND_ERROR:
            snprintf(buf, BUF_SIZE, "%s!", type_to_string(t->u.error.base));
            return buf;
        case TYPE_KIND_CHAN:
            snprintf(buf, BUF_SIZE, "chan %s", type_to_string(t->u.chan.base));
            return buf;
        case TYPE_KIND_FUNCTION: return "func(...)";
        case TYPE_KIND_STRUCT: return "struct";
        case TYPE_KIND_UNION: return "union";
        case TYPE_KIND_ENUM: return "enum";
        case TYPE_KIND_IDENT:
            return t->u.ident.name;
        default:
            return "?";
    }
}

/* Compute size of a type */
int64_t type_sizeof(const Type* t) {
    if (!t) return -1;
    if (t->cached_size != -1) return t->cached_size;
    int64_t sz = -1;
    switch (t->kind) {
        case TYPE_KIND_BASIC: {
            switch (t->u.basic) {
                case TYPE_BASIC_INT8: case TYPE_BASIC_UINT8: case TYPE_BASIC_UTF8:
                case TYPE_BASIC_BOOL: sz = 1; break;
                case TYPE_BASIC_INT16: case TYPE_BASIC_UINT16: case TYPE_BASIC_UTF16: sz = 2; break;
                case TYPE_BASIC_INT32: case TYPE_BASIC_UINT32: case TYPE_BASIC_REAL32:
                case TYPE_BASIC_UTF32: sz = 4; break;
                case TYPE_BASIC_INT64: case TYPE_BASIC_UINT64: case TYPE_BASIC_REAL64: sz = 8; break;
                case TYPE_BASIC_INT128: case TYPE_BASIC_UINT128: sz = 16; break;
                case TYPE_BASIC_INT256: case TYPE_BASIC_UINT256: sz = 32; break;
                case TYPE_BASIC_VOID: sz = 0; break;
                default: sz = -1; break;
            }
            break;
        }
        case TYPE_KIND_FAMILY: {
            Type* concrete = type_family_to_concrete(t->u.family);
            if (!concrete) sz = -1;
            else { sz = type_sizeof(concrete); type_free(concrete); }
            break;
        }
        case TYPE_KIND_POINTER: sz = 8; break;
        case TYPE_KIND_ARRAY: {
            int64_t elem_size = type_sizeof(t->u.array.base);
            if (elem_size < 0) sz = -1;
            else if (t->u.array.size_value >= 0) sz = elem_size * t->u.array.size_value;
            else sz = -1;
            break;
        }
        case TYPE_KIND_TUPLE: {
            int64_t total = 0;
            for (size_t i = 0; i < t->u.tuple.count; ++i) {
                int64_t s = type_sizeof(t->u.tuple.elements[i]);
                if (s < 0) { total = -1; break; }
                total += s;
            }
            sz = total;
            break;
        }
        case TYPE_KIND_NULLABLE: sz = type_sizeof(t->u.nullable.base); break;
        case TYPE_KIND_ERROR: sz = type_sizeof(t->u.error.base); break;
        case TYPE_KIND_CHAN: sz = 8; break; /* channel values are pointer-sized handles */
        case TYPE_KIND_FUNCTION: sz = 8; break;
        case TYPE_KIND_STRUCT: {
            Symbol* sym = t->u.named;
            if (!sym) { sz = -1; break; }
            FieldInfo* f = sym->extra.fields;
            int64_t total = 0;
            while (f) {
                int64_t s = type_sizeof(f->type);
                if (s < 0) { total = -1; break; }
                total += s;
                f = f->next;
            }
            sz = total;
            break;
        }
        case TYPE_KIND_UNION: {
            Symbol* sym = t->u.named;
            if (!sym) { sz = -1; break; }
            FieldInfo* f = sym->extra.fields;
            int64_t max = 0;
            while (f) {
                int64_t s = type_sizeof(f->type);
                if (s < 0) { max = -1; break; }
                if (s > max) max = s;
                f = f->next;
            }
            sz = max;
            break;
        }
        case TYPE_KIND_ENUM: sz = 4; break;
        case TYPE_KIND_IDENT: sz = -1; break;
        default: sz = -1;
    }
    ((Type*)t)->cached_size = sz;
    return sz;
}

/* Compute alignment of a type */
int64_t type_alignof(const Type* t) {
    if (!t) return -1;
    if (t->cached_align != -1) return t->cached_align;
    int64_t al = -1;
    switch (t->kind) {
        case TYPE_KIND_BASIC: {
            int64_t sz = type_sizeof(t);
            al = (sz <= 0) ? 1 : sz;
            break;
        }
        case TYPE_KIND_FAMILY: {
            Type* concrete = type_family_to_concrete(t->u.family);
            if (!concrete) al = -1;
            else { al = type_alignof(concrete); type_free(concrete); }
            break;
        }
        case TYPE_KIND_POINTER: al = 8; break;
        case TYPE_KIND_ARRAY: {
            int64_t elem_align = type_alignof(t->u.array.base);
            if (elem_align < 0) al = -1;
            else al = elem_align;
            break;
        }
        case TYPE_KIND_TUPLE: {
            int64_t max = 0;
            for (size_t i = 0; i < t->u.tuple.count; ++i) {
                int64_t a = type_alignof(t->u.tuple.elements[i]);
                if (a < 0) { max = -1; break; }
                if (a > max) max = a;
            }
            al = max;
            break;
        }
        case TYPE_KIND_NULLABLE: al = type_alignof(t->u.nullable.base); break;
        case TYPE_KIND_ERROR: al = type_alignof(t->u.error.base); break;
        case TYPE_KIND_CHAN: al = 8; break;
        case TYPE_KIND_FUNCTION: al = 8; break;
        case TYPE_KIND_STRUCT: {
            Symbol* sym = t->u.named;
            if (!sym) { al = -1; break; }
            FieldInfo* f = sym->extra.fields;
            int64_t max = 0;
            while (f) {
                int64_t a = type_alignof(f->type);
                if (a < 0) { max = -1; break; }
                if (a > max) max = a;
                f = f->next;
            }
            al = max;
            break;
        }
        case TYPE_KIND_UNION: {
            Symbol* sym = t->u.named;
            if (!sym) { al = -1; break; }
            FieldInfo* f = sym->extra.fields;
            int64_t max = 0;
            while (f) {
                int64_t a = type_alignof(f->type);
                if (a < 0) { max = -1; break; }
                if (a > max) max = a;
                f = f->next;
            }
            al = max;
            break;
        }
        case TYPE_KIND_ENUM: al = 4; break;
        case TYPE_KIND_IDENT: al = -1; break;
        default: al = -1;
    }
    ((Type*)t)->cached_align = al;
    return al;
}

/* Resolve type from AST node */
Type* resolve_type_from_ast(SemanticContext* ctx, ASTNode* node) {
    if (!node) return NULL;
    switch (node->type) {
        case NODE_BASIC_TYPE: {
            BasicType* bt = (BasicType*)node;
            return type_basic(bt->kind);
        }
        case NODE_FAMILY_TYPE: {
            FamilyType* ft = (FamilyType*)node;
            return type_family(ft->kind);
        }
        case NODE_IDENT_TYPE: {
            IdentType* it = (IdentType*)node;
            const char* name = it->name;
            if (strcmp(name, "Int") == 0) return type_family(TYPE_FAMILY_INT);
            if (strcmp(name, "UInt") == 0) return type_family(TYPE_FAMILY_UINT);
            if (strcmp(name, "Real") == 0) return type_family(TYPE_FAMILY_REAL);
            if (strcmp(name, "Char") == 0) return type_family(TYPE_FAMILY_CHAR);
            Symbol* sym = scope_lookup(ctx->current_scope, name);
            if (!sym) {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, it->base.line, it->base.column, 1,
                                            "semantic", "Unknown type '%s'", name);
                return NULL;
            }
            if (sym->kind == SYMBOL_TYPE) {
                /* A comptime alias symbol carries the aliased type
                   directly; composite types are reconstructed by name. */
                if (sym->type) {
                    return type_copy(sym->type);
                }
                return type_named(sym);
            } else {
                errhandler__report_error_ex(ERROR_LEVEL_ERROR, it->base.line, it->base.column, 1,
                                            "semantic", "'%s' is not a type", name);
                return NULL;
            }
        }
        case NODE_POINTER_TYPE: {
            PointerType* pt = (PointerType*)node;
            Type* base = resolve_type_from_ast(ctx, pt->base_type);
            if (!base) return NULL;
            return type_pointer(base);
        }
        case NODE_ARRAY_TYPE: {
            ArrayType* at = (ArrayType*)node;
            Type* base = resolve_type_from_ast(ctx, at->base_type);
            if (!base) return NULL;
            /* An array of Char uses the family's concrete character type */
            if (base->kind == TYPE_KIND_FAMILY && base->u.family == TYPE_FAMILY_CHAR) {
                Type* concrete = type_family_to_concrete(base->u.family);
                type_free(base);
                base = concrete;
            }
            int64_t size_val = -1;
            if (at->size) {
                if (!eval_const_expr(ctx, at->size, &size_val)) {
                    errhandler__report_error_ex(ERROR_LEVEL_ERROR, at->size->line, at->size->column, 1,
                                                "semantic", "Array size must be a constant integer");
                    type_free(base);
                    return NULL;
                }
            }
            return type_array(base, size_val);
        }
        case NODE_TUPLE_TYPE: {
            TupleType* tt = (TupleType*)node;
            Type** elems = (Type**)calloc(tt->count, sizeof(Type*));
            if (!elems) return NULL;
            for (size_t i = 0; i < tt->count; ++i) {
                elems[i] = resolve_type_from_ast(ctx, tt->elements[i]);
                if (!elems[i]) {
                    for (size_t j = 0; j < i; ++j) type_free(elems[j]);
                    free(elems);
                    return NULL;
                }
            }
            return type_tuple(elems, tt->count);
        }
        case NODE_NULLABLE_TYPE: {
            NullableType* nt = (NullableType*)node;
            Type* base = resolve_type_from_ast(ctx, nt->base_type);
            if (!base) return NULL;
            return type_nullable(base);
        }
        case NODE_ERROR_TYPE: {
            ErrorType* et = (ErrorType*)node;
            Type* base = resolve_type_from_ast(ctx, et->base_type);
            if (!base) return NULL;
            return type_error(base);
        }
        case NODE_CHAN_TYPE: {
            ChanType* ct = (ChanType*)node;
            Type* base = resolve_type_from_ast(ctx, ct->element);
            if (!base) return NULL;
            return type_chan(base);
        }
        case NODE_FUNC_TYPE: {
            FuncType* ft = (FuncType*)node;
            Type** params = (Type**)calloc(ft->param_count, sizeof(Type*));
            if (!params) return NULL;
            for (size_t i = 0; i < ft->param_count; ++i) {
                params[i] = resolve_type_from_ast(ctx, ft->param_types[i]);
                if (!params[i]) {
                    for (size_t j = 0; j < i; ++j) type_free(params[j]);
                    free(params);
                    return NULL;
                }
            }
            Type* ret = resolve_type_from_ast(ctx, ft->return_type);
            if (!ret) {
                for (size_t j = 0; j < ft->param_count; ++j) type_free(params[j]);
                free(params);
                return NULL;
            }
            return type_function(params, ft->param_count, ret, false);
        }
        case NODE_STRUCT_TYPE:
        case NODE_UNION_TYPE:
        case NODE_ENUM_TYPE: {
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, node->line, node->column, 1,
                                        "semantic", "Inline type definitions not allowed here");
            return NULL;
        }
        default:
            errhandler__report_error_ex(ERROR_LEVEL_ERROR, node->line, node->column, 1,
                                        "semantic", "Invalid type node");
            return NULL;
    }
}
