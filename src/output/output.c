#include "output.h"
#include "../errhandler/errhandler.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Internal trace flag and file for parser logging */
static bool parser_trace_enabled = false;
static FILE* trace_file          = NULL;

/* Mapping from token type to printable name */
const char* token_names[] = {
    [TOKEN_NUMBER]       = "NUMBER",       [TOKEN_CHAR]         = "CHAR",
    [TOKEN_STRING]       = "STRING",       [TOKEN_IF]           = "IF",
    [TOKEN_ELSE]         = "ELSE",         [TOKEN_DO]           = "DO",
    [TOKEN_BREAK]        = "BREAK",        [TOKEN_CONTINUE]     = "CONTINUE",
    [TOKEN_NOP]          = "NOP",          [TOKEN_HALT]         = "HALT",
    [TOKEN_INTERFLAG]    = "INTERFLAG",    [TOKEN_SIGNAL]       = "SIGNAL",
    [TOKEN_KILL]         = "KILL",         [TOKEN_JUMP]         = "JUMP",
    [TOKEN_RETURN]       = "RETURN",       [TOKEN_SIZEOF]       = "SIZEOF",
    [TOKEN_TYPEOF]       = "TYPEOF",       [TOKEN_ALLOC]        = "ALLOC",
    [TOKEN_CALLOC]       = "CALLOC",       [TOKEN_REALLOC]      = "REALLOC",
    [TOKEN_FREE]         = "FREE",         [TOKEN_NONE]         = "NONE",
    [TOKEN_EXTENDS]      = "EXTENDS",      [TOKEN_TYPE]         = "TYPE",
    [TOKEN_CONSTMOD]     = "CONSTMOD",     [TOKEN_ACCMOD]       = "ACCMOD",
    [TOKEN_SIGNEDMOD]    = "SIGNEDMOD",    [TOKEN_MEMMOD]       = "MEMMOD",
    [TOKEN_LOGICAL]      = "LOGICAL",      [TOKEN_ID]           = "ID",
    [TOKEN_PERCENT]      = "PERCENT",      [TOKEN_COLON]        = "COLON",
    [TOKEN_DOT]          = "DOT",          [TOKEN_SEMICOLON]    = "SEMICOLON",
    [TOKEN_EQUAL]        = "EQUAL",        [TOKEN_COMMA]        = "COMMA",
    [TOKEN_PLUS]         = "PLUS",         [TOKEN_MINUS]        = "MINUS",
    [TOKEN_STAR]         = "STAR",         [TOKEN_SLASH]        = "SLASH",
    [TOKEN_QUESTION]     = "QUESTION",     [TOKEN_TILDE]        = "TILDE",
    [TOKEN_NE_TILDE]     = "NE_TILDE",     [TOKEN_PIPE]         = "PIPE",
    [TOKEN_AMPERSAND]    = "AMPERSAND",    [TOKEN_BANG]         = "BANG",
    [TOKEN_CARET]        = "CARET",        [TOKEN_AT]           = "AT",
    [TOKEN_GT]           = "GT",           [TOKEN_LT]           = "LT",
    [TOKEN_SHR]          = "SHR",          [TOKEN_SHL]          = "SHL",
    [TOKEN_SAR]          = "SAR",          [TOKEN_SAL]          = "SAL",
    [TOKEN_ROR]          = "ROR",          [TOKEN_ROL]          = "ROL",
    [TOKEN_GE]           = "GE",           [TOKEN_LE]           = "LE",
    [TOKEN_DOUBLE_EQ]    = "DOUBLE_EQ",    [TOKEN_NE]           = "NE",
    [TOKEN_PLUS_EQ]      = "PLUS_EQ",      [TOKEN_MINUS_EQ]     = "MINUS_EQ",
    [TOKEN_STAR_EQ]      = "STAR_EQ",      [TOKEN_SLASH_EQ]     = "SLASH_EQ",
    [TOKEN_PERCENT_EQ]   = "PERCENT_EQ",   [TOKEN_PIPE_EQ]      = "PIPE_EQ",
    [TOKEN_AMPERSAND_EQ] = "AMPERSAND_EQ", [TOKEN_CARET_EQ]     = "CARET_EQ",
    [TOKEN_SHL_EQ]       = "SHL_EQ",       [TOKEN_SHR_EQ]       = "SHR_EQ",
    [TOKEN_SAR_EQ]       = "SAR_EQ",       [TOKEN_SAL_EQ]       = "SAL_EQ",
    [TOKEN_ROL_EQ]       = "ROL_EQ",       [TOKEN_ROR_EQ]       = "ROR_EQ",
    [TOKEN_DOUBLE_PLUS]  = "DOUBLE_PLUS",  [TOKEN_DOUBLE_MINUS] = "DOUBLE_MINUS",
    [TOKEN_THEN]         = "THEN",         [TOKEN_LCURLY]       = "LCURLY",
    [TOKEN_RCURLY]       = "RCURLY",       [TOKEN_LBRACE]       = "LBRACE",
    [TOKEN_RBRACE]       = "RBRACE",       [TOKEN_LPAREN]       = "LPAREN",
    [TOKEN_RPAREN]       = "RPAREN",       [TOKEN_EOF]          = "EOF",
    [TOKEN_ERRORCODE]    = "ERRORCODE"
};

/* Mapping from AST node type to printable name */
const char* ast_node_names[] = {
    [AST_VARIABLE_DECLARATION] = "VARIABLE_DECLARATION",
    [AST_VARIABLE_WITH_BODY]   = "VARIABLE_WITH_BODY",
    [AST_VARIABLE_LIST]        = "VARIABLE_LIST",
    [AST_FUNCTION_DECLARATION] = "FUNCTION_DECLARATION",
    [AST_ARRAY_ACCESS]         = "ARRAY_ACCESS",
    [AST_BINARY_OPERATION]     = "BINARY_OPERATION",
    [AST_UNARY_OPERATION]      = "UNARY_OPERATION",
    [AST_LITERAL_VALUE]        = "LITERAL_VALUE",
    [AST_IDENTIFIER]           = "IDENTIFIER",
    [AST_REGISTER]             = "REGISTER",
    [AST_ASSIGNMENT]           = "ASSIGNMENT",
    [AST_COMPOUND_ASSIGNMENT]  = "COMPOUND_ASSIGNMENT",
    [AST_BLOCK]                = "BLOCK",
    [AST_IF_STATEMENT]         = "IF_STATEMENT",
    [AST_ELSE_STATEMENT]       = "ELSE_STATEMENT",
    [AST_RETURN]               = "RETURN",
    [AST_FREE]                 = "FREE",
    [AST_SIZEOF]               = "SIZEOF",
    [AST_STACK]                = "STACK",
    [AST_PUSH]                 = "PUSH",
    [AST_POP]                  = "POP",
    [AST_CAST]                 = "CAST",
    [AST_SIGNAL]               = "SIGNAL",
    [AST_INTERFLAG]            = "INTERFLAG",
    [AST_MULTI_INITIALIZER]    = "MULTI_INITIALIZER",
    [AST_LABEL_DECLARATION]    = "LABEL_DECLARATION",
    [AST_JUMP]                 = "JUMP",
    [AST_POSTFIX_CAST]         = "POSTFIX_CAST",
    [AST_FIELD_ACCESS]         = "FIELD_ACCESS",
    [AST_NOP]                  = "NOP",
    [AST_ARRAY_DECLARATION]    = "ARRAY_DECLARATION",
    [AST_HALT]                 = "HALT",
    [AST_TYPE_CHANGE]          = "TYPE_CHANGE",
    [AST_MULTI_ASSIGNMENT]     = "MULTI_ASSIGNMENT",
    [AST_COMPOUND_TYPE]        = "COMPOUND_TYPE",
    [AST_PREFIX_INCREMENT]     = "PREFIX_INCREMENT",
    [AST_PREFIX_DECREMENT]     = "PREFIX_DECREMENT",
    [AST_POSTFIX_INCREMENT]    = "POSTFIX_INCREMENT",
    [AST_POSTFIX_DECREMENT]    = "POSTFIX_DECREMENT",
    [AST_LABEL_VALUE]          = "LABEL_VALUE",
    [AST_ALLOC]                = "ALLOC",
    [AST_REALLOC]              = "REALLOC",
    [AST_DO_LOOP]              = "DO_LOOP",
    [AST_BREAK]                = "BREAK",
    [AST_CONTINUE]             = "CONTINUE",
    [AST_TERNARY_OPERATION]    = "TERNARY_OPERATION",
    [AST_TYPEOF]               = "TYPEOF",
    [AST_KILL]                 = "KILL"
};

/* Static buffers for indentation to avoid repeated allocation */
static char indent_buffer[64];

/* Forward declaration for recursive AST printing */
static void print_ast_node_recursive(ASTNode* node, uint8_t depth, FILE* out);

/* Convert an initialization state to a human-readable string. */
static const char* get_init_state_string(InitState state) {
    static const char* strings[] = {
        [INIT_UNINITIALIZED] = "no",
        [INIT_PARTIAL]       = "partial",
        [INIT_FULL]          = "yes",
        [INIT_CONSTANT]      = "const",
        [INIT_DEFAULT]       = "default"
    };
    return (state <= INIT_DEFAULT) ? strings[state] : "unknown";
}

/* Convert a scope level to a human-readable string. */
static const char* get_scope_level_string(ScopeLevel level) {
    static const char* strings[] = {
        [SCOPE_GLOBAL]   = "global",
        [SCOPE_FUNCTION] = "function",
        [SCOPE_BLOCK]    = "block",
        [SCOPE_LOOP]     = "loop",
        [SCOPE_COMPOUND] = "compound"
    };
    return (level <= SCOPE_COMPOUND) ? strings[level] : "unknown";
}

/* Initialize indentation buffer with spaces. Called once at startup. */
static void init_indent_buffer(void) {
    static bool initialized = false;
    if (!initialized) {
        memset(indent_buffer, ' ', sizeof(indent_buffer) - 1);
        indent_buffer[sizeof(indent_buffer) - 1] = '\0';
        initialized = true;
    }
}

/* Print indentation spaces using pre-initialized buffer. */
static inline void print_indent(uint8_t level, FILE* out) {
    if (level >= sizeof(indent_buffer)) level = sizeof(indent_buffer) - 1;
    fwrite(indent_buffer, 1, level, out);
}

/* Helper to print a node list (AST*) with given depth. */
static void print_node_list(AST* list, uint8_t depth, FILE* out) {
    if (!list || !list->nodes) return;
    for (uint16_t i = 0; i < list->count; i++) {
        if (list->nodes[i]) {
            print_ast_node_recursive(list->nodes[i], depth, out);
        }
    }
}

/* Recursively print an AST node and its children with indentation. */
static void print_ast_node_recursive(ASTNode* node, uint8_t depth, FILE* out) {
    if (!node) return;

    init_indent_buffer();
    print_indent(depth, out);
    fprintf(out, "%s", get_ast_node_type_name(node->type));

    if (node->value) fprintf(out, ": '%s'", node->value);
    if (node->operation_type) {
        fprintf(out, " [op: %s]", get_token_type_name(node->operation_type));
    }
    fputc('\n', out);

    /* Print modifiers if present */
    if (node->state_modifier) {
        print_indent(depth + 1, out);
        fprintf(out, "State Modifier: %s\n", node->state_modifier);
    }
    if (node->access_modifier) {
        print_indent(depth + 1, out);
        fprintf(out, "Access Modifier: %s\n", node->access_modifier);
    }
    if (node->parent_struct) {
        print_indent(depth + 1, out);
        fprintf(out, "Parent Struct: %s\n", node->parent_struct);
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

    /* Print children */
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

    /* Handle 'extra' field based on node type */
    if (node->extra) {
        print_indent(depth + 1, out);
        fputs("Extra:\n", out);

        switch (node->type) {
            case AST_VARIABLE_LIST:
            case AST_BLOCK:
            case AST_FUNCTION_DECLARATION:
            case AST_MULTI_INITIALIZER:
                print_node_list((AST*)node->extra, depth + 2, out);
                break;
            case AST_ELSE_STATEMENT:
                print_ast_node_recursive((ASTNode*)node->extra, depth + 2, out);
                break;
            default:
                print_ast_node_recursive((ASTNode*)node->extra, depth + 2, out);
                break;
        }
    }
}

/* Recursively count AST nodes and update statistics. */
static void count_nodes(ASTNode* node, uint32_t* total_nodes, uint32_t* type_counts) {
    if (!node) return;
    (*total_nodes)++;
    if (IS_VALID_AST_NODE_TYPE(node->type)) {
        type_counts[node->type]++;
    }
    count_nodes(node->left, total_nodes, type_counts);
    count_nodes(node->right, total_nodes, type_counts);
    if (node->extra) {
        /* Handle both AST* and ASTNode* cases for counting */
        if (node->type == AST_VARIABLE_LIST || node->type == AST_BLOCK ||
            node->type == AST_FUNCTION_DECLARATION || node->type == AST_MULTI_INITIALIZER) {
            AST* list = (AST*)node->extra;
            if (list && list->nodes) {
                for (uint16_t i = 0; i < list->count; i++) {
                    count_nodes(list->nodes[i], total_nodes, type_counts);
                }
            }
        } else {
            count_nodes((ASTNode*)node->extra, total_nodes, type_counts);
        }
    }
    count_nodes(node->default_value, total_nodes, type_counts);
}

void print_section_header(const char* title, FILE* out) {
    fprintf(out, "\n\033[34m%s\033[0m\n", title);
}

void print_all_tokens(Lexer* lexer, FILE* out) {
    if (!lexer || !lexer->tokens) {
        fputs("No tokens to display\n", out);
        return;
    }
    for (uint16_t i = 0; i < lexer->token_count; i++) {
        Token* token = &lexer->tokens[i];
        fprintf(out, "%4u: %-20s", i, get_token_type_name(token->type));
        if (token->value && token->value[0]) {
            fprintf(out, " = '%s'", token->value);
        }
        fprintf(out, " [line %u, col %u]\n", token->line, token->column);
    }
}

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

void print_token_statistics(Lexer* lexer, FILE* out) {
    if (!lexer || !lexer->tokens) {
        fputs("No tokens to analyze\n", out);
        return;
    }
    uint32_t counts[TOKEN_TYPE_COUNT] = {0};
    for (uint16_t i = 0; i < lexer->token_count; i++) {
        TokenType type = lexer->tokens[i].type;
        if (IS_VALID_TOKEN_TYPE(type)) counts[type]++;
    }
    fprintf(out, "Total: %llu\n", lexer->token_count);
    fprintf(out, "Non-EOF: %llu\n\n", lexer->token_count - counts[TOKEN_EOF]);
    fputs("Distribution:\n", out);
    for (uint32_t i = 0; i < TOKEN_TYPE_COUNT; i++) {
        if (counts[i]) {
            fprintf(out, "  %-20s: %u\n", get_token_type_name(i), counts[i]);
        }
    }
}

void print_parser_trace(ParserState* parser, FILE* out) {
    if (!parser) {
        fputs("Parser state is NULL\n", out);
        return;
    }
    fprintf(out, "Position: %u/%u\n", parser->current_token_position, parser->total_tokens);
    fprintf(out, "In declaration context: %s\n",
            parser->in_declaration_context ? "yes" : "no");
    if (parser->current_token_position < parser->total_tokens) {
        Token* token = &parser->token_stream[parser->current_token_position];
        fprintf(out, "\nCurrent token:\n  Type: %s\n", get_token_type_name(token->type));
        if (token->value) fprintf(out, "  Value: '%s'\n", token->value);
        fprintf(out, "  Pos: line %u, col %u\n", token->line, token->column);
    }
}

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

void print_ast_statistics(AST* ast, FILE* out) {
    if (!ast || !ast->nodes) {
        fputs("AST is empty\n", out);
        return;
    }
    uint32_t total_nodes = 0;
    uint32_t type_counts[AST_NODE_TYPE_COUNT];
    memset(type_counts, 0, sizeof(type_counts));

    for (uint16_t i = 0; i < ast->count; i++) {
        count_nodes(ast->nodes[i], &total_nodes, type_counts);
    }
    fprintf(out, "Statements: %u\n", ast->count);
    fprintf(out, "Total nodes: %u\n\n", total_nodes);
    fputs("Distribution:\n", out);
    for (size_t i = 0; i < AST_NODE_TYPE_COUNT; i++) {
        if (type_counts[i]) {
            fprintf(out, "  %-30s: %u\n", get_ast_node_type_name(i), type_counts[i]);
        }
    }
}

/* Helper to print symbol table recursively without nested functions */
static void print_symbol_table_recursive(SymbolTable* table, int indent, FILE* out, size_t* total) {
    char indent_str[32];
    memset(indent_str, ' ', indent * 2);
    indent_str[indent * 2] = '\0';

    for (size_t i = 0; i < table->capacity; i++) {
        SymbolEntry* entry = table->entries[i];
        while (entry) {
            fprintf(out, "%s%-20s %-12s %-10s %-12s %-10s %-10s %-10s\n",
                    indent_str,
                    entry->name,
                    semantic__type_to_string(entry->type),
                    entry->is_constant ? "yes" : "no",
                    get_init_state_string(entry->init_state),
                    entry->is_used ? "yes" : "no",
                    entry->is_mutable ? "yes" : "no",
                    get_scope_level_string(entry->declared_scope));
            (*total)++;
            entry = entry->next;
        }
    }
    SymbolTable* child = table->children;
    while (child) {
        fprintf(out, "\n%sScope: %s (child)\n", indent_str,
                get_scope_level_string(child->level));
        print_symbol_table_recursive(child, indent + 1, out, total);
        child = child->next_child;
    }
}

void print_semantic_symbol_table(SemanticContext* context, FILE* out) {
    if (!context || !out) {
        fputs("No semantic context to display\n", out);
        return;
    }
    SymbolTable* table = semantic__get_global_table(context);
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
    print_symbol_table_recursive(table, 0, out, &total_symbols);
    fprintf(out, "\nTotal symbols: %zu\n", total_symbols);
}

void print_semantic_type_info(SemanticContext* context, FILE* out) {
    if (!context || !out) return;
    SymbolTable* table = semantic__get_global_table(context);
    if (!table) return;

    size_t type_counts[TYPE_COMPOUND + 1];
    size_t init_counts[INIT_DEFAULT + 1];
    memset(type_counts, 0, sizeof(type_counts));
    memset(init_counts, 0, sizeof(init_counts));
    size_t total_symbols = 0;

    for (size_t i = 0; i < table->capacity; i++) {
        SymbolEntry* entry = table->entries[i];
        while (entry) {
            if (entry->type <= TYPE_COMPOUND) type_counts[entry->type]++;
            if (entry->init_state <= INIT_DEFAULT) init_counts[entry->init_state]++;
            total_symbols++;
            entry = entry->next;
        }
    }
    if (total_symbols == 0) {
        fprintf(out, "No symbols to analyze\n");
        return;
    }

    fprintf(out, "TYPE DISTRIBUTION:\n%-15s %-8s %-10s\n%-15s %-8s %-10s\n",
            "Type", "Count", "Percentage", "---------------", "--------", "----------");
    for (int i = TYPE_UNKNOWN; i <= TYPE_COMPOUND; i++) {
        if (type_counts[i] > 0) {
            fprintf(out, "%-15s %-8zu %-9.1f%%\n",
                    semantic__type_to_string(i),
                    type_counts[i],
                    (float)type_counts[i] / total_symbols * 100.0f);
        }
    }

    fprintf(out, "\nINITIALIZATION STATE:\n%-20s %-8s %-10s\n%-20s %-8s %-10s\n",
            "State", "Count", "Percentage", "--------------------", "--------", "----------");
    const char* init_state_names[] = {"Uninitialized", "Partial", "Full", "Constant", "Default"};
    for (int i = 0; i <= INIT_DEFAULT; i++) {
        if (init_counts[i] > 0) {
            fprintf(out, "%-20s %-8zu %-9.1f%%\n",
                    init_state_names[i],
                    init_counts[i],
                    (float)init_counts[i] / total_symbols * 100.0f);
        }
    }
}

void print_semantic_summary(SemanticContext* context, FILE* out) {
    if (!context || !out) return;
    fprintf(out, "SEMANTIC ANALYSIS SUMMARY:\n");
    fprintf(out, "  Status: %s\n", semantic__has_errors(context) ? "FAILED" : "PASSED");
    fprintf(out, "  Warnings enabled: %s\n", semantic__warnings_enabled(context) ? "yes" : "no");
    fprintf(out, "  Total symbols: %zu\n", semantic__get_symbol_count(context));
    fprintf(out, "  Exit on error: %s\n", context->exit_on_error ? "yes" : "no");

    int depth = 0;
    SymbolTable* scope = context->current_scope;
    while (scope) { depth++; scope = scope->parent; }
    fprintf(out, "  Scope depth: %d\n", depth);
    fprintf(out, "  In function: %s\n", context->in_function ? "yes" : "no");
    fprintf(out, "  In loop: %s\n", context->in_loop ? "yes" : "no");
    fprintf(out, "  Current function: %s\n",
            context->current_function ? context->current_function : "none");
}

void print_semantic_analysis(SemanticContext* context, FILE* out) {
    if (!context || !out) {
        fputs("No semantic context to display\n", out);
        return;
    }
    print_semantic_summary(context, out);
    fputc('\n', out);
    print_semantic_symbol_table(context, out);
    fputc('\n', out);
    print_semantic_type_info(context, out);
}

void print_semantic_log(SemanticContext* context, FILE* out) {
    if (!context || !out) {
        fputs("No semantic context to display\n", out);
        return;
    }
    fprintf(out, "SEMANTIC ANALYSIS LOG\n");
    fprintf(out, "Semantic analysis %s\n\n",
            semantic__has_errors(context) ? "FAILED with errors" : "PASSED");
    print_semantic_summary(context, out);
    fprintf(out, "\n");
    print_semantic_symbol_table(context, out);
    fprintf(out, "\n");
    print_semantic_type_info(context, out);
    fprintf(out, "\nSCOPE INFORMATION:\n");
    fprintf(out, "  Global symbols: %zu\n", semantic__get_symbol_count(context));
    fprintf(out, "  Current scope: %s\n",
            get_scope_level_string(context->current_scope->level));
    fprintf(out, "  In function: %s\n", context->in_function ? "yes" : "no");
    fprintf(out, "  In loop: %s\n", context->in_loop ? "yes" : "no");
    fprintf(out, "\nANALYSIS SETTINGS:\n");
    fprintf(out, "  Exit on error: %s\n", context->exit_on_error ? "enabled" : "disabled");
    fprintf(out, "  Warnings: %s\n", context->warnings_enabled ? "enabled" : "disabled");
}

const char* get_ast_node_type_name(ASTNodeType type) {
    return IS_VALID_AST_NODE_TYPE(type) ? ast_node_names[type] : "UNKNOWN";
}

const char* get_token_type_name(TokenType type) {
    return IS_VALID_TOKEN_TYPE(type) ? token_names[type] : "UNKNOWN";
}

void print_type_info(Type* type, FILE* out) {
    if (!type) return;
    fputs("Type: ", out);
    for (uint8_t i = 0; i < type->modifier_count; i++) {
        fprintf(out, "%s ", type->modifiers[i]);
    }
    if (type->pointer_level) fprintf(out, "@%d", type->pointer_level);
    if (type->is_reference) fprintf(out, "&%d", type->is_reference);
    if (type->prefix_number) fprintf(out, "%d", type->prefix_number);
    if (type->compound_count) {
        fputc('(', out);
        for (uint8_t i = 0; i < type->compound_count; i++) {
            if (type->compound_types[i]) fprintf(out, "%s", type->compound_types[i]->name);
            if (i < type->compound_count - 1) fputs(", ", out);
        }
        fputc(')', out);
    }
    if (type->angle_expression) {
        fputc('<', out);
        fputs(get_ast_node_type_name(type->angle_expression->type), out);
        fputc('>', out);
    }
    if (type->is_array) fputs("[]", out);
    fputc('\n', out);
}

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
        case PRINT_SEMANTIC_FULL:
        case PRINT_SEMANTIC_LOG:
            if (semantic) {
                print_section_header("SEMANTIC ANALYSIS", out);
                if (mode == PRINT_SEMANTIC_ONLY) print_semantic_analysis(semantic, out);
                else if (mode == PRINT_SEMANTIC_FULL) {
                    print_semantic_symbol_table(semantic, out);
                    fputc('\n', out);
                    print_semantic_type_info(semantic, out);
                    fputc('\n', out);
                    print_semantic_summary(semantic, out);
                } else print_semantic_log(semantic, out);
                fprintf(out, "\n");
            } else fputs("No semantic context available\n", out);
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
        case PRINT_COMPLETE_ANALYSIS:
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
            {
                ParseStatistics* stats = collect_parse_statistics(lexer, ast, semantic);
                if (stats) {
                    print_section_header("STATISTICS", out);
                    print_statistics_report(stats, out);
                    fprintf(out, "\n");
                    free(stats);
                }
            }
            break;
        case PRINT_PARSER_TRACE:
            fputs("Parser trace requires parser state\n", out);
            break;
    }
}

ParseStatistics* collect_parse_statistics(Lexer* lexer, AST* ast,
                                          SemanticContext* semantic) {
    ParseStatistics* stats = calloc(1, sizeof(ParseStatistics));
    if (!stats) return NULL;

    if (lexer && lexer->tokens) {
        stats->total_tokens = lexer->token_count;
        for (uint16_t i = 0; i < lexer->token_count; i++) {
            TokenType type = lexer->tokens[i].type;
            if (type < 256) stats->token_types[type]++;
        }
    }
    if (ast && ast->nodes) {
        uint32_t total_nodes = 0;
        uint32_t type_counts[AST_NODE_TYPE_COUNT];
        memset(type_counts, 0, sizeof(type_counts));
        for (uint16_t i = 0; i < ast->count; i++) {
            if (ast->nodes[i]) {
                count_nodes(ast->nodes[i], &total_nodes, type_counts);
            }
        }
        stats->total_nodes = total_nodes;
        memcpy(stats->node_types, type_counts,
               sizeof(type_counts) < sizeof(stats->node_types) ? sizeof(type_counts) : sizeof(stats->node_types));
    }
    if (semantic) {
        stats->symbols_count = semantic__get_symbol_count(semantic);
        stats->semantic_errors = semantic__has_errors(semantic) ? 1 : 0;
        stats->semantic_warnings = semantic__warnings_enabled(semantic) ? 1 : 0;
    }
    return stats;
}

void print_statistics_report(ParseStatistics* stats, FILE* out) {
    if (!stats) {
        fputs("No statistics\n", out);
        return;
    }
    fprintf(out, "COMPILATION STATISTICS:\n");
    fprintf(out, "  Tokens: %u\n", stats->total_tokens);
    fprintf(out, "  AST Nodes: %u\n", stats->total_nodes);
    if (stats->symbols_count > 0) fprintf(out, "  Symbols: %u\n", stats->symbols_count);
    if (stats->semantic_errors > 0) fprintf(out, "  Semantic errors: %u\n", stats->semantic_errors);
    if (stats->semantic_warnings > 0) fprintf(out, "  Semantic warnings: %u\n", stats->semantic_warnings);

    fputs("\nToken types:\n", out);
    for (int i = 0; i < 256; i++) {
        if (stats->token_types[i]) {
            fprintf(out, "  %-20s: %u\n", get_token_type_name(i), stats->token_types[i]);
        }
    }
    fputs("\nAST node types:\n", out);
    for (size_t i = 0; i < AST_NODE_TYPE_COUNT; i++) {
        if (stats->node_types[i]) {
            fprintf(out, "  %-30s: %u\n", get_ast_node_type_name(i), stats->node_types[i]);
        }
    }
}

void enable_parser_trace(bool enabled) {
    parser_trace_enabled = enabled;
}

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
