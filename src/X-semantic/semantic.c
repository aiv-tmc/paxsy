#include "semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define TABLE_SIZE 64
#define MAX_SCOPE_DEPTH 256

static void semantic_report_error_va(SemanticContext* context, ASTNode* node, const char* format, va_list args);

SemanticContext* semantic_create_context(void) {
    SemanticContext* context = calloc(1, sizeof(SemanticContext));
    context->global_scope = calloc(1, sizeof(Scope));
    context->global_scope->bucket_count = TABLE_SIZE;
    context->global_scope->symbols = calloc(TABLE_SIZE, sizeof(Symbol*));
    context->global_scope->level = 0;
    context->global_scope->parent = NULL;
    context->current_scope = context->global_scope;
    context->has_errors = false;
    context->error_count = 0;
    return context;
}

void semantic_destroy_context(SemanticContext* context) {
    if (!context) return;
    
    // Clean up all scopes
    Scope* scope = context->global_scope;
    while (scope) {
        Scope* next = scope->parent;
        
        // Clean symbols in this scope
        for (int i = 0; i < scope->bucket_count; i++) {
            Symbol* symbol = scope->symbols[i];
            while (symbol) {
                Symbol* next_symbol = symbol->next_in_bucket;
                free(symbol->name);
                free(symbol);
                symbol = next_symbol;
            }
        }
        
        free(scope->symbols);
        free(scope);
        scope = next;
    }
    
    free(context);
}

static unsigned int hash(const char* str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) hash = ((hash << 5) + hash) + c;
    return hash;
}

void semantic_enter_scope(SemanticContext* context) {
    Scope* new_scope = calloc(1, sizeof(Scope));
    new_scope->parent = context->current_scope;
    new_scope->bucket_count = TABLE_SIZE;
    new_scope->symbols = calloc(TABLE_SIZE, sizeof(Symbol*));
    new_scope->level = context->current_scope->level + 1;
    
    if (new_scope->level > MAX_SCOPE_DEPTH) {
        semantic_report_error(context, NULL, "Scope nesting too deep");
        free(new_scope->symbols);
        free(new_scope);
        return;
    }
    
    context->current_scope = new_scope;
}

void semantic_exit_scope(SemanticContext* context) {
    if (context->current_scope == context->global_scope) {
        semantic_report_error(context, NULL, "Cannot exit global scope");
        return;
    }
    
    Scope* old_scope = context->current_scope;
    context->current_scope = old_scope->parent;
    
    // Clean symbols in old scope
    for (int i = 0; i < old_scope->bucket_count; i++) {
        Symbol* symbol = old_scope->symbols[i];
        while (symbol) {
            Symbol* next = symbol->next_in_bucket;
            free(symbol->name);
            free(symbol);
            symbol = next;
        }
    }
    
    free(old_scope->symbols);
    free(old_scope);
}

Symbol* semantic_lookup_symbol(SemanticContext* context, const char* name) {
    if (!context || !name) return NULL;
    
    Scope* scope = context->current_scope;
    while (scope) {
        unsigned int index = hash(name) % scope->bucket_count;
        Symbol* symbol = scope->symbols[index];
        
        while (symbol) {
            if (strcmp(symbol->name, name) == 0) {
                return symbol;
            }
            symbol = symbol->next_in_bucket;
        }
        
        scope = scope->parent;
    }
    
    return NULL;
}

Symbol* semantic_lookup_symbol_current_scope(SemanticContext* context, const char* name) {
    if (!context || !name) return NULL;
    
    unsigned int index = hash(name) % context->current_scope->bucket_count;
    Symbol* symbol = context->current_scope->symbols[index];
    
    while (symbol) {
        if (strcmp(symbol->name, name) == 0) {
            return symbol;
        }
        symbol = symbol->next_in_bucket;
    }
    
    return NULL;
}

bool semantic_insert_symbol(SemanticContext* context, Symbol* symbol) {
    if (!context || !symbol || !symbol->name) return false;
    
    // Check for duplicate in current scope only
    if (semantic_lookup_symbol_current_scope(context, symbol->name)) {
        semantic_report_error(context, symbol->ast_node, 
                            "Symbol '%s' already declared in this scope", symbol->name);
        return false;
    }
    
    symbol->scope_level = context->current_scope->level;
    
    unsigned int index = hash(symbol->name) % context->current_scope->bucket_count;
    symbol->next_in_bucket = context->current_scope->symbols[index];
    context->current_scope->symbols[index] = symbol;
    context->current_scope->symbol_count++;
    
    return true;
}

void semantic_report_error(SemanticContext* context, ASTNode* node, const char* format, ...) {
    va_list args;
    va_start(args, format);
    semantic_report_error_va(context, node, format, args);
    va_end(args);
}

static void semantic_report_error_va(SemanticContext* context, ASTNode* node, const char* format, va_list args) {
    if (node && node->line_number) {
        fprintf(stderr, "Error at line %d: ", node->line_number);
    } else {
        fprintf(stderr, "Error: ");
    }
    
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    
    context->has_errors = true;
    context->error_count++;
}

ObjectKind get_object_kind_from_state(const char* state_modifier) {
    if (!state_modifier) return OBJ_VARIABLE;
    
    if (strcmp(state_modifier, "var") == 0 || 
        strcmp(state_modifier, "let") == 0)
        return OBJ_VARIABLE;
    else if (strcmp(state_modifier, "func") == 0)
        return OBJ_FUNCTION;
    else if (strcmp(state_modifier, "struct") == 0)
        return OBJ_STRUCT;
    else if (strcmp(state_modifier, "class") == 0)
        return OBJ_CLASS;
    else if (strcmp(state_modifier, "obj") == 0)
        return OBJ_OBJECT;
    
    return OBJ_VARIABLE;
}

const char* get_type_name(Type* type) {
    if (!type || !type->name) return "Void";
    return type->name;
}

bool is_builtin_type(const char* type_name) {
    if (!type_name) return false;
    
    return strcmp(type_name, "Int") == 0 ||
           strcmp(type_name, "Real") == 0 ||
           strcmp(type_name, "Char") == 0 ||
           strcmp(type_name, "Void") == 0 ||
           strcmp(type_name, "Bool") == 0;
}

Symbol* find_symbol_by_type_name(SemanticContext* context, const char* type_name) {
    if (!context || !type_name) return NULL;
    if (is_builtin_type(type_name)) return NULL;
    return semantic_lookup_symbol(context, type_name);
}

bool validate_variable_declaration(SemanticContext* context, ASTNode* node) {
    if (!node || !node->variable_type) return false;
    
    const char* type_name = get_type_name(node->variable_type);
    
    if (!is_builtin_type(type_name)) {
        Symbol* type_symbol = find_symbol_by_type_name(context, type_name);
        if (!type_symbol) {
            semantic_report_error(context, node, 
                                "Type '%s' not found", type_name);
            return false;
        }
    }
    
    if (node->type == AST_VARIABLE_DECLARATION || node->type == AST_VARIABLE_WITH_BODY) {
        if (node->variable_type->is_array) {
            semantic_report_error(context, node,
                                "Variable cannot be an array, use array declaration instead");
            return false;
        }
    }
    
    return true;
}

bool validate_array_declaration(SemanticContext* context, ASTNode* node) {
    if (!node || !node->variable_type) return false;
    
    const char* type_name = get_type_name(node->variable_type);
    
    if (strcmp(type_name, "Void") == 0) {
        semantic_report_error(context, node, "Array cannot have Void type");
        return false;
    }
    
    if (!node->variable_type->is_array) {
        semantic_report_error(context, node, "Array declaration must have array dimensions");
        return false;
    }
    
    return true;
}

bool validate_function_declaration(SemanticContext* context, ASTNode* node) {
    if (!node) return false;
    
    if (!node->variable_type) {
        semantic_report_error(context, node, "Function declaration requires explicit type");
        return false;
    }
    
    return true;
}

bool validate_struct_declaration(SemanticContext* context, ASTNode* node) {
    if (node->variable_type) {
        semantic_report_error(context, node, "Struct cannot have a type");
        return false;
    }
    
    return true;
}

bool validate_class_declaration(SemanticContext* context, ASTNode* node) {
    if (node->variable_type) {
        semantic_report_error(context, node, "Class cannot have a type");
        return false;
    }
    
    return true;
}

bool validate_object_declaration(SemanticContext* context, ASTNode* node) {
    if (!node || !node->variable_type) return false;
    
    const char* type_name = get_type_name(node->variable_type);
    
    if (strcmp(type_name, "Void") == 0) {
        semantic_report_error(context, node, "Object must have a non-Void type");
        return false;
    }
    
    Symbol* type_symbol = find_symbol_by_type_name(context, type_name);
    
    if (!type_symbol) {
        semantic_report_error(context, node, "Type '%s' not found for object", type_name);
        return false;
    }
    
    if (type_symbol->kind != OBJ_CLASS && 
        type_symbol->kind != OBJ_STRUCT && 
        type_symbol->kind != OBJ_OBJECT) {
        semantic_report_error(context, node, 
                            "Object type must be class, struct or object");
        return false;
    }
    
    return true;
}

bool are_types_compatible(Type* t1, Type* t2) {
    if (!t1 && !t2) return true;
    if (!t1 || !t2) return false;
    
    const char* name1 = get_type_name(t1);
    const char* name2 = get_type_name(t2);
    
    if (strcmp(name1, name2) != 0) {
        // Allow char array to accept strings
        if (t2->name && strcmp(t2->name, "Char") == 0 && t2->is_array)
            return true;
        return false;
    }
    
    if (t1->is_array != t2->is_array) return false;
    if (t1->is_array && t1->array_dimensions && t2->array_dimensions) {
        if (t1->array_dimensions->count != t2->array_dimensions->count)
            return false;
    }
    
    if (t1->pointer_level != t2->pointer_level) return false;
    if (t1->is_reference != t2->is_reference) return false;
    if (t1->is_register != t2->is_register) return false;
    
    return true;
}

bool is_string_assignment_valid(Type* target_type, ASTNode* string_node) {
    if (!target_type || !string_node || 
        string_node->operation_type != TOKEN_STRING) {
        return false;
    }
    
    if (!target_type->is_array || !target_type->name || 
        strcmp(target_type->name, "Char") != 0) {
        return false;
    }
    
    if (target_type->array_dimensions && 
        target_type->array_dimensions->count > 0) {
        ASTNode* dim_node = target_type->array_dimensions->nodes[0];
        if (dim_node->type == AST_LITERAL_VALUE && 
            dim_node->operation_type == TOKEN_NUMBER) {
            int array_size = atoi(dim_node->value);
            int str_len = strlen(string_node->value);
            
            if (str_len > array_size) {
                return false;
            }
        }
    }
    
    return true;
}

Symbol* create_symbol_from_ast_node(SemanticContext* context, ASTNode* node) {
    if (!node || !node->value) return NULL;
    
    Symbol* symbol = calloc(1, sizeof(Symbol));
    symbol->name = strdup(node->value);
    symbol->ast_node = node;
    
    if (node->state_modifier)
        symbol->kind = get_object_kind_from_state(node->state_modifier);
    else symbol->kind = OBJ_VARIABLE;
    
    if (node->state_modifier)
        symbol->is_mutable = (strcmp(node->state_modifier, "var") == 0);
    else symbol->is_mutable = true;
    
    symbol->type = node->variable_type;
    
    return symbol;
}

void validate_identifier_usage(SemanticContext* context, ASTNode* node) {
    if (!node || node->type != AST_IDENTIFIER || !node->value) return;
    
    Symbol* symbol = semantic_lookup_symbol(context, node->value);
    if (!symbol) {
        semantic_report_error(context, node, "Undefined identifier '%s'", node->value);
        return;
    }
    
    switch (symbol->kind) {
        case OBJ_VARIABLE:
            if (!symbol->is_mutable && node->parent && 
                (node->parent->type == AST_ASSIGNMENT || 
                 node->parent->type == AST_COMPOUND_ASSIGNMENT)) {
                semantic_report_error(context, node, 
                                    "Cannot modify immutable variable '%s' (declared with 'let')", 
                                    symbol->name);
            }
            break;
            
        case OBJ_ARRAY:
            if (node->parent && node->parent->type != AST_ARRAY_ACCESS) {
                semantic_report_error(context, node, 
                                    "Array '%s' must be accessed with indices", 
                                    symbol->name);
            }
            break;
            
        case OBJ_FUNCTION:
            if (node->parent && node->parent->type != AST_FUNCTION_CALL) {
                semantic_report_error(context, node, 
                                    "Function '%s' must be called with arguments", 
                                    symbol->name);
            }
            break;
    }
}

void analyze_expression(SemanticContext* context, ASTNode* node) {
    if (!node) return;
    
    switch (node->type) {
        case AST_IDENTIFIER:
            validate_identifier_usage(context, node);
            break;
            
        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT: {
            analyze_expression(context, node->left);
            analyze_expression(context, node->right);
            
            if (node->left->variable_type && node->right->variable_type) {
                if (!are_types_compatible(node->left->variable_type, 
                                          node->right->variable_type)) {
                    if (node->right->type == AST_LITERAL_VALUE && 
                        node->right->operation_type == TOKEN_STRING) {
                        if (!is_string_assignment_valid(node->left->variable_type, 
                                                       node->right)) {
                            semantic_report_error(context, node, 
                                                "String assignment invalid for target type");
                        }
                    } else {
                        semantic_report_error(context, node, "Type mismatch in assignment");
                    }
                }
            }
            break;
        }
            
        case AST_ARRAY_ACCESS:
            analyze_expression(context, node->left);
            
            if (node->extra) {
                AST* indices = (AST*)node->extra;
                for (uint16_t i = 0; i < indices->count; i++)
                    analyze_expression(context, indices->nodes[i]);
            }
            break;
            
        case AST_FIELD_ACCESS:
            analyze_expression(context, node->left);
            analyze_expression(context, node->right);
            
            if (node->operation_type == TOKEN_INDICATOR) {
                Symbol* base = semantic_lookup_symbol(context, node->left->value);
                if (base && base->kind != OBJ_STRUCT && base->kind != OBJ_OBJECT) {
                    semantic_report_error(context, node, 
                                        "Operator '->' can only be used with structs and objects");
                }
            } else if (node->operation_type == TOKEN_DOUBLE_COLON) {
                Symbol* base = semantic_lookup_symbol(context, node->left->value);
                if (base && base->kind != OBJ_CLASS) {
                    semantic_report_error(context, node, 
                                        "Operator '::' can only be used with classes");
                }
            }
            break;
            
        case AST_FUNCTION_CALL:
            analyze_expression(context, node->left);
            if (node->extra) {
                AST* args = (AST*)node->extra;
                for (uint16_t i = 0; i < args->count; i++)
                    analyze_expression(context, args->nodes[i]);
            }
            break;
            
        case AST_LITERAL_VALUE:
            break;
            
        default:
            analyze_expression(context, node->left);
            analyze_expression(context, node->right);
            analyze_expression(context, node->extra);
            break;
    }
}

void analyze_statement(SemanticContext* context, ASTNode* node);

void analyze_block(SemanticContext* context, ASTNode* node) {
    if (!node) return;
    
    semantic_enter_scope(context);
    
    if (node->extra) {
        AST* block = (AST*)node->extra;
        for (uint16_t i = 0; i < block->count; i++) {
            analyze_statement(context, block->nodes[i]);
        }
    }
    
    semantic_exit_scope(context);
}

void analyze_statement(SemanticContext* context, ASTNode* node) {
    if (!node) return;
    
    switch (node->type) {
        case AST_VARIABLE_DECLARATION:
        case AST_VARIABLE_WITH_BODY:
        case AST_ARRAY_DECLARATION:
        case AST_FUNCTION_DECLARATION:
        case AST_STRUCT_DECLARATION:
        case AST_CLASS_DECLARATION:
        case AST_OBJECT_DECLARATION:
            // These are handled separately in analyze_declaration
            break;
            
        case AST_BLOCK:
            analyze_block(context, node);
            break;
            
        case AST_RETURN:
            if (node->left) {
                analyze_expression(context, node->left);
            }
            break;
            
        case AST_IF_STATEMENT:
            analyze_expression(context, node->left);
            analyze_statement(context, node->right);
            if (node->extra)
                analyze_statement(context, node->extra);
            break;
            
        case AST_WHILE_STATEMENT:
        case AST_DO_WHILE_STATEMENT:
            analyze_expression(context, node->left);
            analyze_statement(context, node->right);
            break;
            
        case AST_FOR_STATEMENT:
            if (node->left) analyze_statement(context, node->left);
            if (node->right) analyze_expression(context, node->right);
            if (node->extra) analyze_statement(context, node->extra);
            break;
            
        default:
            analyze_expression(context, node);
            break;
    }
}

void analyze_declaration(SemanticContext* context, ASTNode* node) {
    if (!node || !node->value) return;
    
    Symbol* symbol = create_symbol_from_ast_node(context, node);
    if (!symbol) return;
    
    bool valid = true;
    switch (symbol->kind) {
        case OBJ_VARIABLE:
            if (node->type == AST_ARRAY_DECLARATION || 
                (node->variable_type && node->variable_type->is_array)) {
                symbol->kind = OBJ_ARRAY;
                valid = validate_array_declaration(context, node);
            } else valid = validate_variable_declaration(context, node);
            break;
            
        case OBJ_FUNCTION:
            valid = validate_function_declaration(context, node);
            if (valid && node->right) {
                // Enter function scope for parameters and body
                semantic_enter_scope(context);
                
                // Process function parameters
                if (node->extra) {
                    AST* params = (AST*)node->extra;
                    for (uint16_t i = 0; i < params->count; i++) {
                        ASTNode* param = params->nodes[i];
                        Symbol* param_symbol = create_symbol_from_ast_node(context, param);
                        if (param_symbol) {
                            param_symbol->kind = OBJ_VARIABLE;
                            semantic_insert_symbol(context, param_symbol);
                        }
                    }
                }
                
                // Analyze function body
                analyze_statement(context, node->right);
                
                semantic_exit_scope(context);
            }
            break;
            
        case OBJ_STRUCT:
            valid = validate_struct_declaration(context, node);
            if (valid && node->right) {
                // Struct members are in their own scope
                semantic_enter_scope(context);
                analyze_block(context, node->right);
                semantic_exit_scope(context);
            }
            break;
            
        case OBJ_CLASS:
            valid = validate_class_declaration(context, node);
            if (valid && node->right) {
                // Class members are in their own scope
                semantic_enter_scope(context);
                analyze_block(context, node->right);
                semantic_exit_scope(context);
            }
            break;
            
        case OBJ_OBJECT:
            valid = validate_object_declaration(context, node);
            break;
    }
    
    if (!valid) {
        semantic_report_error(context, node, "Invalid declaration of '%s'", node->value);
        free(symbol->name);
        free(symbol);
        return;
    }
    
    if (!semantic_insert_symbol(context, symbol)) {
        free(symbol->name);
        free(symbol);
        return;
    }
    
    // Validate default value if present
    if (node->default_value) {
        analyze_expression(context, node->default_value);
    }
}

void semantic_analyze(AST* ast) {
    if (!ast) return;
    
    SemanticContext* context = semantic_create_context();
    
    // First pass: declarations
    for (uint16_t i = 0; i < ast->count; i++) {
        ASTNode* node = ast->nodes[i];
        
        if (node->type == AST_VARIABLE_DECLARATION ||
            node->type == AST_VARIABLE_WITH_BODY ||
            node->type == AST_ARRAY_DECLARATION ||
            node->type == AST_FUNCTION_DECLARATION ||
            node->type == AST_STRUCT_DECLARATION ||
            node->type == AST_CLASS_DECLARATION ||
            node->type == AST_OBJECT_DECLARATION) {
            analyze_declaration(context, node);
        }
    }
    
    // Second pass: statements
    for (uint16_t i = 0; i < ast->count; i++) {
        ASTNode* node = ast->nodes[i];
        
        if (node->type == AST_VARIABLE_DECLARATION ||
            node->type == AST_VARIABLE_WITH_BODY ||
            node->type == AST_ARRAY_DECLARATION ||
            node->type == AST_FUNCTION_DECLARATION ||
            node->type == AST_STRUCT_DECLARATION ||
            node->type == AST_CLASS_DECLARATION ||
            node->type == AST_OBJECT_DECLARATION) {
            continue;
        }
        
        analyze_statement(context, node);
    }
    
    if (context->has_errors) {
        fprintf(stderr, "Semantic analysis failed with %d error(s)\n", context->error_count);
    } else {
        fprintf(stderr, "Semantic analysis completed successfully\n");
    }
    
    semantic_destroy_context(context);
}
