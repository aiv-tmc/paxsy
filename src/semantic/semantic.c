#include "semantic.h"
#include "../errhandler/errhandler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* Hash table size (power of two for efficient modulo) */
#define HASH_TABLE_SIZE 64
#define INITIAL_SCOPE_CAPACITY 16

/**
 * Duplicate a string (strdup replacement for C99)
 * @param str String to duplicate
 * @return Duplicated string or NULL on allocation failure
 */
static char* duplicate_string(const char* str) {
    if (!str) return NULL;
    
    size_t len = strlen(str);
    char* new_str = malloc(len + 1);
    if (!new_str) return NULL;
    
    memcpy(new_str, str, len);
    new_str[len] = '\0';
    return new_str;
}

/**
 * Hash function for symbol names (FNV-1a variant)
 * @param str String to hash
 * @return Hash value
 */
static uint32_t hash_string(const char* str) {
    uint32_t hash = 5381;
    int c;
    
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    
    return hash;
}

/**
 * Create a new symbol table
 * @param parent Parent symbol table (NULL for global scope)
 * @return New symbol table or NULL on allocation failure
 */
static SymbolTable* create_symbol_table(SymbolTable* parent) {
    SymbolTable* table = malloc(sizeof(SymbolTable));
    if (!table) return NULL;
    
    table->entries = calloc(HASH_TABLE_SIZE, sizeof(SymbolEntry*));
    if (!table->entries) {
        free(table);
        return NULL;
    }
    
    table->capacity = HASH_TABLE_SIZE;
    table->count = 0;
    table->parent = parent;
    
    return table;
}

/**
 * Destroy symbol table and free all memory
 * @param table Symbol table to destroy
 */
static void destroy_symbol_table(SymbolTable* table) {
    if (!table) return;
    
    for (size_t i = 0; i < table->capacity; i++) {
        SymbolEntry* entry = table->entries[i];
        while (entry) {
            SymbolEntry* next = entry->next;
            
            /* Free function signature if it's a function */
            if (entry->type == TYPE_FUNCTION && entry->extra.func_sig) {
                FunctionParam* param = entry->extra.func_sig->params;
                while (param) {
                    FunctionParam* next_param = param->next;
                    free(param->name);
                    free(param);
                    param = next_param;
                }
                free(entry->extra.func_sig);
            }
            
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    
    free(table->entries);
    free(table);
}

/**
 * Create a new semantic analysis context
 * @return New context or NULL on allocation failure
 */
SemanticContext* semantic_create_context(void) {
    SemanticContext* context = malloc(sizeof(SemanticContext));
    if (!context) return NULL;
    
    context->global_scope = create_symbol_table(NULL);
    if (!context->global_scope) {
        free(context);
        return NULL;
    }
    
    context->current_scope = context->global_scope;
    context->has_errors = false;
    context->warnings_enabled = true;
    context->exit_on_error = true; /* Default to exit on error */
    
    return context;
}

/**
 * Destroy semantic analysis context and free all memory
 * @param context Context to destroy
 */
void semantic_destroy_context(SemanticContext* context) {
    if (!context) return;
    
    destroy_symbol_table(context->global_scope);
    free(context);
}

/**
 * Set whether to exit compilation on semantic error
 * @param context Semantic context
 * @param exit_on_error true to exit on error
 */
void semantic_set_exit_on_error(SemanticContext* context, bool exit_on_error) {
    if (context) {
        context->exit_on_error = exit_on_error;
    }
}

/**
 * Enter a new scope (e.g., function body, block)
 * @param context Current semantic context
 */
void semantic_enter_scope(SemanticContext* context) {
    SymbolTable* new_scope = create_symbol_table(context->current_scope);
    if (new_scope) {
        context->current_scope = new_scope;
    }
}

/**
 * Exit current scope and return to parent scope
 * @param context Current semantic context
 */
void semantic_exit_scope(SemanticContext* context) {
    if (context->current_scope && context->current_scope != context->global_scope) {
        SymbolTable* parent = context->current_scope->parent;
        destroy_symbol_table(context->current_scope);
        context->current_scope = parent;
    }
}

/**
 * Add a variable symbol to the current scope
 * @param context Semantic context
 * @param name Symbol name
 * @param type Data type
 * @param type_info Detailed type information
 * @param is_constant Whether symbol is constant
 * @param line Declaration line number
 * @param column Declaration column number
 * @return true if added successfully, false on error or redeclaration
 */
bool semantic_add_variable(SemanticContext* context, const char* name,
                          DataType type, Type* type_info, bool is_constant,
                          uint16_t line, uint16_t column) {
    if (!context || !name) return false;
    
    /* Check for redeclaration in current scope */
    SymbolEntry* existing = semantic_find_symbol(context, name);
    if (existing) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_REDECLARATION,
            line, column, (uint8_t)strlen(name),
            "semantic",
            "Redeclaration of symbol '%s' (previous declaration at line %d)",
            name, existing->line
        );
        context->has_errors = true;
        return false;
    }
    
    /* Create new symbol entry */
    SymbolEntry* entry = malloc(sizeof(SymbolEntry));
    if (!entry) return false;
    
    entry->name = duplicate_string(name);
    entry->type = type;
    entry->type_info = type_info;
    entry->is_constant = is_constant;
    entry->is_initialized = false;
    entry->is_used = false;
    entry->line = line;
    entry->column = column;
    entry->extra.func_sig = NULL;
    
    /* Add to hash table */
    uint32_t index = hash_string(name) % context->current_scope->capacity;
    entry->next = context->current_scope->entries[index];
    context->current_scope->entries[index] = entry;
    context->current_scope->count++;
    
    return true;
}

/**
 * Add a function symbol to the current scope
 * @param context Semantic context
 * @param name Function name
 * @param return_type Return type
 * @param return_type_info Detailed return type information
 * @param params Function parameters
 * @param param_count Number of parameters
 * @param line Declaration line number
 * @param column Declaration column number
 * @return true if added successfully, false on error or redeclaration
 */
bool semantic_add_function(SemanticContext* context, const char* name,
                          DataType return_type, Type* return_type_info,
                          FunctionParam* params, size_t param_count,
                          uint16_t line, uint16_t column) {
    if (!context || !name) return false;
    
    /* Check for redeclaration in current scope */
    SymbolEntry* existing = semantic_find_symbol(context, name);
    if (existing) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_REDECLARATION,
            line, column, (uint8_t)strlen(name),
            "semantic",
            "Redeclaration of function '%s' (previous declaration at line %d)",
            name, existing->line
        );
        context->has_errors = true;
        return false;
    }
    
    /* Create function signature */
    FunctionSignature* sig = malloc(sizeof(FunctionSignature));
    if (!sig) return false;
    
    sig->return_type = return_type;
    sig->return_type_info = return_type_info;
    sig->params = params;
    sig->param_count = param_count;
    
    /* Create new symbol entry */
    SymbolEntry* entry = malloc(sizeof(SymbolEntry));
    if (!entry) {
        free(sig);
        return false;
    }
    
    entry->name = duplicate_string(name);
    entry->type = TYPE_FUNCTION;
    entry->type_info = return_type_info;
    entry->is_constant = false;
    entry->is_initialized = true; /* Functions are always initialized */
    entry->is_used = false;
    entry->line = line;
    entry->column = column;
    entry->extra.func_sig = sig;
    
    /* Add to hash table */
    uint32_t index = hash_string(name) % context->current_scope->capacity;
    entry->next = context->current_scope->entries[index];
    context->current_scope->entries[index] = entry;
    context->current_scope->count++;
    
    return true;
}

/**
 * Find a symbol in current and parent scopes
 * @param context Semantic context
 * @param name Symbol name to find
 * @return Symbol entry or NULL if not found
 */
SymbolEntry* semantic_find_symbol(SemanticContext* context, const char* name) {
    if (!context || !name) return NULL;
    
    SymbolTable* table = context->current_scope;
    
    while (table) {
        uint32_t index = hash_string(name) % table->capacity;
        SymbolEntry* entry = table->entries[index];
        
        while (entry) {
            if (strcmp(entry->name, name) == 0) {
                return entry;
            }
            entry = entry->next;
        }
        
        table = table->parent;
    }
    
    return NULL;
}

/**
 * Mark a symbol as used (for unused variable detection)
 * @param context Semantic context
 * @param name Symbol name
 * @return true if symbol was found and marked, false otherwise
 */
bool semantic_mark_symbol_used(SemanticContext* context, const char* name) {
    SymbolEntry* entry = semantic_find_symbol(context, name);
    if (entry) {
        entry->is_used = true;
        return true;
    }
    
    /* Report undeclared symbol error */
    errhandler__report_error_ex(
        ERROR_LEVEL_ERROR,
        ERROR_CODE_SEM_UNDECLARED_SYMBOL,
        0, 0, (uint8_t)strlen(name),
        "semantic",
        "Use of undeclared symbol '%s'",
        name
    );
    context->has_errors = true;
    
    return false;
}

/**
 * Mark a symbol as initialized
 * @param context Semantic context
 * @param name Symbol name
 * @return true if symbol was found and marked, false otherwise
 */
bool semantic_mark_symbol_initialized(SemanticContext* context, const char* name) {
    SymbolEntry* entry = semantic_find_symbol(context, name);
    if (entry) {
        entry->is_initialized = true;
        return true;
    }
    return false;
}

/**
 * Convert token type to data type
 * @param token_type Lexer token type
 * @return Corresponding data type
 */
DataType semantic_type_from_token(TokenType token_type) {
    switch (token_type) {
        case TOKEN_NUMBER:
            return TYPE_REAL; /* Default to real for numbers */
        case TOKEN_CHAR:
            return TYPE_CHAR;
        case TOKEN_STRING:
            return TYPE_STRING;
        case TOKEN_NULL:
            return TYPE_NONE;
        case TOKEN_NONE:
            return TYPE_NONE;
        default:
            return TYPE_UNKNOWN;
    }
}

/**
 * Convert type name string to data type
 * @param type_name Type name string
 * @return Corresponding data type
 */
DataType semantic_type_from_string(const char* type_name) {
    if (!type_name) return TYPE_UNKNOWN;
    
    if (strcmp(type_name, "Int") == 0) return TYPE_INT;
    if (strcmp(type_name, "Real") == 0) return TYPE_REAL;
    if (strcmp(type_name, "Char") == 0) return TYPE_CHAR;
    if (strcmp(type_name, "String") == 0) return TYPE_STRING;
    if (strcmp(type_name, "Bool") == 0) return TYPE_BOOL;
    if (strcmp(type_name, "Void") == 0) return TYPE_VOID;
    if (strcmp(type_name, "none") == 0) return TYPE_NONE;
    
    return TYPE_UNKNOWN;
}

/**
 * Convert data type to string representation
 * @param type Data type
 * @return String representation
 */
const char* semantic_type_to_string(DataType type) {
    switch (type) {
        case TYPE_INT: return "Int";
        case TYPE_REAL: return "Real";
        case TYPE_CHAR: return "Char";
        case TYPE_STRING: return "String";
        case TYPE_BOOL: return "Bool";
        case TYPE_VOID: return "Void";
        case TYPE_NONE: return "none";
        case TYPE_POINTER: return "pointer";
        case TYPE_REFERENCE: return "reference";
        case TYPE_ARRAY: return "array";
        case TYPE_FUNCTION: return "function";
        case TYPE_COMPOUND: return "compound";
        default: return "unknown";
    }
}

/**
 * Check if two types are compatible for operations
 * @param type1 First type
 * @param type2 Second type
 * @return true if types are compatible, false otherwise
 */
bool semantic_types_compatible(DataType type1, DataType type2) {
    /* Same types are always compatible */
    if (type1 == type2) return true;
    
    /* Int and Real are compatible (with promotion to Real) */
    if ((type1 == TYPE_INT && type2 == TYPE_REAL) ||
        (type1 == TYPE_REAL && type2 == TYPE_INT)) {
        return true;
    }
    
    /* none is compatible with pointers and references */
    if (type1 == TYPE_NONE) {
        return (type2 == TYPE_POINTER || type2 == TYPE_REFERENCE);
    }
    if (type2 == TYPE_NONE) {
        return (type1 == TYPE_POINTER || type1 == TYPE_REFERENCE);
    }
    
    return false;
}

/**
 * Check if type can be assigned to another type
 * @param target_type Target variable type
 * @param source_type Source expression type
 * @return true if assignment is valid, false otherwise
 */
bool semantic_types_assignable(DataType target_type, DataType source_type) {
    /* Can assign same type */
    if (target_type == source_type) return true;
    
    /* Can assign Int to Real (implicit conversion) */
    if (target_type == TYPE_REAL && source_type == TYPE_INT) return true;
    
    /* Can assign none to pointer/reference */
    if (source_type == TYPE_NONE &&
        (target_type == TYPE_POINTER || target_type == TYPE_REFERENCE)) {
        return true;
    }
    
    return false;
}

/**
 * Check type of AST expression node
 * @param context Semantic context
 * @param node AST node to check
 * @return Type check result
 */
TypeCheckResult semantic_check_type(SemanticContext* context, ASTNode* node) {
    TypeCheckResult result = {false, TYPE_UNKNOWN, NULL, NULL};
    
    if (!node) {
        result.error_msg = duplicate_string("Null node");
        return result;
    }
    
    switch (node->type) {
        case AST_LITERAL_VALUE: {
            result.type = semantic_type_from_token(node->operation_type);
            result.valid = (result.type != TYPE_UNKNOWN);
            if (!result.valid) {
                result.error_msg = duplicate_string("Invalid literal type");
            }
            break;
        }
        
        case AST_IDENTIFIER: {
            if (node->value) {
                SymbolEntry* entry = semantic_find_symbol(context, node->value);
                if (entry) {
                    semantic_mark_symbol_used(context, node->value);
                    result.type = entry->type;
                    result.type_info = entry->type_info;
                    result.valid = true;
                    
                    /* Warn about uninitialized variable */
                    if (!entry->is_initialized && context->warnings_enabled &&
                        entry->type != TYPE_FUNCTION) {
                        errhandler__report_error_ex(
                            ERROR_LEVEL_WARNING,
                            ERROR_CODE_SEM_UNINITIALIZED,
                            0, 0, (uint8_t)strlen(node->value),
                            "semantic",
                            "Use of possibly uninitialized variable '%s'",
                            node->value
                        );
                    }
                } else {
                    errhandler__report_error_ex(
                        ERROR_LEVEL_ERROR,
                        ERROR_CODE_SEM_UNDECLARED_SYMBOL,
                        0, 0, (uint8_t)strlen(node->value),
                        "semantic",
                        "Undeclared identifier '%s'",
                        node->value
                    );
                    result.error_msg = duplicate_string("Undeclared identifier");
                    context->has_errors = true;
                }
            }
            break;
        }
        
        case AST_BINARY_OPERATION:
            result = semantic_check_binary_op(context, node);
            break;
            
        case AST_UNARY_OPERATION:
            result = semantic_check_unary_op(context, node);
            break;
            
        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
            result = semantic_check_assignment(context, node);
            break;
            
        case AST_FUNCTION_DECLARATION:
            result.type = TYPE_FUNCTION;
            result.valid = true;
            break;
            
        case AST_CAST: {
            if (node->variable_type) {
                result.type = semantic_type_from_string(node->variable_type->name);
                result.type_info = node->variable_type;
                
                /* Check cast expression type */
                TypeCheckResult expr_result = semantic_check_type(context, node->left);
                result.valid = expr_result.valid && 
                              semantic_types_compatible(result.type, expr_result.type);
                
                if (!result.valid) {
                    result.error_msg = duplicate_string("Invalid cast");
                }
            } else {
                result.error_msg = duplicate_string("Cast without target type");
            }
            break;
        }
        
        default:
            result.valid = true; /* Assume valid for unknown types */
            break;
    }
    
    return result;
}

/**
 * Check type of binary operation
 * @param context Semantic context
 * @param node Binary operation AST node
 * @return Type check result
 */
TypeCheckResult semantic_check_binary_op(SemanticContext* context, ASTNode* node) {
    TypeCheckResult result = {false, TYPE_UNKNOWN, NULL, NULL};
    
    if (!node->left || !node->right) {
        result.error_msg = duplicate_string("Binary operation missing operands");
        return result;
    }
    
    TypeCheckResult left_result = semantic_check_type(context, node->left);
    TypeCheckResult right_result = semantic_check_type(context, node->right);
    
    if (!left_result.valid || !right_result.valid) {
        result.error_msg = duplicate_string("Invalid operand type");
        return result;
    }
    
    /* Check operand type compatibility */
    if (!semantic_types_compatible(left_result.type, right_result.type)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                "Type mismatch in binary operation: %s and %s",
                semantic_type_to_string(left_result.type),
                semantic_type_to_string(right_result.type));
        
        result.error_msg = duplicate_string(msg);
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, 0,
            "semantic",
            "%s", msg
        );
        context->has_errors = true;
        return result;
    }
    
    /* Determine result type based on operation */
    switch (node->operation_type) {
        case TOKEN_PLUS:
        case TOKEN_MINUS:
        case TOKEN_STAR:
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
            /* Arithmetic operations */
            if (left_result.type == TYPE_STRING || right_result.type == TYPE_STRING) {
                /* Strings only support concatenation with + */
                if (node->operation_type == TOKEN_PLUS) {
                    result.type = TYPE_STRING;
                    result.valid = true;
                } else {
                    result.error_msg = duplicate_string("Invalid operation for string type");
                }
            } else if (left_result.type == TYPE_INT && right_result.type == TYPE_INT) {
                result.type = TYPE_INT;
                result.valid = true;
            } else {
                result.type = TYPE_REAL; /* Mixed or real operands */
                result.valid = true;
            }
            break;
            
        case TOKEN_DOUBLE_EQ:
        case TOKEN_NE:
        case TOKEN_LT:
        case TOKEN_GT:
        case TOKEN_LE:
        case TOKEN_GE:
            /* Comparison operations return boolean */
            result.type = TYPE_BOOL;
            result.valid = true;
            break;
            
        case TOKEN_LOGICAL:
            /* Logical operations require boolean operands */
            if (left_result.type == TYPE_BOOL && right_result.type == TYPE_BOOL) {
                result.type = TYPE_BOOL;
                result.valid = true;
            } else {
                result.error_msg = duplicate_string("Logical operations require boolean operands");
            }
            break;
            
        case TOKEN_PIPE:
        case TOKEN_AMPERSAND:
        case TOKEN_CARET:
        case TOKEN_SHL:
        case TOKEN_SHR:
            /* Bitwise operations require integer operands */
            if (left_result.type == TYPE_INT && right_result.type == TYPE_INT) {
                result.type = TYPE_INT;
                result.valid = true;
            } else {
                result.error_msg = duplicate_string("Bitwise operations require integer operands");
            }
            break;
            
        default:
            result.type = left_result.type; /* Default to left operand type */
            result.valid = true;
            break;
    }
    
    return result;
}

/**
 * Check type of unary operation
 * @param context Semantic context
 * @param node Unary operation AST node
 * @return Type check result
 */
TypeCheckResult semantic_check_unary_op(SemanticContext* context, ASTNode* node) {
    TypeCheckResult result = {false, TYPE_UNKNOWN, NULL, NULL};
    
    if (!node->right) { /* Unary operations use right as operand */
        result.error_msg = duplicate_string("Unary operation missing operand");
        return result;
    }
    
    TypeCheckResult operand_result = semantic_check_type(context, node->right);
    
    if (!operand_result.valid) {
        result.error_msg = duplicate_string("Invalid operand type");
        return result;
    }
    
    switch (node->operation_type) {
        case TOKEN_PLUS:
        case TOKEN_MINUS:
            /* Unary + and - for numbers */
            if (operand_result.type == TYPE_INT || operand_result.type == TYPE_REAL) {
                result.type = operand_result.type;
                result.valid = true;
            } else {
                result.error_msg = duplicate_string("Unary +/- requires numeric operand");
            }
            break;
            
        case TOKEN_BANG:
            /* Logical negation */
            if (operand_result.type == TYPE_BOOL) {
                result.type = TYPE_BOOL;
                result.valid = true;
            } else {
                result.error_msg = duplicate_string("Logical NOT requires boolean operand");
            }
            break;
            
        case TOKEN_TILDE:
            /* Bitwise negation */
            if (operand_result.type == TYPE_INT) {
                result.type = TYPE_INT;
                result.valid = true;
            } else {
                result.error_msg = duplicate_string("Bitwise NOT requires integer operand");
            }
            break;
            
        default:
            result.error_msg = duplicate_string("Unknown unary operation");
            break;
    }
    
    return result;
}

/**
 * Check type of assignment operation
 * @param context Semantic context
 * @param node Assignment AST node
 * @return Type check result
 */
TypeCheckResult semantic_check_assignment(SemanticContext* context, ASTNode* node) {
    TypeCheckResult result = {false, TYPE_UNKNOWN, NULL, NULL};
    
    if (!node->left || !node->right) {
        result.error_msg = duplicate_string("Assignment missing operands");
        return result;
    }
    
    /* Left side must be an l-value (identifier) */
    if (node->left->type != AST_IDENTIFIER) {
        result.error_msg = duplicate_string("Left side of assignment must be an identifier");
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, 0,
            "semantic",
            "Left side of assignment must be an identifier"
        );
        context->has_errors = true;
        return result;
    }
    
    const char* var_name = node->left->value;
    SymbolEntry* entry = semantic_find_symbol(context, var_name);
    
    if (!entry) {
        result.error_msg = duplicate_string("Assignment to undeclared variable");
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_UNDECLARED_SYMBOL,
            0, 0, (uint8_t)strlen(var_name),
            "semantic",
            "Assignment to undeclared variable '%s'",
            var_name
        );
        context->has_errors = true;
        return result;
    }
    
    /* Warn about assignment to constant */
    if (entry->is_constant && context->warnings_enabled) {
        errhandler__report_error_ex(
            ERROR_LEVEL_WARNING,
            ERROR_CODE_SEM_ASSIGN_TO_CONST,
            0, 0, (uint8_t)strlen(var_name),
            "semantic",
            "Assignment to constant variable '%s'",
            var_name
        );
    }
    
    /* Check right-hand side type */
    TypeCheckResult right_result = semantic_check_type(context, node->right);
    
    if (!right_result.valid) {
        result.error_msg = duplicate_string("Invalid right-hand side type");
        return result;
    }
    
    /* Check type compatibility */
    if (!semantic_types_assignable(entry->type, right_result.type)) {
        char error_msg[256];
        snprintf(error_msg, sizeof(error_msg),
                "Type mismatch in assignment: cannot assign %s to %s",
                semantic_type_to_string(right_result.type),
                semantic_type_to_string(entry->type));
        
        result.error_msg = duplicate_string(error_msg);
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, (uint8_t)strlen(var_name),
            "semantic",
            "%s", error_msg
        );
        context->has_errors = true;
        return result;
    }
    
    /* Mark variable as initialized */
    semantic_mark_symbol_initialized(context, var_name);
    
    result.type = entry->type;
    result.type_info = entry->type_info;
    result.valid = true;
    
    return result;
}

/**
 * Check type of function call
 * @param context Semantic context
 * @param node Function call AST node
 * @return Type check result
 */
TypeCheckResult semantic_check_function_call(SemanticContext* context,
                                             ASTNode* node) {
    TypeCheckResult result = {false, TYPE_UNKNOWN, NULL, NULL};
    
    /* TODO: Implement function call type checking */
    result.valid = true; /* Placeholder */
    result.type = TYPE_UNKNOWN;
    
    return result;
}

/**
 * Check semantic validity of an expression
 * @param context Semantic context
 * @param node Expression AST node
 * @return true if expression is valid, false otherwise
 */
bool semantic_check_expression(SemanticContext* context, ASTNode* node) {
    if (!node) return true;
    
    TypeCheckResult result = semantic_check_type(context, node);
    
    if (!result.valid && result.error_msg) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            ERROR_CODE_SEM_TYPE_ERROR,
            0, 0, 0,
            "semantic",
            "Type error: %s",
            result.error_msg
        );
        context->has_errors = true;
        return false;
    }
    
    return true;
}

/**
 * Check function declaration
 * @param context Semantic context
 * @param node Function declaration AST node
 * @return true if declaration is valid, false otherwise
 */
static bool check_function_declaration(SemanticContext* context, ASTNode* node) {
    if (!node || !node->value) return false;
    
    DataType return_type = TYPE_VOID;
    Type* return_type_info = node->variable_type;
    
    if (return_type_info && return_type_info->name) {
        return_type = semantic_type_from_string(return_type_info->name);
    }
    
    /* TODO: Extract parameters from AST */
    FunctionParam* params = NULL;
    size_t param_count = 0;
    
    /* Add function to symbol table */
    bool added = semantic_add_function(context, node->value, return_type,
                                      return_type_info, params, param_count,
                                      0, 0); /* TODO: Add line/column info */
    
    if (added && node->right) {
        /* Enter function scope and check function body */
        semantic_enter_scope(context);
        
        /* TODO: Add parameters to function scope */
        
        /* Check function body */
        semantic_check_statement(context, node->right);
        
        semantic_exit_scope(context);
    }
    
    return added;
}

/**
 * Check variable declaration
 * @param context Semantic context
 * @param node Variable declaration AST node
 * @return true if declaration is valid, false otherwise
 */
static bool check_variable_declaration(SemanticContext* context, ASTNode* node) {
    if (!node || !node->value) return false;
    
    DataType type = TYPE_UNKNOWN;
    Type* type_info = node->variable_type;
    
    /* Determine type from explicit annotation or default value */
    if (type_info && type_info->name) {
        type = semantic_type_from_string(type_info->name);
    } else if (node->default_value) {
        TypeCheckResult default_result = semantic_check_type(context, node->default_value);
        type = default_result.type;
    }
    
    /* Default to Int if type cannot be determined */
    if (type == TYPE_UNKNOWN) {
        type = TYPE_INT;
    }
    
    /* Add to symbol table */
    bool is_constant = (node->state_modifier && 
                       (strcmp(node->state_modifier, "const") == 0));
    
    bool added = semantic_add_variable(context, node->value, type, type_info,
                                      is_constant, 0, 0); /* TODO: Add line/column info */
    
    if (added && node->default_value) {
        /* Check default value expression */
        if (!semantic_check_expression(context, node->default_value)) {
            return false;
        }
        semantic_mark_symbol_initialized(context, node->value);
    }
    
    return added;
}

/**
 * Check semantic validity of a statement
 * @param context Semantic context
 * @param node Statement AST node
 * @return true if statement is valid, false otherwise
 */
bool semantic_check_statement(SemanticContext* context, ASTNode* node) {
    if (!node) return true;
    
    switch (node->type) {
        case AST_VARIABLE_DECLARATION:
            return check_variable_declaration(context, node);
            
        case AST_FUNCTION_DECLARATION:
            return check_function_declaration(context, node);
            
        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
            return semantic_check_expression(context, node);
            
        case AST_IF_STATEMENT:
            /* Check condition */
            if (!semantic_check_expression(context, node->left)) {
                return false;
            }
            /* Check then branch */
            if (node->right && !semantic_check_statement(context, node->right)) {
                return false;
            }
            /* Check else branch */
            if (node->extra && !semantic_check_statement(context, node->extra)) {
                return false;
            }
            return true;
            
        case AST_BLOCK: {
            semantic_enter_scope(context);
            
            if (node->extra) {
                AST* block_ast = (AST*)node->extra;
                for (uint16_t i = 0; i < block_ast->count; i++) {
                    if (!semantic_check_statement(context, block_ast->nodes[i])) {
                        semantic_exit_scope(context);
                        return false;
                    }
                }
            }
            
            semantic_exit_scope(context);
            return true;
        }
            
        case AST_RETURN:
            if (node->left) {
                return semantic_check_expression(context, node->left);
            }
            return true;
            
        default:
            /* For other statements, check expressions if any */
            return semantic_check_expression(context, node);
    }
}

/**
 * Perform semantic analysis on entire AST
 * @param context Semantic context
 * @param ast Abstract syntax tree
 * @return true if analysis passed without errors, false otherwise
 */
bool semantic_analyze(SemanticContext* context, AST* ast) {
    if (!context || !ast || !ast->nodes) {
        return false;
    }
    
    context->has_errors = false;
    
    /* Analyze all statements */
    for (uint16_t i = 0; i < ast->count; i++) {
        if (!semantic_check_statement(context, ast->nodes[i])) {
            context->has_errors = true;
            /* Exit early if configured to exit on error */
            if (context->exit_on_error) {
                fprintf(stderr, "Semantic analysis failed with errors. Compilation terminated.\n");
                exit(1);
            }
        }
    }
    
    /* Detect unused variables */
    if (context->warnings_enabled && !context->has_errors) {
        SymbolTable* table = context->global_scope;
        
        for (size_t i = 0; i < table->capacity; i++) {
            SymbolEntry* entry = table->entries[i];
            while (entry) {
                if (!entry->is_used && !entry->is_constant && entry->type != TYPE_FUNCTION) {
                    errhandler__report_error_ex(
                        ERROR_LEVEL_WARNING,
                        ERROR_CODE_SEM_UNUSED_VARIABLE,
                        entry->line, entry->column, (uint8_t)strlen(entry->name),
                        "semantic",
                        "Unused variable '%s'",
                        entry->name
                    );
                }
                entry = entry->next;
            }
        }
    }
    
    /* Exit with error if semantic errors were found and exit_on_error is true */
    if (context->has_errors && context->exit_on_error) {
        fprintf(stderr, "Semantic analysis failed with errors. Compilation terminated.\n");
        exit(1);
    }
    
    return !context->has_errors;
}

/**
 * Get total number of symbols in global scope
 * @param context Semantic context
 * @return Number of symbols
 */
size_t semantic_get_symbol_count(SemanticContext* context) {
    if (!context || !context->global_scope) return 0;
    return context->global_scope->count;
}

/**
 * Get global symbol table for inspection
 * @param context Semantic context
 * @return Global symbol table
 */
SymbolTable* semantic_get_global_table(SemanticContext* context) {
    if (!context) return NULL;
    return context->global_scope;
}

/**
 * Check if semantic analysis encountered errors
 * @param context Semantic context
 * @return true if errors were found, false otherwise
 */
bool semantic_has_errors(SemanticContext* context) {
    return context ? context->has_errors : false;
}

/**
 * Check if warnings are enabled
 * @param context Semantic context
 * @return true if warnings are enabled, false otherwise
 */
bool semantic_warnings_enabled(SemanticContext* context) {
    return context ? context->warnings_enabled : false;
}
