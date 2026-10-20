#include "internal.h"
#include <stdlib.h>
#include <string.h>

/*
 * Helper: checks if a token type is a built-in type keyword.
 */
static bool is_basic_type_token(TokenType type) {
    switch (type) {
        case TOKEN_INT:
        case TOKEN_UINT:
        case TOKEN_REAL:
        case TOKEN_CHAR:
        case TOKEN_INT8:
        case TOKEN_INT16:
        case TOKEN_INT32:
        case TOKEN_INT64:
        case TOKEN_INT128:
        case TOKEN_INT256:
        case TOKEN_UINT8:
        case TOKEN_UINT16:
        case TOKEN_UINT32:
        case TOKEN_UINT64:
        case TOKEN_UINT128:
        case TOKEN_UINT256:
        case TOKEN_REAL32:
        case TOKEN_REAL64:
        case TOKEN_UTF8:
        case TOKEN_UTF16:
        case TOKEN_UTF32:
        case TOKEN_BOOL:
        case TOKEN_VOID:
        case TOKEN_AUTO:
            return true;
        default:
            return false;
    }
}

/*
 * Returns true when the token can begin a type specification:
 * a built-in type keyword, a composite keyword, or '&'.
 */
bool token_is_type_start(TokenType type) {
    return is_basic_type_token(type) || type == TOKEN_AMPERSAND ||
           type == TOKEN_STRUCT || type == TOKEN_UNION || type == TOKEN_ENUM;
}

/*
 * Returns true when the token is a built-in type keyword (sized
 * integers, reals, UTF code units, Bool, Void, Auto, families).
 */
bool token_is_builtin_type(TokenType type) {
    return is_basic_type_token(type);
}

/*
 * Parses a type specification. Handles prefix '&', base type,
 * then postfix operators [expr], ?, !.
 */
ASTNode* parse_type_spec(Parser* p) {
    // Pointer: & Type
    if (match(p, TOKEN_AMPERSAND)) {
        ASTNode* base = parse_type_spec(p);
        if (!base) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected type after '&'");
            p->panic_mode = true;
            return NULL;
        }
        PointerType* pt = (PointerType*)make_node(p, NODE_POINTER_TYPE, sizeof(PointerType));
        if (!pt) {
            free_ast_node(base);
            return NULL;
        }
        pt->base_type = base;
        return (ASTNode*)pt;
    }

    // Base type
    ASTNode* base_node = NULL;
    TokenType type = peek(p);

    if (type == TOKEN_LPAREN) {
        /* Tuple type: ( Type, ... ). A single type without a comma
           is a grouped type and denotes the inner type itself. */
        advance(p);
        ASTNode* first = parse_type_spec(p);
        if (!first) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected type in parentheses");
            p->panic_mode = true;
            return NULL;
        }
        if (peek(p) != TOKEN_COMMA) {
            if (!match(p, TOKEN_RPAREN)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected ')' after grouped type");
                p->panic_mode = true;
                free_ast_node(first);
                return NULL;
            }
            base_node = first;
        } else {
            TupleType* tt = (TupleType*)make_node(p, NODE_TUPLE_TYPE, sizeof(TupleType));
            if (!tt) {
                free_ast_node(first);
                return NULL;
            }
            tt->elements = NULL;
            tt->count = 0;
            append_child(p, (void***)&tt->elements, &tt->count, first);
            while (match(p, TOKEN_COMMA) && peek(p) != TOKEN_RPAREN) {
                ASTNode* elem = parse_type_spec(p);
                if (!elem) {
                    errhandler__report_error(p->line, p->column, "syntax",
                                             "Expected type in tuple");
                    p->panic_mode = true;
                    break;
                }
                append_child(p, (void***)&tt->elements, &tt->count, elem);
            }
            if (!match(p, TOKEN_RPAREN)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected ')' to close tuple type");
                p->panic_mode = true;
            }
            base_node = (ASTNode*)tt;
        }
    } else if (type == TOKEN_FUNC) {
        // Function type: func ( Type , ... ) : Type
        advance(p);
        if (!match(p, TOKEN_LPAREN)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected '(' in function type");
            p->panic_mode = true;
            return NULL;
        }
        FuncType* ft = (FuncType*)make_node(p, NODE_FUNC_TYPE, sizeof(FuncType));
        if (!ft) {
            return NULL;
        }
        ft->param_types = NULL;
        ft->param_count = 0;

        if (peek(p) != TOKEN_RPAREN) {
            do {
                ASTNode* param = parse_type_spec(p);
                if (!param) {
                    errhandler__report_error(p->line, p->column, "syntax",
                                             "Expected parameter type");
                    p->panic_mode = true;
                    break;
                }
                append_child(p, (void***)&ft->param_types, &ft->param_count, param);
            } while (match(p, TOKEN_COMMA) && peek(p) != TOKEN_RPAREN);
        }
        if (!match(p, TOKEN_RPAREN)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected ')' after function parameters");
            p->panic_mode = true;
        }
        if (!match(p, TOKEN_COLON)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected ':' in function return type");
            p->panic_mode = true;
        }
        ft->return_type = parse_type_spec(p);
        if (!ft->return_type) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected return type");
            p->panic_mode = true;
        }
        base_node = (ASTNode*)ft;
    } else if (type == TOKEN_IDENTIFIER || is_basic_type_token(type)) {
        Token* tok = advance(p);
        bool is_family = false;
        FamilyTypeKind fkind;

        switch (tok->type) {
            case TOKEN_INT:   fkind = TYPE_FAMILY_INT;   is_family = true; break;
            case TOKEN_UINT:  fkind = TYPE_FAMILY_UINT;  is_family = true; break;
            case TOKEN_REAL:  fkind = TYPE_FAMILY_REAL;  is_family = true; break;
            case TOKEN_CHAR:  fkind = TYPE_FAMILY_CHAR;  is_family = true; break;
            default: break;
        }
        if (is_family) {
            FamilyType* ft = (FamilyType*)make_node(p, NODE_FAMILY_TYPE, sizeof(FamilyType));
            if (!ft) {
                return NULL;
            }
            ft->kind = fkind;
            base_node = (ASTNode*)ft;
        } else {
            BasicTypeKind kind;
            bool is_basic = true;
            switch (tok->type) {
                case TOKEN_INT8:   kind = TYPE_BASIC_INT8; break;
                case TOKEN_INT16:  kind = TYPE_BASIC_INT16; break;
                case TOKEN_INT32:  kind = TYPE_BASIC_INT32; break;
                case TOKEN_INT64:  kind = TYPE_BASIC_INT64; break;
                case TOKEN_INT128: kind = TYPE_BASIC_INT128; break;
                case TOKEN_INT256: kind = TYPE_BASIC_INT256; break;
                case TOKEN_UINT8:  kind = TYPE_BASIC_UINT8; break;
                case TOKEN_UINT16: kind = TYPE_BASIC_UINT16; break;
                case TOKEN_UINT32: kind = TYPE_BASIC_UINT32; break;
                case TOKEN_UINT64: kind = TYPE_BASIC_UINT64; break;
                case TOKEN_UINT128: kind = TYPE_BASIC_UINT128; break;
                case TOKEN_UINT256: kind = TYPE_BASIC_UINT256; break;
                case TOKEN_REAL32: kind = TYPE_BASIC_REAL32; break;
                case TOKEN_REAL64: kind = TYPE_BASIC_REAL64; break;
                case TOKEN_UTF8:   kind = TYPE_BASIC_UTF8; break;
                case TOKEN_UTF16:  kind = TYPE_BASIC_UTF16; break;
                case TOKEN_UTF32:  kind = TYPE_BASIC_UTF32; break;
                case TOKEN_BOOL:   kind = TYPE_BASIC_BOOL; break;
                case TOKEN_VOID:   kind = TYPE_BASIC_VOID; break;
                case TOKEN_AUTO:   kind = TYPE_BASIC_AUTO; break;
                default:
                    is_basic = false;
                    break;
            }
            if (is_basic) {
                BasicType* bt = (BasicType*)make_node(p, NODE_BASIC_TYPE, sizeof(BasicType));
                if (!bt) {
                    return NULL;
                }
                bt->kind = kind;
                base_node = (ASTNode*)bt;
            } else {
                IdentType* it = (IdentType*)make_node(p, NODE_IDENT_TYPE, sizeof(IdentType));
                if (!it) {
                    return NULL;
                }
                it->name = strdup_custom(tok->value);
                base_node = (ASTNode*)it;
            }
        }
    } else if (type == TOKEN_STRUCT) {
        base_node = parse_struct_def(p);
    } else if (type == TOKEN_UNION) {
        base_node = parse_union_def(p);
    } else if (type == TOKEN_ENUM) {
        base_node = parse_enum_def(p);
    } else if (type == TOKEN_CHAN) {
        /* Channel type: chan T */
        advance(p);
        ASTNode* elem = parse_type_spec(p);
        if (!elem) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected element type after 'chan'");
            p->panic_mode = true;
            return NULL;
        }
        ChanType* ct = (ChanType*)make_node(p, NODE_CHAN_TYPE, sizeof(ChanType));
        if (!ct) {
            free_ast_node(elem);
            return NULL;
        }
        ct->element = elem;
        base_node = (ASTNode*)ct;
    } else {
        errhandler__report_error(p->line, p->column, "syntax", "Expected type");
        p->panic_mode = true;
        return NULL;
    }

    if (!base_node) {
        return NULL;
    }

    // Postfix operators: array, nullable, error.
    while (1) {
        if (match(p, TOKEN_LBRACKET)) {
            ASTNode* size = NULL;
            if (peek(p) != TOKEN_RBRACKET) {
                size = parse_assignment_expr(p);
                if (!size) {
                    errhandler__report_error(p->line, p->column, "syntax",
                                             "Expected array size expression");
                    p->panic_mode = true;
                    break;
                }
            }
            if (!match(p, TOKEN_RBRACKET)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected ']' after array size");
                p->panic_mode = true;
                free_ast_node(size);
                break;
            }
            ArrayType* at = (ArrayType*)make_node(p, NODE_ARRAY_TYPE, sizeof(ArrayType));
            if (!at) {
                free_ast_node(base_node);
                free_ast_node(size);
                return NULL;
            }
            at->base_type = base_node;
            at->size = size;
            base_node = (ASTNode*)at;
        } else if (match(p, TOKEN_QUESTION)) {
            NullableType* nt = (NullableType*)make_node(p, NODE_NULLABLE_TYPE, sizeof(NullableType));
            if (!nt) {
                free_ast_node(base_node);
                return NULL;
            }
            nt->base_type = base_node;
            base_node = (ASTNode*)nt;
        } else if (match(p, TOKEN_BANG)) {
            ErrorType* et = (ErrorType*)make_node(p, NODE_ERROR_TYPE, sizeof(ErrorType));
            if (!et) {
                free_ast_node(base_node);
                return NULL;
            }
            et->base_type = base_node;
            base_node = (ASTNode*)et;
        } else {
            break;
        }
    }
    return base_node;
}
