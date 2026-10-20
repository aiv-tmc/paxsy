#include "internal.h"
#include <stdlib.h>
#include <string.h>

/*
 *  AST node allocation (arena-based)
*/

void* make_node(Parser* p, NodeType type, size_t size) {
    void* node = parser_alloc_raw(p, size);
    if (!node) return NULL;
    ASTNode* base = (ASTNode*)node;
    base->type = type;
    base->line = p->line;
    base->column = p->column;
    return node;
}

void* parser_alloc_raw(Parser* p, size_t size) {
    /* Keep every allocation aligned so arena-stored nodes stay well-formed */
    size = (size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    ParserArenaBlock* block = p->arena;
    if (!block || block->used + size > block->capacity) {
        /* Allocate a fresh block instead of reallocating: pointers into
         * previously allocated arena memory must remain valid forever */
        size_t cap = 4096;
        if (size > cap) cap = size;
        ParserArenaBlock* nb =
            (ParserArenaBlock*)malloc(sizeof(ParserArenaBlock) + cap);
        if (!nb) return NULL;
        nb->next = p->arena;
        nb->capacity = cap;
        nb->used = 0;
        p->arena = nb;
        block = nb;
    }
    void* ptr = block->data + block->used;
    block->used += size;
    memset(ptr, 0, size);
    return ptr;
}

void parser_free_arena(ParserArenaBlock* blocks) {
    while (blocks) {
        ParserArenaBlock* next = blocks->next;
        free(blocks);
        blocks = next;
    }
}

char* arena_strdup(Parser* p, const char* s, size_t len) {
    if (!s) return NULL;
    if (len == 0) len = strlen(s);
    char* dest = (char*)parser_alloc_raw(p, len + 1);
    if (!dest) return NULL;
    memcpy(dest, s, len);
    dest[len] = '\0';
    return dest;
}

/*
 *  Dynamic array helper (for child pointers)
*/

void append_child(Parser* p, void*** array, size_t* count, void* child) {
    size_t new_count = *count + 1;
    size_t new_size = new_count * sizeof(void*);
    void** new_array = (void**)parser_alloc_raw(p, new_size);
    if (!new_array) return;
    if (*array && *count > 0) {
        memcpy(new_array, *array, (*count) * sizeof(void*));
    }
    new_array[*count] = child;
    *array = new_array;
    *count = new_count;
}

/*
 *  String duplication (malloc-based, for non‑arena strings)
*/

char* strdup_custom(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* dup = (char*)malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

/*
 *  Recursive AST freeing (frees malloc’d strings, but not arena nodes)
*/

static void free_string(char** str) {
    if (str && *str) { free(*str); *str = NULL; }
}

void free_ast_node(ASTNode* node) {
    if (!node) return;

    switch (node->type) {
        case NODE_PROGRAM: {
            Program* n = (Program*)node;
            for (size_t i = 0; i < n->item_count; ++i)
                free_ast_node(n->items[i]);
            break;
        }
        case NODE_MODULE: {
            ModuleDecl* n = (ModuleDecl*)node;
            free_string(&n->name);
            for (size_t i = 0; i < n->body_count; ++i)
                free_ast_node(n->body[i]);
            break;
        }
        case NODE_USE: {
            UseDecl* n = (UseDecl*)node;
            for (size_t i = 0; i < n->path_len; ++i)
                free_string(&n->path[i]);
            break;
        }
        case NODE_FUNC_DECL: {
            FuncDecl* n = (FuncDecl*)node;
            free_string(&n->name);
            for (size_t i = 0; i < n->method_of_count; ++i)
                free(n->method_of[i]);
            for (size_t i = 0; i < n->param_count; ++i)
                free_ast_node(n->params[i]);
            free_ast_node(n->return_type);
            free_ast_node(n->body);
            free_ast_node(n->expr_body);
            break;
        }
        case NODE_LET_DECL: {
            LetDecl* n = (LetDecl*)node;
            for (size_t i = 0; i < n->count; ++i) {
                free_string(&n->names[i]);
                free_ast_node(n->types[i]);
            }
            free_ast_node(n->initializer);
            free_ast_node(n->array_size);
            for (size_t i = 0; i < n->init_count; ++i) {
                free_ast_node(n->inits[i]);
            }
            break;
        }
        case NODE_COMPTIME_DECL: {
            ComptimeDecl* n = (ComptimeDecl*)node;
            free_string(&n->name);
            free_ast_node(n->type_def);
            free_ast_node(n->value);
            break;
        }
        case NODE_EXTERN_DECL: {
            ExternDecl* n = (ExternDecl*)node;
            free_string(&n->name);
            free_ast_node(n->type);
            for (size_t i = 0; i < n->param_count; ++i)
                free_ast_node(n->params[i]);
            break;
        }
        case NODE_EXTERN_BLOCK: {
            ExternBlock* n = (ExternBlock*)node;
            for (size_t i = 0; i < n->item_count; ++i)
                free_ast_node(n->items[i]);
            break;
        }
        case NODE_BLOCK: {
            Block* n = (Block*)node;
            for (size_t i = 0; i < n->stmt_count; ++i)
                free_ast_node(n->statements[i]);
            break;
        }
        case NODE_IF_STMT: {
            IfStmt* n = (IfStmt*)node;
            free_ast_node(n->condition);
            free_ast_node(n->then_branch);
            for (size_t i = 0; i < n->else_if_count; ++i) {
                free_ast_node(n->else_if_conditions[i]);
                free_ast_node(n->else_if_branches[i]);
            }
            free_ast_node(n->else_branch);
            break;
        }
        case NODE_IF_EXISTS: {
            IfExistsStmt* n = (IfExistsStmt*)node;
            free_string(&n->name);
            free_ast_node(n->then_branch);
            free_ast_node(n->else_branch);
            break;
        }
        case NODE_MATCH_STMT: {
            MatchStmt* n = (MatchStmt*)node;
            free_ast_node(n->expr);
            for (size_t i = 0; i < n->case_count; ++i)
                free_ast_node(n->cases[i]);
            free_ast_node(n->else_case);
            break;
        }
        case NODE_MATCH_CASE: {
            MatchCase* n = (MatchCase*)node;
            for (size_t i = 0; i < n->pattern_count; ++i)
                free_ast_node(n->patterns[i]);
            free_ast_node(n->body);
            break;
        }
        case NODE_MATCH_EXPR: {
            MatchExpr* n = (MatchExpr*)node;
            free_ast_node(n->subject);
            for (size_t i = 0; i < n->case_count; ++i)
                free_ast_node(n->cases[i]);
            free_ast_node(n->else_value);
            break;
        }
        case NODE_MATCH_EXPR_CASE: {
            MatchExprCase* n = (MatchExprCase*)node;
            for (size_t i = 0; i < n->pattern_count; ++i)
                free_ast_node(n->patterns[i]);
            free_ast_node(n->value);
            break;
        }
        case NODE_WHILE_STMT: {
            WhileStmt* n = (WhileStmt*)node;
            free_string(&n->label);
            free_ast_node(n->condition);
            free_ast_node(n->body);
            break;
        }
        case NODE_DO_WHILE_STMT: {
            DoWhileStmt* n = (DoWhileStmt*)node;
            free_string(&n->label);
            free_ast_node(n->body);
            free_ast_node(n->condition);
            break;
        }
        case NODE_FOR_STMT: {
            ForStmt* n = (ForStmt*)node;
            free_string(&n->label);
            free_string(&n->var_name);
            free_ast_node(n->var_type);
            free_ast_node(n->range);
            free_ast_node(n->body);
            break;
        }
        case NODE_BREAK_STMT:
        case NODE_CONTINUE_STMT: {
            if (node->type == NODE_BREAK_STMT) {
                BreakStmt* n = (BreakStmt*)node;
                free_string(&n->label);
            } else {
                ContinueStmt* n = (ContinueStmt*)node;
                free_string(&n->label);
            }
            break;
        }
        case NODE_RETURN_STMT: {
            ReturnStmt* n = (ReturnStmt*)node;
            free_ast_node(n->value);
            break;
        }
        case NODE_DEFER_STMT: {
            DeferStmt* n = (DeferStmt*)node;
            free_ast_node(n->stmt);
            break;
        }
        case NODE_ASM_STMT: {
            AsmStmt* n = (AsmStmt*)node;
            free_ast_node(n->code);
            break;
        }
        case NODE_VOLATILE_STMT: {
            VolatileStmt* n = (VolatileStmt*)node;
            free_ast_node(n->stmt);
            break;
        }
        case NODE_COMPTIME_STMT: {
            ComptimeStmt* n = (ComptimeStmt*)node;
            free_string(&n->name);
            free_ast_node(n->stmt);
            break;
        }
        case NODE_PASS_STMT:
            break;
        case NODE_EXPR_STMT: {
            ExprStmt* n = (ExprStmt*)node;
            free_ast_node(n->expr);
            break;
        }
        case NODE_IDENT: {
            IdentExpr* n = (IdentExpr*)node;
            free_string(&n->name);
            break;
        }
        case NODE_LITERAL: {
            LiteralExpr* n = (LiteralExpr*)node;
            free_string(&n->raw_value);
            free_string(&n->string_val);
            break;
        }
        case NODE_UNARY: {
            UnaryExpr* n = (UnaryExpr*)node;
            free_ast_node(n->operand);
            break;
        }
        case NODE_BINARY: {
            BinaryExpr* n = (BinaryExpr*)node;
            free_ast_node(n->left);
            free_ast_node(n->right);
            break;
        }
        case NODE_ASSIGN: {
            AssignExpr* n = (AssignExpr*)node;
            free_ast_node(n->left);
            free_ast_node(n->right);
            break;
        }
        case NODE_SEND: {
            SendExpr* n = (SendExpr*)node;
            free_ast_node(n->target);
            free_ast_node(n->message);
            break;
        }
        case NODE_CALL: {
            CallExpr* n = (CallExpr*)node;
            free_ast_node(n->callee);
            for (size_t i = 0; i < n->arg_count; ++i)
                free_ast_node(n->args[i]);
            break;
        }
        case NODE_MEMBER_ACCESS: {
            MemberAccess* n = (MemberAccess*)node;
            free_ast_node(n->object);
            free_string(&n->member);
            break;
        }
        case NODE_MODULE_ACCESS: {
            ModuleAccess* n = (ModuleAccess*)node;
            for (size_t i = 0; i < n->path_len; ++i)
                free_string(&n->path[i]);
            free_string(&n->symbol);
            break;
        }
        case NODE_INDEX: {
            IndexExpr* n = (IndexExpr*)node;
            free_ast_node(n->array);
            free_ast_node(n->index);
            break;
        }
        case NODE_SLICE: {
            SliceExpr* n = (SliceExpr*)node;
            free_ast_node(n->array);
            free_ast_node(n->range);
            break;
        }
        case NODE_STRUCT_INIT: {
            StructInitExpr* n = (StructInitExpr*)node;
            free_ast_node(n->struct_type);
            for (size_t i = 0; i < n->count; ++i) {
                if (n->named) free_string(&n->field_names[i]);
                free_ast_node(n->values[i]);
            }
            break;
        }
        case NODE_RANGE: {
            RangeExpr* n = (RangeExpr*)node;
            free_ast_node(n->start);
            free_ast_node(n->end);
            break;
        }
        case NODE_IF_EXPR: {
            IfExpr* n = (IfExpr*)node;
            free_ast_node(n->condition);
            free_ast_node(n->then_expr);
            free_ast_node(n->else_expr);
            break;
        }
        case NODE_TRY_EXPR: {
            TryExpr* n = (TryExpr*)node;
            free_ast_node(n->expr);
            break;
        }
        case NODE_CATCH_EXPR: {
            CatchExpr* n = (CatchExpr*)node;
            free_ast_node(n->expr);
            free_string(&n->var_name);
            free_ast_node(n->handler);
            break;
        }
        case NODE_ERROR_EXPR: {
            ErrorExpr* n = (ErrorExpr*)node;
            free_ast_node(n->code);
            break;
        }
        case NODE_INTERPOLATED_STRING: {
            InterpolatedString* n = (InterpolatedString*)node;
            for (size_t i = 0; i < n->segment_count; ++i) {
                InterpolatedSegment* seg = n->segments[i];
                /* seg->text is arena-owned; only expression trees need freeing */
                if (seg->kind != SEG_TEXT)
                    free_ast_node(seg->expr);
            }
            break;
        }
        case NODE_CAST: {
            CastExpr* n = (CastExpr*)node;
            free_ast_node(n->expr);
            free_ast_node(n->target_type);
            break;
        }
        case NODE_ARRAY_LITERAL: {
            ArrayLiteral* n = (ArrayLiteral*)node;
            for (size_t i = 0; i < n->count; ++i)
                free_ast_node(n->elements[i]);
            break;
        }
        case NODE_ALLOC_EXPR: {
            AllocExpr* n = (AllocExpr*)node;
            free_ast_node(n->type);
            free_ast_node(n->count);
            break;
        }
        case NODE_FREE_STMT: {
            FreeStmt* n = (FreeStmt*)node;
            free_ast_node(n->expr);
            break;
        }
        /* Type nodes */
        case NODE_FAMILY_TYPE:
        case NODE_BASIC_TYPE:
            break;
        case NODE_POINTER_TYPE: {
            PointerType* n = (PointerType*)node;
            free_ast_node(n->base_type);
            break;
        }
        case NODE_ARRAY_TYPE: {
            ArrayType* n = (ArrayType*)node;
            free_ast_node(n->base_type);
            free_ast_node(n->size);
            break;
        }
        case NODE_TUPLE_TYPE: {
            TupleType* n = (TupleType*)node;
            for (size_t i = 0; i < n->count; ++i)
                free_ast_node(n->elements[i]);
            break;
        }
        case NODE_NULLABLE_TYPE: {
            NullableType* n = (NullableType*)node;
            free_ast_node(n->base_type);
            break;
        }
        case NODE_ERROR_TYPE: {
            ErrorType* n = (ErrorType*)node;
            free_ast_node(n->base_type);
            break;
        }
        case NODE_CHAN_TYPE: {
            ChanType* n = (ChanType*)node;
            free_ast_node(n->element);
            break;
        }
        case NODE_FUNC_TYPE: {
            FuncType* n = (FuncType*)node;
            for (size_t i = 0; i < n->param_count; ++i)
                free_ast_node(n->param_types[i]);
            free_ast_node(n->return_type);
            break;
        }
        case NODE_IDENT_TYPE: {
            IdentType* n = (IdentType*)node;
            free_string(&n->name);
            break;
        }
        case NODE_STRUCT_TYPE: {
            StructType* n = (StructType*)node;
            for (size_t i = 0; i < n->field_count; ++i)
                free_ast_node(n->fields[i]);
            for (size_t i = 0; i < n->method_count; ++i)
                free_ast_node(n->methods[i]);
            break;
        }
        case NODE_UNION_TYPE: {
            UnionType* n = (UnionType*)node;
            for (size_t i = 0; i < n->field_count; ++i)
                free_ast_node(n->fields[i]);
            break;
        }
        case NODE_ENUM_TYPE: {
            EnumType* n = (EnumType*)node;
            for (size_t i = 0; i < n->const_count; ++i)
                free_ast_node(n->constants[i]);
            break;
        }
        case NODE_ENUM_CONST: {
            EnumConst* n = (EnumConst*)node;
            free_string(&n->name);
            free_ast_node(n->value);
            break;
        }
        case NODE_PARAM: {
            Param* n = (Param*)node;
            free_string(&n->name);
            free_ast_node(n->type);
            break;
        }
        case NODE_FIELD: {
            Field* n = (Field*)node;
            free_string(&n->name);
            free_ast_node(n->type);
            break;
        }
        default:
            /* Unknown node – free nothing (avoid crash) */
            break;
    }
}

/*
 *  Free the whole Program AST (entry point)
*/

void parser_free_ast(Program* prog) {
    if (!prog) return;
    free_ast_node((ASTNode*)prog); /* frees malloc'd strings only */
    parser_free_arena((ParserArenaBlock*)prog->arena_blocks);
}

/*
 *  Helper: parse "name : Type" (with optional modifiers)
*/

bool parse_name_type(Parser* p, char** out_name, ASTNode** out_type, uint32_t* out_mods) {
    if (!match(p, TOKEN_IDENTIFIER)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected identifier");
        return false;
    }
    *out_name = strdup_custom(p->tokens[p->current - 1].value);
    *out_mods = 0;   // modifiers not parsed here (handled elsewhere)
    if (match(p, TOKEN_COLON)) {
        *out_type = parse_type_spec(p);
        if (!*out_type) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected type after ':'");
            return false;
        }
    } else {
        *out_type = NULL;
    }
    return true;
}

/*
 *  Helper: parse function signature: name ( params ) : return_type
*/

bool parse_function_signature(Parser* p, char** out_name, ASTNode*** out_params,
                              size_t* out_param_count, ASTNode** out_return_type,
                              uint32_t* out_mods, bool allow_mods,
                              char*** out_method_of, size_t* out_method_of_count) {
    (void)allow_mods;   // not used in current implementation

    *out_method_of = NULL;
    *out_method_of_count = 0;

    /* Method qualifier: Point.name or {Point, Vector3}.name */
    if (peek(p) == TOKEN_LBRACE) {
        advance(p);
        do {
            if (!match(p, TOKEN_IDENTIFIER)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Expected struct name in method qualifier");
                goto qualifier_error;
            }
            append_child(p, (void***)out_method_of, out_method_of_count,
                         strdup_custom(p->tokens[p->current - 1].value));
        } while (match(p, TOKEN_COMMA) && peek(p) != TOKEN_RBRACE);
        if (!match(p, TOKEN_RBRACE)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected '}' after method qualifier");
            goto qualifier_error;
        }
        if (!match(p, TOKEN_DOT)) {
            errhandler__report_error(p->line, p->column, "syntax",
                                     "Expected '.' after method qualifier");
            goto qualifier_error;
        }
    } else if (peek(p) == TOKEN_IDENTIFIER && p->current + 1 < p->token_count &&
               p->tokens[p->current + 1].type == TOKEN_DOT) {
        append_child(p, (void***)out_method_of, out_method_of_count,
                     strdup_custom(p->tokens[p->current].value));
        advance(p); /* struct name */
        advance(p); /* '.' */
    }

    // Parse function name
    if (!match(p, TOKEN_IDENTIFIER)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected function name");
        goto qualifier_error;
    }
    *out_name = strdup_custom(p->tokens[p->current - 1].value);

    // Parameters: ( param1 : Type, param2 : Type, ... )
    if (!match(p, TOKEN_LPAREN)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected '(' after function name");
        return false;
    }

    *out_params = NULL;
    *out_param_count = 0;

    if (peek(p) != TOKEN_RPAREN) {
        do {
            char* pname = NULL;
            ASTNode* ptype = NULL;
            uint32_t mods = 0;
            uint8_t is_comptime_param = 0;
            if (peek(p) == TOKEN_COMPTIME) {
                advance(p);
                is_comptime_param = 1;
            }
            if (is_comptime_param) {
                /* A comptime parameter is a bare name; its type is
                   inferred from the call-site argument. */
                if (!match(p, TOKEN_IDENTIFIER)) {
                    errhandler__report_error(p->line, p->column, "syntax",
                                             "Expected comptime parameter name");
                    return false;
                }
                pname = strdup_custom(p->tokens[p->current - 1].value);
            } else if (!parse_name_type(p, &pname, &ptype, &mods)) {
                errhandler__report_error(p->line, p->column, "syntax",
                                         "Invalid parameter");
                return false;
            }
            Param* param = (Param*)make_node(p, NODE_PARAM, sizeof(Param));
            if (!param) {
                free(pname);
                free_ast_node(ptype);
                return false;
            }
            param->name = pname;
            param->type = ptype;
            param->modifiers = mods;
            param->is_volatile = (mods & MOD_VOLATILE) ? 1 : 0;
            param->is_comptime = is_comptime_param;
            append_child(p, (void***)out_params, out_param_count, param);
        } while (match(p, TOKEN_COMMA) && peek(p) != TOKEN_RPAREN);
    }

    if (!match(p, TOKEN_RPAREN)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ')' after parameters");
        return false;
    }

    // Return type: : Type
    if (!match(p, TOKEN_COLON)) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected ':' for return type");
        return false;
    }
    *out_return_type = parse_type_spec(p);
    if (!*out_return_type) {
        errhandler__report_error(p->line, p->column, "syntax",
                                 "Expected return type");
        return false;
    }

    *out_mods = 0;   // modifiers not parsed here
    return true;

qualifier_error:
    for (size_t i = 0; i < *out_method_of_count; ++i) {
        free((*out_method_of)[i]);
    }
    return false;
}
