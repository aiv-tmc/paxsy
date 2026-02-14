#include "output.h"
#include "../errhandler/errhandler.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Static module variables */
static bool parser_trace_enabled = false;
static FILE* trace_file          = NULL;

/* Token type names array */
const char* token_names[] = {
    [TOKEN_NUMBER]              = "NUMBER",
    [TOKEN_CHAR]                = "CHAR",
    [TOKEN_STRING]              = "STRING",
    [TOKEN_NONE]                = "NONE",
    [TOKEN_NULL]                = "NULL",
    [TOKEN_IF]                  = "IF",
    [TOKEN_ELSE]                = "ELSE",
    [TOKEN_NOP]                 = "NOP",
    [TOKEN_HALT]                = "HALT",
    [TOKEN_JUMP]                = "JUMP",
    [TOKEN_FREE]                = "FREE",
    [TOKEN_SIZEOF]              = "SIZEOF",
    [TOKEN_PARSEOF]             = "PARSEOF",
    [TOKEN_REALLOC]             = "REALLOC",
    [TOKEN_ALLOC]               = "ALLOC",
    [TOKEN_SIGNAL]              = "SIGNAL",
    [TOKEN_PUSH]                = "PUSH",
    [TOKEN_POP]                 = "POP",
    [TOKEN_RETURN]              = "RETURN",
    [TOKEN_STATE]               = "STATE",
    [TOKEN_TYPE]                = "TYPE",
    [TOKEN_ACCMOD]              = "ACCMOD",
    [TOKEN_MODIFIER]            = "MODIFIER",
    [TOKEN_LOGICAL]             = "LOGICAL",
    [TOKEN_ID]                  = "ID",
    [TOKEN_PERCENT]             = "PERCENT",
    [TOKEN_COLON]               = "COLON",
    [TOKEN_DOT]                 = "DOT",
    [TOKEN_SEMICOLON]           = "SEMICOLON",
    [TOKEN_EQUAL]               = "EQUAL",
    [TOKEN_COMMA]               = "COMMA",
    [TOKEN_PLUS]                = "PLUS",
    [TOKEN_MINUS]               = "MINUS",
    [TOKEN_STAR]                = "STAR",
    [TOKEN_SLASH]               = "SLASH",
    [TOKEN_QUESTION]            = "QUESTION",
    [TOKEN_TILDE]               = "TILDE",
    [TOKEN_NE_TILDE]            = "NE_TILDE",
    [TOKEN_PIPE]                = "PIPE",
    [TOKEN_AMPERSAND]           = "AMPERSAND",
    [TOKEN_BANG]                = "BANG",
    [TOKEN_CARET]               = "CARET",
    [TOKEN_AT]                  = "AT",
    [TOKEN_GT]                  = "GT",
    [TOKEN_LT]                  = "LT",
    [TOKEN_SHR]                 = "SHR",
    [TOKEN_SHL]                 = "SHL",
    [TOKEN_SAR]                 = "SAR",
    [TOKEN_SAL]                 = "SAL",
    [TOKEN_ROR]                 = "ROR",
    [TOKEN_ROL]                 = "ROL",
    [TOKEN_GE]                  = "GE",
    [TOKEN_LE]                  = "LE",
    [TOKEN_DOUBLE_EQ]           = "DOUBLE_EQ",
    [TOKEN_NE]                  = "NE",
    [TOKEN_PLUS_EQ]             = "PLUS_EQ",
    [TOKEN_MINUS_EQ]            = "MINUS_EQ",
    [TOKEN_STAR_EQ]             = "STAR_EQ",
    [TOKEN_SLASH_EQ]            = "SLASH_EQ",
    [TOKEN_PERCENT_EQ]          = "PERCENT_EQ",
    [TOKEN_PIPE_EQ]             = "PIPE_EQ",
    [TOKEN_AMPERSAND_EQ]        = "AMPERSAND_EQ",
    [TOKEN_CARET_EQ]            = "CARET_EQ",
    [TOKEN_SHL_EQ]              = "SHL_EQ",
    [TOKEN_SHR_EQ]              = "SHR_EQ",
    [TOKEN_SAR_EQ]              = "SAR_EQ",
    [TOKEN_SAL_EQ]              = "SAL_EQ",
    [TOKEN_ROL_EQ]              = "ROL_EQ",
    [TOKEN_ROR_EQ]              = "ROR_EQ",
    [TOKEN_DOUBLE_AMPERSAND]    = "DOUBLE_AMPERSAND",
    [TOKEN_DOUBLE_AT]           = "DOUBLE_AT",
    [TOKEN_DOUBLE_PLUS]         = "DOUBLE_PLUS",
    [TOKEN_DOUBLE_MINUS]        = "DOUBLE_MINUS",
    [TOKEN_INDICATOR]           = "INDICATOR",
    [TOKEN_THEN]                = "THEN",
    [TOKEN_LCURLY]              = "LCURLY",
    [TOKEN_RCURLY]              = "RCURLY",
    [TOKEN_LBRACE]              = "LBRACE",
    [TOKEN_RBRACE]              = "RBRACE",
    [TOKEN_LPAREN]              = "LPAREN",
    [TOKEN_RPAREN]              = "RPAREN",
    [TOKEN_EOF]                 = "EOF",
    [TOKEN_ERROR]               = "ERROR"
};

/* AST node type names array */
const char* ast_node_names[] = {
    [AST_VARIABLE_DECLARATION]   = "VARIABLE_DECLARATION",
    [AST_VARIABLE_WITH_BODY]     = "VARIABLE_WITH_BODY",
    [AST_FUNCTION_DECLARATION]   = "FUNCTION_DECLARATION",
    [AST_ARRAY_ACCESS]           = "ARRAY_ACCESS",
    [AST_BINARY_OPERATION]       = "BINARY_OPERATION",
    [AST_UNARY_OPERATION]        = "UNARY_OPERATION",
    [AST_LITERAL_VALUE]          = "LITERAL_VALUE",
    [AST_IDENTIFIER]             = "IDENTIFIER",
    [AST_REGISTER]               = "REGISTER",
    [AST_ASSIGNMENT]             = "ASSIGNMENT",
    [AST_COMPOUND_ASSIGNMENT]    = "COMPOUND_ASSIGNMENT",
    [AST_BLOCK]                  = "BLOCK",
    [AST_IF_STATEMENT]           = "IF_STATEMENT",
    [AST_RETURN]                 = "RETURN",
    [AST_FREE]                   = "FREE",
    [AST_SIZEOF]                 = "SIZEOF",
    [AST_PARSEOF]                = "PARSEOF",
    [AST_TYPEOF]                 = "TYPEOF",
    [AST_STACK]                  = "STACK",
    [AST_PUSH]                   = "PUSH",
    [AST_POP]                    = "POP",
    [AST_CAST]                   = "CAST",
    [AST_SIGNAL]                 = "SIGNAL",
    [AST_MULTI_INITIALIZER]      = "MULTI_INITIALIZER",
    [AST_LABEL_DECLARATION]      = "LABEL_DECLARATION",
    [AST_JUMP]                   = "JUMP",
    [AST_POSTFIX_CAST]           = "POSTFIX_CAST",
    [AST_FIELD_ACCESS]           = "FIELD_ACCESS",
    [AST_NOP]                    = "NOP",
    [AST_ARRAY_DECLARATION]      = "ARRAY_DECLARATION",
    [AST_HALT]                   = "HALT",
    [AST_TYPE_CHANGE]            = "TYPE_CHANGE",
    [AST_MULTI_ASSIGNMENT]       = "MULTI_ASSIGNMENT",
    [AST_COMPOUND_TYPE]          = "COMPOUND_TYPE",
    [AST_PREFIX_INCREMENT]       = "PREFIX_INCREMENT",
    [AST_PREFIX_DECREMENT]       = "PREFIX_DECREMENT",
    [AST_POSTFIX_INCREMENT]      = "POSTFIX_INCREMENT",
    [AST_POSTFIX_DECREMENT]      = "POSTFIX_DECREMENT",
    [AST_LABEL_VALUE]            = "LABEL_VALUE",
    [AST_ALLOC]                  = "ALLOC",
    [AST_REALLOC]                = "REALLOC",
    [AST_DO_LOOP]                = "DO_LOOP",
    [AST_BREAK]                  = "BREAK",
    [AST_CONTINUE]               = "CONTINUE",
    [AST_TERNARY_OPERATION]      = "TERNARY_OPERATION"
};

/*
 * Get initialization state string
 */
static const char* get_init_state_string(InitState state) {
    switch (state) {
        case INIT_UNINITIALIZED: return "no";
        case INIT_PARTIAL: return "partial";
        case INIT_FULL: return "yes";
        case INIT_CONSTANT: return "const";
        case INIT_DEFAULT: return "default";
        default: return "unknown";
    }
}

/*
 * Get scope level string
 */
static const char* get_scope_level_string(ScopeLevel level) {
    switch (level) {
        case SCOPE_GLOBAL: return "global";
        case SCOPE_FUNCTION: return "function";
        case SCOPE_BLOCK: return "block";
        case SCOPE_LOOP: return "loop";
        case SCOPE_COMPOUND: return "compound";
        default: return "unknown";
    }
}

/*
 * Print section header with title
 */
void print_section_header(const char* title, FILE* out) {
    fprintf(out, "\n");
    fprintf(out, "\033[34m%s\033[0m\n", title);
}

/*
 * Print indentation for tree display
 */
static inline void print_indent(uint8_t level, FILE* out) {
    for (uint8_t i = 0; i < level; i++) fputc(' ', out);
}

/*
 * Recursively print AST node with indentation
 */
static void print_ast_node_recursive(ASTNode* node, uint8_t depth, FILE* out) {
    if (!node) return;

    print_indent(depth, out);
    fprintf(out, "%s", get_ast_node_type_name(node->type));
    
    if (node->value) fprintf(out, ": '%s'", node->value);
    
    if (node->operation_type)
        fprintf(out, " [op: %s]", get_token_type_name(node->operation_type));
    
    fputc('\n', out);

    if (node->state_modifier) {
        print_indent(depth + 1, out);
        fprintf(out, "State Modifier: %s\n", node->state_modifier);
    }

    if (node->access_modifier) {
        print_indent(depth + 1, out);
        fprintf(out, "Access Modifier: %s\n", node->access_modifier);
    }

    if (node->variable_type) {
        print_indent(depth + 1, out);
        print_type_info(node->variable_type, out);
    }

    if (node->default_value) {
        print_indent(depth + 1, out);
        fputs("Default Value:\n", out);
        print_ast_node_recursive(node->default_value, depth + 2, out);
    }

    if (node->left) {
        print_indent(depth + 1, out);
        fputs("Left:\n", out);
        print_ast_node_recursive(node->left, depth + 2, out);
    }

    if (node->right) {
        print_indent(depth + 1, out);
        fputs("Right:\n", out);
        print_ast_node_recursive(node->right, depth + 2, out);
    }

    if (node->extra) {
        print_indent(depth + 1, out);
        fputs("Extra:\n", out);
        
        if (node->type == AST_BLOCK || node->type == AST_FUNCTION_DECLARATION) {
            AST* ast_list = (AST*)node->extra;
            
            if (ast_list) {
                for (uint16_t i = 0; i < ast_list->count; i++)
                    print_ast_node_recursive(ast_list->nodes[i], depth + 2, out);
            }
        } else
            print_ast_node_recursive(node->extra, depth + 2, out);
    }
}

/*
 * Recursive helper for counting AST nodes
 */
static void count_nodes(ASTNode* node, uint32_t* total_nodes, uint32_t* type_counts) {
    if (!node) return;

    (*total_nodes)++;
    
    if ((node->type & (AST_NODE_TYPE_COUNT - 1)) == node->type)
        type_counts[node->type]++;

    count_nodes(node->left, total_nodes, type_counts);
    count_nodes(node->right, total_nodes, type_counts);
    count_nodes(node->extra, total_nodes, type_counts);
    count_nodes(node->default_value, total_nodes, type_counts);
}

/*
 * Print symbol table from semantic context
 */
void print_semantic_symbol_table(SemanticContext* context, FILE* out) {
    if (!context || !out) {
        fputs("No semantic context to display\n", out);
        return;
    }
    
    SymbolTable* table = semantic_get_global_table(context);
    if (!table) {
        fputs("No symbol table available\n", out);
        return;
    }
    
    fprintf(out, "SYMBOL TABLE:\n");
    fprintf(out, "%-20s %-12s %-10s %-12s %-10s %-10s %-10s\n", 
            "Name", "Type", "Const", "Init State", "Used", "Mutable", "Scope");
    fprintf(out, "%-20s %-12s %-10s %-12s %-10s %-10s %-10s\n",
            "--------------------", "------------", "----------", "------------", 
            "----------", "----------", "----------");
    
    size_t total_symbols = 0;
    
    /* Helper function to print table recursively */
    void print_table(SymbolTable* tbl, int indent) {
        char indent_str[32];
        memset(indent_str, ' ', indent * 2);
        indent_str[indent * 2] = '\0';
        
        for (size_t i = 0; i < tbl->capacity; i++) {
            SymbolEntry* entry = tbl->entries[i];
            
            while (entry) {
                fprintf(out, "%s%-20s %-12s %-10s %-12s %-10s %-10s %-10s\n",
                        indent_str,
                        entry->name, 
                        semantic_type_to_string(entry->type),
                        entry->is_constant ? "yes" : "no",
                        get_init_state_string(entry->init_state),
                        entry->is_used ? "yes" : "no",
                        entry->is_mutable ? "yes" : "no",
                        get_scope_level_string(entry->declared_scope));
                
                total_symbols++;
                entry = entry->next;
            }
        }
        
        /* Print child scopes */
        SymbolTable* child = tbl->children;
        while (child) {
            fprintf(out, "\n%sScope: %s (child)\n", indent_str, 
                    get_scope_level_string(child->level));
            print_table(child, indent + 1);
            child = child->next_child;
        }
    }
    
    print_table(table, 0);
    fprintf(out, "\nTotal symbols: %zu\n", total_symbols);
}

/*
 * Print type information from semantic context
 */
void print_semantic_type_info(SemanticContext* context, FILE* out) {
    if (!context || !out) return;
    
    SymbolTable* table = semantic_get_global_table(context);
    if (!table) return;
    
    /* Type statistics */
    size_t type_counts[TYPE_COMPOUND + 1] = {0};
    size_t init_counts[INIT_DEFAULT + 1] = {0};
    size_t total_symbols = 0;
    
    for (size_t i = 0; i < table->capacity; i++) {
        SymbolEntry* entry = table->entries[i];
        while (entry) {
            if (entry->type <= TYPE_COMPOUND) {
                type_counts[entry->type]++;
            }
            if (entry->init_state <= INIT_DEFAULT) {
                init_counts[entry->init_state]++;
            }
            total_symbols++;
            entry = entry->next;
        }
    }
    
    if (total_symbols == 0) {
        fprintf(out, "No symbols to analyze\n");
        return;
    }
    
    fprintf(out, "TYPE DISTRIBUTION:\n");
    fprintf(out, "%-15s %-8s %-10s\n", "Type", "Count", "Percentage");
    fprintf(out, "%-15s %-8s %-10s\n", "---------------", "--------", "----------");
    
    for (int i = TYPE_UNKNOWN; i <= TYPE_COMPOUND; i++) {
        if (type_counts[i] > 0) {
            fprintf(out, "%-15s %-8zu %-9.1f%%\n",
                    semantic_type_to_string(i),
                    type_counts[i],
                    (float)type_counts[i] / total_symbols * 100.0f);
        }
    }
    
    fprintf(out, "\nINITIALIZATION STATE:\n");
    fprintf(out, "%-20s %-8s %-10s\n", "State", "Count", "Percentage");
    fprintf(out, "%-20s %-8s %-10s\n", "--------------------", "--------", "----------");
    
    const char* init_state_names[] = {
        "Uninitialized", "Partial", "Full", "Constant", "Default"
    };
    
    for (int i = 0; i <= INIT_DEFAULT; i++) {
        if (init_counts[i] > 0) {
            fprintf(out, "%-20s %-8zu %-9.1f%%\n",
                    init_state_names[i],
                    init_counts[i],
                    (float)init_counts[i] / total_symbols * 100.0f);
        }
    }
    
    /* Additional type compatibility information */
    fprintf(out, "\nTYPE COMPATIBILITY:\n");
    fprintf(out, "  Int <-> Real: compatible\n");
    fprintf(out, "  none <-> pointer/reference: compatible\n");
    fprintf(out, "  Identical types: always compatible\n");
}

/*
 * Print semantic analysis errors and warnings
 */
void print_semantic_errors_warnings(SemanticContext* context, FILE* out) {
    if (!context || !out) return;
    
    /* Errors and warnings are already reported through errhandler,
       but we can output a summary */
    fprintf(out, "Errors and warnings are reported through the error handler.\n");
    fprintf(out, "Use --log or --verbose flags to see detailed messages.\n");
}

/*
 * Print semantic analysis summary
 */
void print_semantic_summary(SemanticContext* context, FILE* out) {
    if (!context || !out) return;
    
    fprintf(out, "SEMANTIC ANALYSIS SUMMARY:\n");
    fprintf(out, "  Status: %s\n", 
            semantic_has_errors(context) ? "FAILED" : "PASSED");
    fprintf(out, "  Warnings enabled: %s\n",
            semantic_warnings_enabled(context) ? "yes" : "no");
    fprintf(out, "  Total symbols: %zu\n",
            semantic_get_symbol_count(context));
    fprintf(out, "  Exit on error: %s\n",
            context->exit_on_error ? "yes" : "no");
    
    /* Scope depth tracking */
    int depth = 0;
    SymbolTable* scope = context->current_scope;
    while (scope) {
        depth++;
        scope = scope->parent;
    }
    fprintf(out, "  Scope depth: %d\n", depth);
    fprintf(out, "  In function: %s\n", context->in_function ? "yes" : "no");
    fprintf(out, "  In loop: %s\n", context->in_loop ? "yes" : "no");
    fprintf(out, "  Current function: %s\n", 
            context->current_function ? context->current_function : "none");
}

/*
 * Print semantic analysis log
 */
void print_semantic_log(SemanticContext* context, FILE* out) {
    if (!context || !out) {
        fputs("No semantic context to display\n", out);
        return;
    }
    
    fprintf(out, "SEMANTIC ANALYSIS LOG\n");
    fprintf(out, "====================\n\n");
    
    if (semantic_has_errors(context)) {
        fprintf(out, "❌ Semantic analysis FAILED with errors\n\n");
    } else {
        fprintf(out, "✅ Semantic analysis PASSED\n\n");
    }
    
    print_semantic_summary(context, out);
    fprintf(out, "\n");
    
    /* Print symbol table */
    print_semantic_symbol_table(context, out);
    fprintf(out, "\n");
    
    /* Print type information */
    print_semantic_type_info(context, out);
    fprintf(out, "\n");
    
    /* Print scope information */
    fprintf(out, "SCOPE INFORMATION:\n");
    fprintf(out, "  Global symbols: %zu\n", semantic_get_symbol_count(context));
    fprintf(out, "  Current scope: %s\n", 
            get_scope_level_string(context->current_scope->level));
    fprintf(out, "  In function: %s\n", context->in_function ? "yes" : "no");
    fprintf(out, "  In loop: %s\n", context->in_loop ? "yes" : "no");
    fprintf(out, "\n");
    
    /* Print analysis flags */
    fprintf(out, "ANALYSIS SETTINGS:\n");
    fprintf(out, "  Exit on error: %s\n", context->exit_on_error ? "enabled" : "disabled");
    fprintf(out, "  Warnings: %s\n", context->warnings_enabled ? "enabled" : "disabled");
}

/*
 * Print complete semantic analysis
 */
void print_semantic_analysis(SemanticContext* context, FILE* out) {
    if (!context || !out) {
        fputs("No semantic context to display\n", out);
        return;
    }
    
    /* Summary */
    print_semantic_summary(context, out);
    fputc('\n', out);
    
    /* Symbol table */
    print_semantic_symbol_table(context, out);
    fputc('\n', out);
    
    /* Type information */
    print_semantic_type_info(context, out);
}

/*
 * Get AST node type name as string
 */
const char* get_ast_node_type_name(ASTNodeType type) {
    return (type & (AST_NODE_TYPE_COUNT - 1)) == type ? 
           ast_node_names[type] : "UNKNOWN";
}

/*
 * Get token type name as string
 */
const char* get_token_type_name(TokenType type) {
    return (type & (TOKEN_TYPE_COUNT - 1)) == type ? 
           token_names[type] : "UNKNOWN";
}

/*
 * Print AST node inline (without newline)
 */
void print_ast_node_inline(ASTNode* node, FILE* out) {
    if (!node) {
        fputs("NULL", out);
        return;
    }
    
    node->value ? fprintf(out, "'%s'", node->value) : 
                  fputs(get_ast_node_type_name(node->type), out);
}

/*
 * Print type information
 */
void print_type_info(Type* type, FILE* out) {
    if (!type) return;

    fputs("Type: ", out);
    
    for (uint8_t i = 0; i < type->modifier_count; i++)
        fprintf(out, "%s ", type->modifiers[i]);
    
    if (type->pointer_level)
        fprintf(out, "@%d", type->pointer_level);
    
    if (type->is_reference)
        fprintf(out, "&%d", type->is_reference);
    
    if (type->is_register)
        fprintf(out, "%%%d", type->is_register);
    
    if (type->prefix_number)
        fprintf(out, "%d", type->prefix_number);
    
    if (type->compound_count) {
        fputc('(', out);
        
        for (uint8_t i = 0; i < type->compound_count; i++) {
            if (type->compound_types[i])
                fprintf(out, "%s", type->compound_types[i]->name);
            
            if (i < type->compound_count - 1) fputs(", ", out);
        }
        
        fputc(')', out);
    }
    
    if (type->angle_expression) {
        fputc('<', out);
        print_ast_node_inline(type->angle_expression, out);
        fputc('>', out);
    }
    
    if (type->is_array) fputs("[]", out);
    
    fputc('\n', out);
}

/*
 * Print all tokens in lexer
 */
void print_all_tokens(Lexer* lexer, FILE* out) {
    if (!lexer || !lexer->tokens) {
        fputs("No tokens to display\n", out);
        return;
    }
    
    for (uint16_t i = 0; i < lexer->token_count; i++) {
        Token* token = &lexer->tokens[i];
        fprintf(out, "%4u: %-20s", i, get_token_type_name(token->type));
        
        if (token->value && token->value[0])
            fprintf(out, " = '%s'", token->value);
        
        fprintf(out, " [line %u, col %u]\n", token->line, token->column);
    }
}

/*
 * Print tokens grouped by line
 */
void print_tokens_by_line(Lexer* lexer, FILE* out) {
    if (!lexer || !lexer->tokens) {
        fputs("No tokens to display\n", out);
        return;
    }
    
    uint16_t current_line = 0;
    
    for (uint16_t i = 0; i < lexer->token_count; i++) {
        Token* token = &lexer->tokens[i];
        
        if (token->type == TOKEN_EOF) continue;
        
        if (token->line != current_line) {
            if (current_line) fputc('\n', out);
            
            current_line = token->line;
            fprintf(out, "Line %3u: ", current_line);
        }
        
        token->value && token->value[0] ? 
            fprintf(out, "%s ", token->value) : 
            fprintf(out, "%s ", get_token_type_name(token->type));
    }
    
    fputc('\n', out);
}

/*
 * Print token statistics
 */
void print_token_statistics(Lexer* lexer, FILE* out) {
    if (!lexer || !lexer->tokens) {
        fputs("No tokens to analyze\n", out);
        return;
    }

    uint32_t counts[TOKEN_TYPE_COUNT] = {0};
    
    for (uint16_t i = 0; i < lexer->token_count; i++) {
        TokenType type = lexer->tokens[i].type;
        
        if ((type & (TOKEN_TYPE_COUNT - 1)) == type) counts[type]++;
    }

    fprintf(out, "Total: %u\n", lexer->token_count);
    fprintf(out, "Non-EOF: %u\n\n", lexer->token_count - counts[TOKEN_EOF]);
    
    fputs("Distribution:\n", out);
    
    for (uint32_t i = 0; i < TOKEN_TYPE_COUNT; i++) {
        if (counts[i])
            fprintf(out, "  %-20s: %u\n", get_token_type_name(i), counts[i]);
    }
}

/*
 * Print detailed token information
 */
void print_detailed_token_info(Lexer* lexer, FILE* out) {
    if (!lexer || !lexer->tokens) {
        fputs("No tokens to display\n", out);
        return;
    }

    
    for (uint16_t i = 0; i < lexer->token_count; i++) {
        Token* token = &lexer->tokens[i];
        fprintf(out, "%4u: %s\n", i, get_token_type_name(token->type));
        fprintf(out, "     Value: %s\n", token->value ? token->value : "[none]");
        fprintf(out, "     Pos: line %u, col %u\n", token->line, token->column);
        fprintf(out, "     Len: %u bytes\n", token->length);
        
        if (i < lexer->token_count - 1) fputc('\n', out);
    }
}

/*
 * Print tokens with line markers
 */
void print_tokens_in_lines(Lexer* lexer, FILE* out) {
    if (!lexer || !lexer->tokens) {
        fputs("No tokens to display\n", out);
        return;
    }

    uint16_t current_line = 0;
    
    for (uint16_t i = 0; i < lexer->token_count; i++) {
        Token* token = &lexer->tokens[i];
        
        if (token->type == TOKEN_EOF) continue;
        
        if (token->line != current_line) {
            if (current_line) fputs("]\n", out);
            
            current_line = token->line;
            fprintf(out, "[Line %u: ", current_line);
        }
        
        token->value && token->value[0] ? 
            fprintf(out, "%s ", token->value) : 
            fprintf(out, "%s ", get_token_type_name(token->type));
    }
    
    if (current_line) fputs("]\n", out);
}

/*
 * Print AST in detailed tree format
 */
void print_ast_detailed(AST* ast, FILE* out) {
    if (!ast || !ast->nodes || !ast->count) {
        fputs("AST is empty\n", out);
        return;
    }

    for (uint16_t i = 0; i < ast->count; i++) {
        if (!ast->nodes[i]) continue;
        
        fprintf(out, "Statement %u:\n", i + 1);
        print_ast_node_recursive(ast->nodes[i], 1, out);
        
        if (i < ast->count - 1) fputc('\n', out);
    }
}

/*
 * Print AST nodes grouped by type
 */
void print_ast_by_type(AST* ast, FILE* out) {
    if (!ast || !ast->nodes) {
        fputs("AST is empty\n", out);
        return;
    }

    uint32_t type_counts[AST_NODE_TYPE_COUNT] = {0};
    
    for (uint16_t i = 0; i < ast->count; i++)
        if (ast->nodes[i]) type_counts[ast->nodes[i]->type]++;

    fprintf(out, "Total: %u\n\n", ast->count);
    
    for (int i = 0; i < AST_NODE_TYPE_COUNT; i++) {
        if (type_counts[i])
            fprintf( out
                   , "  %-30s: %u\n"
                   , get_ast_node_type_name(i)
                   , type_counts[i]
            );
    }
}

/*
 * Print AST statistics
 */
void print_ast_statistics(AST* ast, FILE* out) {
    if (!ast || !ast->nodes) {
        fputs("AST is empty\n", out);
        return;
    }

    uint32_t total_nodes                = 0;
    uint32_t type_counts[AST_NODE_TYPE_COUNT] = {0};

    for (uint16_t i = 0; i < ast->count; i++)
        count_nodes(ast->nodes[i], &total_nodes, type_counts);

    fprintf(out, "Statements: %u\n", ast->count);
    fprintf(out, "Total nodes: %u\n\n", total_nodes);
    
    fputs("Distribution:\n", out);
    
    for (int i = 0; i < AST_NODE_TYPE_COUNT; i++) {
        if (type_counts[i])
            fprintf( out
                   , "  %-30s: %u\n", get_ast_node_type_name(i)
                   , type_counts[i]
            );
    }
}

/*
 * Print AST with type information
 */
void print_ast_with_types(AST* ast, FILE* out) {
    if (!ast || !ast->nodes) {
        fputs("AST is empty\n", out);
        return;
    }

    for (uint16_t i = 0; i < ast->count; i++) {
        ASTNode* node = ast->nodes[i];
        
        if (!node) continue;
        
        fprintf( out
               , "Statement %u: %s\n"
               , i + 1
               , get_ast_node_type_name(node->type)
        );
        
        if (node->value) fprintf(out, "  Value: '%s'\n", node->value);
        if (node->operation_type)
            fprintf(out, "  Op: %s\n", get_token_type_name(node->operation_type));
        if (node->variable_type) {
            fputs("  Type: ", out);
            print_type_info(node->variable_type, out);
        }
        if (node->state_modifier)
            fprintf(out, "  State: %s\n", node->state_modifier);
        if (node->access_modifier)
            fprintf(out, "  Access: %s\n", node->access_modifier);
        
        uint8_t children = (node->left ? 1 : 0) + (node->right ? 1 : 0) + 
                           (node->extra ? 1 : 0) + (node->default_value ? 1 : 0);
        
        if (children) fprintf(out, "  Children: %u\n", children);
        if (i < ast->count - 1) fputc('\n', out);
    }
}

/*
 * Print AST in compact format
 */
void print_ast_compact(AST* ast, FILE* out) {
    if (!ast || !ast->nodes) {
        fputs("AST is empty\n", out);
        return;
    }

    for (uint16_t i = 0; i < ast->count; i++) {
        ASTNode* node = ast->nodes[i];
        
        if (!node) continue;
        
        fprintf(out, "%u: %s", i + 1, get_ast_node_type_name(node->type));
        if (node->value) fprintf(out, " '%s'", node->value);
        if (node->operation_type)
            fprintf(out, " [%s]", get_token_type_name(node->operation_type));
        fputc('\n', out);
    }
}

/*
 * Enable or disable parser trace
 */
void enable_parser_trace(bool enabled) {
    parser_trace_enabled = enabled;
}

/*
 * Log parser step for debugging
 */
void log_parser_step(ParserState* parser, const char* action, ASTNode* node) {
    if (!parser_trace_enabled || !parser) return;

    FILE* out = trace_file ? trace_file : stdout;
    
    fprintf(out, "[Parser@%u] %s: ", parser->current_token_position, action);
    
    if (node) {
        fprintf(out, "%s", get_ast_node_type_name(node->type));
        
        if (node->value) fprintf(out, " '%s'", node->value);
    }
    
    if (parser->current_token_position < parser->total_tokens) {
        Token* token = &parser->token_stream[parser->current_token_position];
        fprintf(out, " | Current: %s", get_token_type_name(token->type));
        
        if (token->value) fprintf(out, " '%s'", token->value);
    }
    
    fputc('\n', out);
}

/*
 * Print parser trace information
 */
void print_parser_trace(ParserState* parser, FILE* out) {
    if (!parser) {
        fputs("Parser state is NULL\n", out);
        return;
    }

    fprintf(out, "Position: %u/%u\n", parser->current_token_position, 
            parser->total_tokens);
    fprintf(out, "In declaration context: %s\n", 
            parser->in_declaration_context ? "yes" : "no");
    
    if (parser->current_token_position < parser->total_tokens) {
        Token* token = &parser->token_stream[parser->current_token_position];
        fprintf(out, "\nCurrent token:\n");
        fprintf(out, "  Type: %s\n", get_token_type_name(token->type));
        
        if (token->value)
            fprintf(out, "  Value: '%s'\n", token->value);
        
        fprintf(out, "  Pos: line %u, col %u\n", token->line, token->column);
    }
}

/*
 * Print complete analysis with specified mode
 */
void print_complete_analysis(Lexer* lexer, AST* ast, 
                            SemanticContext* semantic, PrintMode mode, FILE* out) {
    switch (mode) {
        case PRINT_TOKENS_ONLY:
            print_section_header("LEXER TOKENS", out);
            print_all_tokens(lexer, out);
            print_token_statistics(lexer, out);
            fprintf(out, "\n");
            break;
            
        case PRINT_AST_ONLY:
            print_section_header("PARSER AST", out);
            print_ast_detailed(ast, out);
            print_ast_statistics(ast, out);
            fprintf(out, "\n");
            break;
            
        case PRINT_SEMANTIC_ONLY:
            if (semantic) {
                print_section_header("SEMANTIC ANALYSIS", out);
                print_semantic_analysis(semantic, out);
                fprintf(out, "\n");
            } else {
                fputs("No semantic context available\n", out);
            }
            break;
            
        case PRINT_SEMANTIC_FULL:
            if (semantic) {
                print_section_header("SEMANTIC ANALYSIS", out);
                print_semantic_symbol_table(semantic, out);
                fputc('\n', out);
                print_semantic_type_info(semantic, out);
                fputc('\n', out);
                print_semantic_summary(semantic, out);
                fprintf(out, "\n");
            }
            break;
            
        case PRINT_SEMANTIC_LOG:
            if (semantic) {
                print_semantic_log(semantic, out);
                fprintf(out, "\n");
            } else {
                fputs("No semantic context available\n", out);
            }
            break;
            
        case PRINT_ALL:
            print_section_header("LEXER TOKENS", out);
            print_all_tokens(lexer, out);
            fprintf(out, "\n");
            
            print_section_header("PARSER AST", out);
            print_ast_detailed(ast, out);
            fprintf(out, "\n");
            
            if (semantic) {
                print_section_header("SEMANTIC ANALYSIS", out);
                print_semantic_analysis(semantic, out);
                fprintf(out, "\n");
            }
            break;
            
        case PRINT_VERBOSE:
            print_section_header("LEXER TOKENS (DETAILED)", out);
            print_detailed_token_info(lexer, out);
            fprintf(out, "\n");
            
            print_section_header("PARSER AST BY TYPE", out);
            print_ast_by_type(ast, out);
            fprintf(out, "\n");
            
            print_section_header("PARSER AST WITH TYPES", out);
            print_ast_with_types(ast, out);
            fprintf(out, "\n");
            
            if (semantic) {
                print_section_header("SEMANTIC ANALYSIS", out);
                print_semantic_analysis(semantic, out);
                fprintf(out, "\n");
            }
            break;
            
        case PRINT_COMPLETE_ANALYSIS:
            print_section_header("LEXER TOKENS", out);
            print_tokens_in_lines(lexer, out);
            fprintf(out, "\n");
            
            print_section_header("PARSER AST", out);
            print_ast_compact(ast, out);
            fprintf(out, "\n");
            
            /* Semantic analysis */
            if (semantic) {
                print_section_header("SEMANTIC ANALYSIS", out);
                print_semantic_analysis(semantic, out);
                fprintf(out, "\n");
            }
            
            /* Statistics */
            ParseStatistics* stats = collect_parse_statistics(lexer, ast, semantic);
            if (stats) {
                print_section_header("STATISTICS", out);
                print_statistics_report(stats, out);
                fprintf(out, "\n");
                free(stats);
            }
            break;
            
        case PRINT_PARSER_TRACE:
            fputs("Parser trace requires parser state\n", out);
            break;
    }
}

/*
 * Collect parse statistics
 */
ParseStatistics* collect_parse_statistics(Lexer* lexer, AST* ast,
                                         SemanticContext* semantic) {
    ParseStatistics* stats = malloc(sizeof(ParseStatistics));
    
    if (!stats) return NULL;
    
    memset(stats, 0, sizeof(ParseStatistics));

    if (lexer && lexer->tokens) {
        stats->total_tokens = lexer->token_count;
        
        for (uint16_t i = 0; i < lexer->token_count; i++) {
            TokenType type = lexer->tokens[i].type;
            
            if (type < 256) stats->token_types[type]++;
        }
    }

    if (ast && ast->nodes) {
        stats->total_nodes = ast->count;
        
        for (uint16_t i = 0; i < ast->count; i++) {
            if (ast->nodes[i]) {
                void count_node(ASTNode* node) {
                    if (!node) return;
                    if (node->type < 64)
                        stats->node_types[node->type]++;
                    
                    count_node(node->left);
                    count_node(node->right);
                    count_node(node->extra);
                    count_node(node->default_value);
                }
                
                count_node(ast->nodes[i]);
            }
        }
    }
    
    if (semantic) {
        stats->symbols_count = semantic_get_symbol_count(semantic);
        stats->semantic_errors = semantic_has_errors(semantic) ? 1 : 0;
        stats->semantic_warnings = semantic_warnings_enabled(semantic) ? 1 : 0;
    }
    
    return stats;
}

/*
 * Print statistics report
 */
void print_statistics_report(ParseStatistics* stats, FILE* out) {
    if (!stats) {
        fputs("No statistics\n", out);
        return;
    }

    fprintf(out, "COMPILATION STATISTICS:\n");
    fprintf(out, "  Tokens: %u\n", stats->total_tokens);
    fprintf(out, "  AST Nodes: %u\n", stats->total_nodes);
    
    if (stats->symbols_count > 0) {
        fprintf(out, "  Symbols: %u\n", stats->symbols_count);
    }
    
    if (stats->semantic_errors > 0) {
        fprintf(out, "  Semantic errors: %u\n", stats->semantic_errors);
    }
    
    if (stats->semantic_warnings > 0) {
        fprintf(out, "  Semantic warnings: %u\n", stats->semantic_warnings);
    }
    
    fputs("\nToken types:\n", out);
    
    for (int i = 0; i < 256; i++) {
        if (stats->token_types[i]) {
            fprintf( out
                   , "  %-20s: %u\n"
                   , get_token_type_name(i)
                   , stats->token_types[i]
            );
        }
    }
    
    fputs("\nAST node types:\n", out);
    
    for (int i = 0; i < 64; i++) {
        if (stats->node_types[i]) {
            fprintf( out
                   , "  %-30s: %u\n"
                   , get_ast_node_type_name(i)
                   , stats->node_types[i]
            );
        }
    }
}
