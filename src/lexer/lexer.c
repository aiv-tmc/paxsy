#include "../utils/common.h"
#include "lexer.h"
#include "../parser/literals.h"
#include "../errhandler/errhandler.h"

#include <string.h>
#include <stdlib.h>

#define HASH_TABLE_SIZE 64   /**< Size of the symbol hash table (power of two) */
#define INITIAL_TOKEN_CAPACITY 8 /**< Initial capacity of the token array */

/**
 * @struct SymbolEntry
 * @brief Entry in the symbol hash table (linked list for collisions).
 */
typedef struct SymbolEntry {
    const char* symbol;          /**< Pointer to the symbol string (static storage) */
    TokenType token_type;         /**< Associated token type */
    uint8_t symbol_length;        /**< Length of the symbol (for quick comparison) */
    struct SymbolEntry* next;     /**< Next entry in the same hash bucket */
} SymbolEntry;

/** Hash table buckets (static, zero‑initialised) */
static SymbolEntry* symbol_table[HASH_TABLE_SIZE] = {0};

/** Contiguous memory block holding all symbol entries (allocated once) */
static SymbolEntry* symbol_block = NULL;

/**
 * @brief FNV‑1a hash function for a string of given length.
 * 
 * @param symbol_string Pointer to the string (not necessarily null‑terminated)
 * @param symbol_length Number of characters to hash
 * @return Hash value (modulo HASH_TABLE_SIZE)
 */
static inline uint8_t hash_symbol(const char* symbol_string, uint8_t symbol_length) {
    uint32_t hash_value = 2166136261u;          /* FNV offset basis */
    while (symbol_length--) {
        hash_value ^= (uint8_t)*symbol_string++; /* XOR with next byte */
        hash_value *= 16777619u;                 /* FNV prime */
    }
    return hash_value & (HASH_TABLE_SIZE - 1);   /* Modulo power of two */
}

/**
 * @brief Initialises the global symbol table.
 * 
 * Inserts all keywords, operators, and punctuation marks defined in the
 * static table `symbol_definitions`. The entries are allocated in a single
 * contiguous block to reduce fragmentation and simplify cleanup.
 * This function is called once via `lexer__init_lexer`.
 */
static void init_symbol_table(void) {
    static const struct {
        const char* symbol;   /**< Symbol string (literal, static storage) */
        TokenType token_type; /**< Corresponding token type */
    } symbol_definitions[] = {
        /* Keywords */
        {"if",          TOKEN_IF},
        {"else",        TOKEN_ELSE},
        {"nop",         TOKEN_NOP},
        {"halt",        TOKEN_HALT},
        {"jump",        TOKEN_JUMP},
        {"free",        TOKEN_FREE},
        {"sizeof",      TOKEN_SIZEOF},
        {"parseof",     TOKEN_PARSEOF},
        {"signal",      TOKEN_SIGNAL},
        {"alloc",       TOKEN_ALLOC},
        {"realloc",     TOKEN_REALLOC},
        {"push",        TOKEN_PUSH},
        {"pop",         TOKEN_POP},
        {"return",      TOKEN_RETURN},
        {"none",        TOKEN_NONE},
        {"null",        TOKEN_NULL},
        
        /* State keywords (all map to TOKEN_STATE) */
        {"func",        TOKEN_STATE},
        {"var",         TOKEN_STATE},
        {"obj",         TOKEN_STATE},
        {"struct",      TOKEN_STATE},
        {"class",       TOKEN_STATE},
        
        /* Type keywords */
        {"Int",         TOKEN_TYPE},
        {"Real",        TOKEN_TYPE},
        {"Char",        TOKEN_TYPE},
        {"Void",        TOKEN_TYPE},
        
        /* Access modifiers */
        {"public",      TOKEN_ACCMOD},
        {"protected",   TOKEN_ACCMOD},
        {"private",     TOKEN_ACCMOD},
        
        /* Modifier keywords */
        {"const",       TOKEN_MODIFIER},
        {"fixed",       TOKEN_MODIFIER},
        {"unsigned",    TOKEN_MODIFIER},
        {"signed",      TOKEN_MODIFIER},
        {"extern",      TOKEN_MODIFIER},
        {"static",      TOKEN_MODIFIER},
        {"volatile",    TOKEN_MODIFIER},
        {"regis",       TOKEN_MODIFIER},
        
        /* Logical operator keywords */
        {"or",          TOKEN_LOGICAL},
        {"and",         TOKEN_LOGICAL},
        
        /* Single-character operators */
        {"%",           TOKEN_PERCENT},
        {":",           TOKEN_COLON},
        {".",           TOKEN_DOT},
        {";",           TOKEN_SEMICOLON},
        {"=",           TOKEN_EQUAL},
        {",",           TOKEN_COMMA},
        {"+",           TOKEN_PLUS},
        {"-",           TOKEN_MINUS},
        {"*",           TOKEN_STAR},
        {"/",           TOKEN_SLASH},
        {"?",           TOKEN_QUESTION},
        {"~",           TOKEN_TILDE},
        {"|",           TOKEN_PIPE},
        {"&",           TOKEN_AMPERSAND},
        {"!",           TOKEN_BANG},
        {"!~",          TOKEN_NE_TILDE},
        {"^",           TOKEN_CARET},
        {"@",           TOKEN_AT},
        {">",           TOKEN_GT},
        {"<",           TOKEN_LT},
        {">>",          TOKEN_SHR},
        {"<<",          TOKEN_SHL},
        {">>>",         TOKEN_SAR},
        {"<<<",         TOKEN_SAL},
        {">>>>",        TOKEN_ROR},
        {"<<<<",        TOKEN_ROL},
        {">=",          TOKEN_GE},
        {"<=",          TOKEN_LE},
        {"==",          TOKEN_DOUBLE_EQ},
        {"!=",          TOKEN_NE},
        {"+=",          TOKEN_PLUS_EQ},
        {"-=",          TOKEN_MINUS_EQ},
        {"*=",          TOKEN_STAR_EQ},
        {"/=",          TOKEN_SLASH_EQ},
        {"%=",          TOKEN_PERCENT_EQ},
        {"|=",          TOKEN_PIPE_EQ},
        {"&=",          TOKEN_AMPERSAND_EQ},
        {"^=",          TOKEN_CARET_EQ},
        {"<<=",         TOKEN_SHL_EQ},
        {">>=",         TOKEN_SHR_EQ},
        {"<<<=",        TOKEN_SAL_EQ},
        {">>>=",        TOKEN_SAR_EQ},
        {"<<<<=",       TOKEN_ROL_EQ},
        {">>>>=",       TOKEN_ROR_EQ},
        {"&&",          TOKEN_DOUBLE_AMPERSAND},
        {"@@",          TOKEN_DOUBLE_AT},
        {"++",          TOKEN_DOUBLE_PLUS},
        {"--",          TOKEN_DOUBLE_MINUS},
        {"->",          TOKEN_INDICATOR},
        {"::",          TOKEN_INDICATOR},
        {"=>",          TOKEN_THEN},
         
        /* Brackets */
        {"{",           TOKEN_LCURLY},
        {"}",           TOKEN_RCURLY},
        {"[",           TOKEN_LBRACE},
        {"]",           TOKEN_RBRACE},
        {"(",           TOKEN_LPAREN},
        {")",           TOKEN_RPAREN}
    };
    
    const size_t symbol_count = ARRAY_SIZE(symbol_definitions);
    
    /* Allocate all entries as one contiguous block */
    symbol_block = (SymbolEntry*)memory_allocate_zero(symbol_count * sizeof(SymbolEntry));
    if (!symbol_block) return;
    
    for (size_t index = 0; index < symbol_count; index++) {
        uint8_t symbol_length = (uint8_t)strlen(symbol_definitions[index].symbol);
        uint8_t hash_index = hash_symbol(symbol_definitions[index].symbol, symbol_length);
        
        SymbolEntry* entry = &symbol_block[index];
        entry->symbol = symbol_definitions[index].symbol;
        entry->token_type = symbol_definitions[index].token_type;
        entry->symbol_length = symbol_length;
        /* Insert at the beginning of the bucket list */
        entry->next = symbol_table[hash_index];
        symbol_table[hash_index] = entry;
    }
}

/**
 * @brief Looks up a string in the symbol table.
 * 
 * Compares the given string (of known length) against the symbols stored
 * in the hash table. Returns the associated token type if found, otherwise
 * returns TOKEN_ID (meaning it is a plain identifier).
 * 
 * @param symbol_string Pointer to the string to look up (not null‑terminated)
 * @param symbol_length Length of the string
 * @return TokenType (TOKEN_ID if not found)
 */
static inline TokenType lookup_symbol(const char* symbol_string, uint8_t symbol_length) {
    SymbolEntry* current_entry = symbol_table[hash_symbol(symbol_string, symbol_length)];
    
    while (current_entry) {
        if (current_entry->symbol_length == symbol_length &&
            memcmp(current_entry->symbol, symbol_string, symbol_length) == 0) {
            return current_entry->token_type;
        }
        current_entry = current_entry->next;
    }
    return TOKEN_ID;
}

/**
 * @brief Frees the global symbol table.
 * 
 * Releases the contiguous memory block and clears the hash table.
 * Registered with `atexit` to run automatically on program termination.
 */
static void free_symbol_table(void) {
    memory_free_safe((void**)&symbol_block);
    memory_set_safe(symbol_table, 0, sizeof(symbol_table));
}

/**
 * @brief Skips whitespace and newline characters, updating line/column counters.
 * 
 * @param lexer Lexer state to modify.
 */
static inline void skip_whitespace(Lexer* lexer) {
    const char* source = lexer->source;
    uint32_t pos = lexer->position;
    uint16_t line = lexer->line;
    uint16_t col = lexer->column;
    
    while (pos < lexer->source_length) {
        char current_char = source[pos];
        
        if (char_is_whitespace(current_char)) {
            pos++;
            col++;
        } else if (current_char == '\n') {
            pos++;
            line++;
            col = 1;
        } else break;
    }
    
    lexer->position = pos;
    lexer->line = line;
    lexer->column = col;
}

/**
 * @brief Attempts to recognise a multi‑character operator at the current position.
 * 
 * Tries the longest possible operator (up to 5 characters) by descending length.
 * If a match is found, `operator_length` is set to its length and the token type
 * is returned. Otherwise returns TOKEN_ERROR.
 * 
 * @param lexer Lexer state
 * @param operator_length Output parameter: length of the matched operator
 * @return TokenType of the operator, or TOKEN_ERROR if none matches
 */
static inline TokenType lookup_operator(Lexer* lexer, uint8_t* operator_length) {
    const char* current_position = lexer->source + lexer->position;
    uint32_t remaining_chars = lexer->source_length - lexer->position;
    
    if (remaining_chars > 0 && !char_is_operator_start(*current_position))
        return TOKEN_ERROR;
    
    /* Try lengths from 5 down to 1 */
    for (uint8_t test_length = 5; test_length > 0; test_length--) {
        if (remaining_chars >= test_length) {
            TokenType token_type = lookup_symbol(current_position, test_length);
            if (token_type != TOKEN_ID) {
                *operator_length = test_length;
                return token_type;
            }
        }
    }
    return TOKEN_ERROR;
}

/**
 * @brief Appends a new token to the lexer's token list.
 * 
 * The token's value is duplicated (with strdup‑like function) and stored.
 * The token array is automatically resized when full.
 * 
 * @param lexer Lexer state
 * @param token_type Type of the token
 * @param token_value Pointer to the token's text (may be NULL if length == 0)
 * @param value_length Length of the token text
 */
static void add_token_to_lexer(Lexer* lexer, TokenType token_type,
                              const char* token_value, uint32_t value_length) {
    if (is_null(lexer)) return;
    
    if (lexer->token_count >= UINT64_MAX) {
        errhandler__report_error(
            ERROR_CODE_MEMORY_OVERFLOW,
            lexer->line,
            lexer->column,
            "memory",
            "Too many tokens, maximum is 18446744073709551615"
        );
        return;
    }
    
    /* Resize token array if necessary */
    if (lexer->token_count >= lexer->token_capacity) {
        const uint16_t new_capacity = MIN(lexer->token_capacity * 2, UINT16_MAX);
        
        if (new_capacity <= lexer->token_capacity) {
            errhandler__report_error(
                ERROR_CODE_MEMORY_OVERFLOW,
                lexer->line,
                lexer->column,
                "inside",
                "Token array capacity overflow"
            );
            return;
        }
        
        Token* new_token_array = (Token*)memory_reallocate_zero(
            lexer->tokens,
            lexer->token_capacity * sizeof(Token),
            new_capacity * sizeof(Token)
        );
        
        if (is_null(new_token_array)) {
            errhandler__report_error(
                ERROR_CODE_MEMORY_ALLOCATION,
                lexer->line,
                lexer->column,
                "inside",
                "Memory allocation failed for token array"
            );
            return;
        }
        
        lexer->tokens = new_token_array;
        lexer->token_capacity = new_capacity;
    }
    
    Token* current_token = &lexer->tokens[lexer->token_count];
    memory_set_safe(current_token, 0, sizeof(Token));
    
    current_token->type = token_type;
    current_token->line = lexer->line;
    current_token->length = (uint16_t)value_length;
    
    /* Compute starting column (token may span multiple characters) */
    if (value_length > 0) {
        current_token->column = lexer->column > value_length ? 
                               lexer->column - value_length : 1;
    } else {
        current_token->column = lexer->column;
    }
    
    /* Copy token value (length‑limited) */
    if (value_length > 0 && !is_null(token_value)) {
        /* strdup but with explicit length (assumes null‑terminated source) */
        current_token->value = strdup(token_value, value_length);
        
        if (is_null(current_token->value)) {
            errhandler__report_error(
                ERROR_CODE_MEMORY_ALLOCATION,
                lexer->line,
                lexer->column,
                "inside",
                "Memory allocation failed for token value"
            );
            return;
        }
    }
    
    lexer->token_count++;
}

/**
 * @brief Initialises a new lexer.
 * 
 * Allocates the Lexer structure and its internal token array.
 * The global symbol table is initialised on the first call.
 * 
 * @param input Null‑terminated source string
 * @return Pointer to Lexer or NULL on failure
 */
Lexer* lexer__init_lexer(const char* input) {
    static uint8_t symbol_table_initialized = 0;
    
    if (!symbol_table_initialized) {
        init_symbol_table();
        symbol_table_initialized = 1;
        atexit(free_symbol_table);
    }
    
    if (is_null(input)) return NULL;
    
    Lexer* lexer = (Lexer*)memory_allocate_zero(sizeof(Lexer));
    if (is_null(lexer)) return NULL;
    
    lexer->source = input;
    lexer->source_length = (uint32_t)strlen(input);
    lexer->position = 0;
    lexer->line = 1;
    lexer->column = 1;
    lexer->token_count = 0;
    lexer->token_capacity = INITIAL_TOKEN_CAPACITY;
    
    lexer->tokens = (Token*)memory_allocate_zero(
        lexer->token_capacity * sizeof(Token)
    );
    
    if (is_null(lexer->tokens)) {
        memory_free_safe((void**)&lexer);
        return NULL;
    }
    
    return lexer;
}

/**
 * @brief Frees a lexer and all associated tokens.
 * 
 * @param lexer Lexer to free (may be NULL)
 */
void lexer__free_lexer(Lexer* lexer) {
    if (is_null(lexer)) return;
    
    if (!is_null(lexer->tokens)) {
        for (uint16_t i = 0; i < lexer->token_count; i++) {
            memory_free_safe((void**)&lexer->tokens[i].value);
        }
        memory_free_safe((void**)&lexer->tokens);
    }
    
    memory_free_safe((void**)&lexer);
}

/**
 * @brief Main tokenisation routine.
 * 
 * Scans the source character by character, recognising:
 * - Whitespace (skipped)
 * - String/character literals (delegated to literal__parse_concatenated)
 * - Operators (via lookup_operator)
 * - Identifiers / keywords (via lookup_symbol)
 * - Number literals (delegated to literal__parse_number)
 * - Unknown characters (reported as error)
 * 
 * Finally adds an EOF token.
 * 
 * @param lexer Lexer to process
 */
void lexer__tokenize(Lexer* lexer) {
    const char* source = lexer->source;
    
    while (lexer->position < lexer->source_length) {
        skip_whitespace(lexer);
        if (lexer->position >= lexer->source_length) break;
        
        char current_char = source[lexer->position];
        
        /* Handle string and character literals */
        if (current_char == '\'' || current_char == '"') {
            Token combined_token = literal__parse_concatenated(lexer);
            
            if (combined_token.type != TOKEN_ERROR) {
                add_token_to_lexer(
                    lexer,
                    combined_token.type,
                    combined_token.value,
                    combined_token.length
                );
                
                memory_free_safe((void**)&combined_token.value);
            }
            continue;
        }
        
        /* Try to recognise an operator */
        uint8_t operator_length;
        TokenType operator_type = lookup_operator(lexer, &operator_length);
        
        if (operator_type != TOKEN_ERROR) {
            add_token_to_lexer(
                lexer,
                operator_type,
                source + lexer->position,
                operator_length
            );
            
            lexer->position += operator_length;
            lexer->column += operator_length;
            continue;
        }
        
        /* Identifier or keyword */
        if (char_is_identifier_start(current_char)) {
            uint32_t identifier_start = lexer->position;
            
            while (lexer->position < lexer->source_length &&
                   char_is_identifier_char(source[lexer->position])) {
                lexer->position++;
                lexer->column++;
            }
            
            uint32_t identifier_length = lexer->position - identifier_start;
            const char* identifier_string = source + identifier_start;
            
            TokenType identifier_type = lookup_symbol(
                identifier_string,
                (uint8_t)identifier_length
            );
            
            add_token_to_lexer(
                lexer,
                identifier_type,
                identifier_string,
                identifier_length
            );
        } 
        /* Number literal (including leading +/‑ and '.' cases) */
        else if (char_is_digit(current_char) ||
                 (current_char == '.' && lexer->position + 1 < lexer->source_length &&
                  (char_is_digit(source[lexer->position + 1]) || 
                   source[lexer->position + 1] == '(')) ||
                 ((current_char == '-' || current_char == '+') &&
                  lexer->position + 1 < lexer->source_length &&
                  char_is_digit(source[lexer->position + 1]))) {
            Token number_token = literal__parse_number(lexer);
            add_token_to_lexer(
                lexer,
                number_token.type,
                number_token.value,
                number_token.length
            );
            
            memory_free_safe((void**)&number_token.value);
        } else {
            /* Unknown character – report error and skip it */
            errhandler__report_error(
                ERROR_CODE_LEXER_UNKNOWN_CHAR,
                lexer->line,
                lexer->column,
                "syntax",
                "Unexpected character in source code"
            );
            
            lexer->position++;
            lexer->column++;
        }
    }
    
    /* Mark end of input */
    add_token_to_lexer(lexer, TOKEN_EOF, "", 0);
}
