#include "internal.h"
#include <stdlib.h>
#include <string.h>

/*
 * Parsers for composite type definitions in spec syntax:
 *   comptime Point = Struct { x: Real64, y: Real64, };
 *   comptime Value = Union { as_int: Int32, as_real: Real32, };
 *   comptime Color = Enum { Red, Green, Ok = 0, };
 * Entries are comma-separated inside braces; a trailing comma is allowed.
 * Array fields use the ordinary postfix form (arr: Int32[8]).
 * Methods are declared at top level with qualified names, never inside
 * the type body, and field defaults do not exist.
 */

/*
 * Parse the field list shared by Struct and Union: comma-separated
 * 'name: Type' entries inside braces. On success returns true and fills
 * *fields_out / *count_out with arena-allocated Field nodes.
 * Returns false after reporting an error and sets panic mode.
 */
static bool parse_field_list(Parser* p, ASTNode*** fields_out, size_t* count_out) {
    if (!match(p, TOKEN_LBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '{' to open composite type");
        p->panic_mode = true;
        return false;
    }
    while (peek(p) != TOKEN_RBRACE && peek(p) != TOKEN_EOF) {
        if (!match(p, TOKEN_IDENTIFIER)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected field name");
            p->panic_mode = true;
            return false;
        }
        char* fname = strdup_custom(p->tokens[p->current - 1].value);
        if (!match(p, TOKEN_COLON)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected ':' after field name");
            free(fname);
            p->panic_mode = true;
            return false;
        }
        ASTNode* ftype = parse_type_spec(p);
        if (!ftype) {
            free(fname);
            return false;
        }
        Field* f = (Field*)make_node(p, NODE_FIELD, sizeof(Field));
        if (!f) {
            free(fname);
            free_ast_node(ftype);
            return false;
        }
        f->name = fname;
        f->type = ftype;
        append_child(p, (void***)fields_out, count_out, (ASTNode*)f);
        if (peek(p) == TOKEN_RBRACE) {
            break;
        }
        if (!match(p, TOKEN_COMMA)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected ',' or '}' in field list");
            p->panic_mode = true;
            return false;
        }
    }
    if (!match(p, TOKEN_RBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '}' to close composite type");
        p->panic_mode = true;
        return false;
    }
    return true;
}

/*
 * Parse a Struct definition: Struct { field: Type, ... }.
 * Consumes the 'Struct' keyword. Returns a NODE_STRUCT_TYPE node or NULL.
 */
ASTNode* parse_struct_def(Parser* p) {
    advance(p);
    StructType* st = (StructType*)make_node(p, NODE_STRUCT_TYPE, sizeof(StructType));
    if (!st) {
        return NULL;
    }
    st->fields = NULL;
    st->field_count = 0;
    st->methods = NULL;
    st->method_count = 0;
    if (!parse_field_list(p, &st->fields, &st->field_count)) {
        return NULL;
    }
    return (ASTNode*)st;
}

/*
 * Parse a Union definition: Union { field: Type, ... }.
 * Consumes the 'Union' keyword. Returns a NODE_UNION_TYPE node or NULL.
 */
ASTNode* parse_union_def(Parser* p) {
    advance(p);
    UnionType* ut = (UnionType*)make_node(p, NODE_UNION_TYPE, sizeof(UnionType));
    if (!ut) {
        return NULL;
    }
    ut->fields = NULL;
    ut->field_count = 0;
    if (!parse_field_list(p, &ut->fields, &ut->field_count)) {
        return NULL;
    }
    return (ASTNode*)ut;
}

/*
 * Parse an Enum definition: Enum { Name, Name = value, ... }.
 * Explicit values are constant expressions evaluated during semantic
 * analysis. Consumes the 'Enum' keyword. Returns a NODE_ENUM_TYPE node
 * or NULL.
 */
ASTNode* parse_enum_def(Parser* p) {
    advance(p);
    EnumType* et = (EnumType*)make_node(p, NODE_ENUM_TYPE, sizeof(EnumType));
    if (!et) {
        return NULL;
    }
    et->constants = NULL;
    et->const_count = 0;
    if (!match(p, TOKEN_LBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '{' after Enum");
        p->panic_mode = true;
        return NULL;
    }
    while (peek(p) != TOKEN_RBRACE && peek(p) != TOKEN_EOF) {
        if (!match(p, TOKEN_IDENTIFIER)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected constant name in enum");
            p->panic_mode = true;
            return NULL;
        }
        char* cname = strdup_custom(p->tokens[p->current - 1].value);
        ASTNode* cval = NULL;
        if (match(p, TOKEN_EQUAL)) {
            cval = parse_assignment_expr(p);
            if (!cval) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected constant value");
                free(cname);
                p->panic_mode = true;
                return NULL;
            }
        }
        EnumConst* ec = (EnumConst*)make_node(p, NODE_ENUM_CONST, sizeof(EnumConst));
        if (!ec) {
            free(cname);
            free_ast_node(cval);
            return NULL;
        }
        ec->name = cname;
        ec->value = cval;
        append_child(p, (void***)&et->constants, &et->const_count, (ASTNode*)ec);
        if (peek(p) == TOKEN_RBRACE) {
            break;
        }
        if (!match(p, TOKEN_COMMA)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected ',' or '}' in enum");
            p->panic_mode = true;
            return NULL;
        }
    }
    if (!match(p, TOKEN_RBRACE)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '}' to close enum");
        p->panic_mode = true;
        return NULL;
    }
    return (ASTNode*)et;
}
