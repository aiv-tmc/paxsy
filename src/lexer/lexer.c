#include "lexer.h"
#include "../parser/literals.h"
#include "../errhandler/errhandler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define HASH_TABLE_SIZE 64   // Power of 2 for bitmask modulo
#define INITIAL_TOKEN_CAPACITY 8  // Initial token array size

/*
 * Hash table entry structure for symbol storage
 * Linked list for collision resolution
 */
typedef struct SymbolEntry {
    const char* symbol;       // Symbol string (static storage)
    TokenType token_type;     // Associated token type
    uint8_t symbol_length;    // Symbol length for quick comparison
    struct SymbolEntry* next; // Next entry in collision chain
} SymbolEntry;

/* Static hash table for keyword/operator lookup */
static SymbolEntry* symbol_table[HASH_TABLE_SIZE] = {0};

/* Pointer to the allocated symbol entries block */
static SymbolEntry* symbol_block = NULL;

/*
 * FNV-1a hash function for symbol strings
 * Inline for performance in hot loops
 * @param symbol_string: String to hash
 * @param symbol_length: Length of string to hash
 * @return: Hash value in range [0, HASH_TABLE_SIZE-1]
 */
static inline uint8_t hash_symbol
    ( const char* symbol_string
    , uint8_t symbol_length
    ) {
    uint32_t hash_value = 2166136261u;
    
    while (symbol_length--) {
        hash_value ^= (uint8_t)*symbol_string++;
        hash_value *= 16777619u;
    }
    
    return hash_value & (HASH_TABLE_SIZE - 1);
}

/*
 * Initialize static symbol hash table with keywords and operators
 * Uses single allocation for all entries to improve cache locality
 */
static void init_symbol_table(void) {
    /* 
     * Static symbol definitions
     * Compiler merges duplicate strings for memory efficiency
     */
    static const struct 
    {
        const char* symbol;
        TokenType token_type;
    } symbol_definitions[] =
    {
        /* Keywords */
        {"if",          TOKEN_IF                },
        {"else",        TOKEN_ELSE              },
        {"nop",         TOKEN_NOP               },
        {"halt",        TOKEN_HALT              },
        {"jump",        TOKEN_JUMP              },
        {"free",        TOKEN_FREE              },
        {"sizeof",      TOKEN_SIZEOF            },
        {"parseof",     TOKEN_PARSEOF           },
        {"inter",       TOKEN_INTER             },
        {"signal",      TOKEN_SIGNAL            },
        {"alloc",       TOKEN_ALLOC             },
        {"realloc",     TOKEN_REALLOC           },
        {"push",        TOKEN_PUSH              },
        {"pop",         TOKEN_POP               },
        {"return",      TOKEN_RETURN            },
        {"none",        TOKEN_NONE              },
        {"null",        TOKEN_NULL              },
        
        /* State keywords */
        {"func",        TOKEN_STATE             },
        {"var",         TOKEN_STATE             },
        {"obj",         TOKEN_STATE             },
        {"struct",      TOKEN_STATE             },
        {"class",       TOKEN_STATE             },
        
        /* Type keywords */
        {"Int",         TOKEN_TYPE              },
        {"Real",        TOKEN_TYPE              },
        {"Char",        TOKEN_TYPE              },
        {"Void",        TOKEN_TYPE              },
        
        /* Access Modifiers */
        {"public",      TOKEN_ACCMOD            },
        {"protected",   TOKEN_ACCMOD            },
        {"private",     TOKEN_ACCMOD            },
        
        /* Modifier keywords */
        {"const",       TOKEN_MODIFIER          },
        {"fixed",       TOKEN_MODIFIER          },
        {"unsigned",    TOKEN_MODIFIER          },
        {"signed",      TOKEN_MODIFIER          },
        {"extern",      TOKEN_MODIFIER          },
        {"static",      TOKEN_MODIFIER          },
        {"volatile",    TOKEN_MODIFIER          },
        
        /* Logical operator keywords */
        {"or",          TOKEN_LOGICAL           },
        {"and",         TOKEN_LOGICAL           },
        
        /* Single-character operators */
        {"%",           TOKEN_PERCENT           },
        {":",           TOKEN_COLON             },
        {".",           TOKEN_DOT               },
        {";",           TOKEN_SEMICOLON         },
        {"=",           TOKEN_EQUAL             },
        {",",           TOKEN_COMMA             },
        {"+",           TOKEN_PLUS              },
        {"-",           TOKEN_MINUS             },
        {"*",           TOKEN_STAR              },
        {"/",           TOKEN_SLASH             },
        {"?",           TOKEN_QUESTION          },
        {"~",           TOKEN_TILDE             },
        {"|",           TOKEN_PIPE              },
        {"&",           TOKEN_AMPERSAND         },
        {"!",           TOKEN_BANG              },
        {"!~",          TOKEN_NE_TILDE          },
        {"^",           TOKEN_CARET             },
        {"@",           TOKEN_AT                },
        {">",           TOKEN_GT                },
        {"<",           TOKEN_LT                },
        {">>",          TOKEN_SHR               },
        {"<<",          TOKEN_SHL               },
        {">>>",         TOKEN_SAR               },
        {"<<<",         TOKEN_SAL               },
        {">>>>",        TOKEN_ROR               },
        {"<<<<",        TOKEN_ROL               },
        {">=",          TOKEN_GE                },
        {"<=",          TOKEN_LE                },
        {"==",          TOKEN_DOUBLE_EQ         },
        {"!=",          TOKEN_NE                },
        {"+=",          TOKEN_PLUS_EQ           },
        {"-=",          TOKEN_MINUS_EQ          },
        {"*=",          TOKEN_STAR_EQ           },
        {"/=",          TOKEN_SLASH_EQ          },
        {"%=",          TOKEN_PERCENT_EQ        },
        {"|=",          TOKEN_PIPE_EQ           },
        {"&=",          TOKEN_AMPERSAND_EQ      },
        {"^=",          TOKEN_CARET_EQ          },
        {"<<=",         TOKEN_SHL_EQ            },
        {">>=",         TOKEN_SHR_EQ            },
        {"<<<=",        TOKEN_SAL_EQ            },
        {">>>=",        TOKEN_SAR_EQ            },
        {"<<<<=",       TOKEN_ROL_EQ            },
        {">>>>=",       TOKEN_ROR_EQ            },
        {"&&",          TOKEN_DOUBLE_AMPERSAND  },
        {"@@",          TOKEN_DOUBLE_AT         },
        {"++",          TOKEN_DOUBLE_PLUS       },
        {"--",          TOKEN_DOUBLE_MINUS      },
        {"->",          TOKEN_INDICATOR         },
        {"::",          TOKEN_INDICATOR         },
        {"=>",          TOKEN_THEN              },
         
        /* Brackets */
        {"{",           TOKEN_LCURLY            },
        {"}",           TOKEN_RCURLY            },
        {"[",           TOKEN_LBRACE            },
        {"]",           TOKEN_RBRACE            },
        {"(",           TOKEN_LPAREN            },
        {")",           TOKEN_RPAREN            }
    };
    
    /* Calculate number of symbol definitions */
    const size_t symbol_count = sizeof(symbol_definitions) /
                                sizeof(symbol_definitions[0]);
    
    /* Single allocation for all hash table entries */
    SymbolEntry* symbol_entries = malloc
        ( symbol_count
        * sizeof(SymbolEntry)
    );
    
    if (!symbol_entries) return;
    
    /* Save the pointer to the allocated block */
    symbol_block = symbol_entries;
    
    /* Populate hash table with linked lists */
    for (size_t index = 0; index < symbol_count; index++) {
        uint8_t symbol_length = (uint8_t)strlen
            ( symbol_definitions[index].symbol
        );
        
        uint8_t hash_index = hash_symbol
            ( symbol_definitions[index].symbol
            , symbol_length
        );
        
        SymbolEntry* entry = &symbol_entries[index];
        entry->symbol = symbol_definitions[index].symbol;
        entry->token_type = symbol_definitions[index].token_type;
        entry->symbol_length = symbol_length;
        entry->next = symbol_table[hash_index];
        symbol_table[hash_index] = entry;
    }
}

/*
 * Lookup symbol in hash table with exact length match
 * Inline for performance in tokenization hot path
 * @param symbol_string: String to lookup
 * @param symbol_length: Exact length of string
 * @return: Token type or TOKEN_ID if not found
 */
static inline TokenType lookup_symbol
    ( const char* symbol_string
    , uint8_t symbol_length
) {
    SymbolEntry* current_entry = symbol_table
        [ hash_symbol
            ( symbol_string
            , symbol_length
        )
    ];
    
    while (current_entry != NULL) {
        if
            ( current_entry->symbol_length == symbol_length
            && memcmp
                ( current_entry->symbol
                , symbol_string
                , symbol_length
            ) == 0
        ) return current_entry->token_type;
        
        current_entry = current_entry->next;
    }
    
    return TOKEN_ID;
}

/*
 * Free symbol table memory at program exit
 * Single free call for batch-allocated entries
 */
static void free_symbol_table(void) {
    /* Free the allocated block */
    if (symbol_block != NULL) {
        free(symbol_block);
        symbol_block = NULL;
    }
    
    /* Clear the hash table */
    memset(symbol_table, 0, sizeof(symbol_table));
}

/*
 * Skip whitespace characters in input stream
 * Inline for performance in main tokenization loop
 * Updates position, line and column counters efficiently
 * @param lexer: Lexer instance
 */
static inline void skip_whitespace(Lexer* lexer) {
    const char* source = lexer->source;
    uint32_t pos = lexer->position;
    uint16_t line = lexer->line;
    uint16_t col = lexer->column;
    
    while (pos < lexer->source_length) {
        char current_char = source[pos];
        
        if (current_char == ' ' || current_char == '\t') {
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

/*
 * Check if character can start operator token
 * Manual implementation avoids ctype.h overhead
 * @param character: Character to check
 * @return: Non-zero if operator start, zero otherwise
 */
static inline int is_operator_start_character(char character) {
    return !( (character >= 'a' && character <= 'z')
           || (character >= 'A' && character <= 'Z')
           || (character >= '0' && character <= '9')
           || character == '_'
           );
}

/*
 * Lookup multi-character operator at current position
 * Tries longest matches first (up to 5 characters)
 * @param lexer: Lexer instance
 * @param operator_length: Output parameter for operator length
 * @return: Token type or TOKEN_ERROR if not found
 */
static inline TokenType lookup_operator
    ( Lexer* lexer
    , uint8_t* operator_length
) {
    const char* current_position = lexer->source + lexer->position;
    uint32_t remaining_chars = lexer->source_length - lexer->position;
    
    if
        ( remaining_chars > 0
        && !is_operator_start_character(*current_position)
    ) return TOKEN_ERROR;
    
    /* Try longest matches first (5 characters maximum) */
    for (uint8_t test_length = 5; test_length > 0; test_length--) {
        if (remaining_chars >= test_length) {
            TokenType token_type = lookup_symbol
                ( current_position
                , test_length
            );
            
            if (token_type != TOKEN_ID) {
                *operator_length = test_length;
                return token_type;
            }
        }
    }
    
    return TOKEN_ERROR;
}

/*
 * Check if character is valid identifier character
 * Manual implementation avoids ctype.h overhead
 * @param character: Character to check
 * @return: Non-zero if valid identifier character
 */
static inline int is_identifier_character(char character) {
    return (character >= 'a' && character <= 'z')
        || (character >= 'A' && character <= 'Z')
        || (character >= '0' && character <= '9')
        || character == '_';
}

/*
 * Add token to lexer's token array
 * Handles dynamic array resizing and value copying
 * @param lexer: Lexer instance
 * @param token_type: Token type to add
 * @param token_value: Token value string (copied if not NULL)
 * @param value_length: Length of value string
 */
static void add_token_to_lexer
    ( Lexer* lexer
    , TokenType token_type
    , const char* token_value
    , uint32_t value_length
) {
    if (lexer == NULL) {
        fprintf(stderr, "ERROR: Lexer is NULL\n");
        return;
    }
    
    if (lexer->token_count >= UINT64_MAX) {
        errhandler__report_error
            ( lexer->line
            , lexer->column
            , "inside"
            , "Too many tokens, maximum is 18446744073709551615"
        );
        return;
    }
    
    /* Resize token array if capacity exceeded */
    if (lexer->token_count >= lexer->token_capacity) {
        const uint16_t new_capacity = lexer->token_capacity * 2;
        
        if (new_capacity <= lexer->token_capacity || new_capacity >= UINT16_MAX) {
            errhandler__report_error
                ( lexer->line
                , lexer->column
                , "inside"
                , "Token array capacity overflow"
            );
            return;
        }
        
        Token* new_token_array = realloc
            ( lexer->tokens
            , new_capacity * sizeof(Token)
        );
        
        if (new_token_array == NULL) {
            errhandler__report_error
                ( lexer->line
                , lexer->column
                , "inside"
                , "Memory allocation failed for token array"
            );
            return;
        }
        
        lexer->tokens = new_token_array;
        lexer->token_capacity = new_capacity;
    }
    
    Token* current_token = &lexer->tokens[lexer->token_count];
    
    memset(current_token, 0, sizeof(Token));
    
    current_token->type = token_type;
    current_token->line = lexer->line;
    current_token->length = (uint16_t)value_length;
    
    if (value_length > 0) {
        if (lexer->column > value_length)
            current_token->column = lexer->column - value_length;
        else current_token->column = 1;
    } else current_token->column = lexer->column;
    
    /* Copy token value if provided */
    if (value_length > 0 && token_value != NULL) {
        current_token->value = malloc(value_length + 1);
        
        if (current_token->value == NULL) {
            errhandler__report_error
                ( lexer->line
                , lexer->column
                , "inside"
                , "Memory allocation failed for token value"
            );
            lexer->token_count--;
            return;
        }
        
        memcpy
            ( current_token->value
            , token_value
            , value_length
            );
        current_token->value[value_length] = '\0';
    } else {
        current_token->value = NULL;
    }
    
    lexer->token_count++;
}

/*
 * Initialize lexer with source code input
 * Performs one-time symbol table initialization
 * @param input: Source code string to tokenize
 * @return: Initialized lexer instance or NULL on failure
 */
Lexer* lexer__init_lexer(const char* input) {
    static uint8_t symbol_table_initialized = 0;
    
    if (!symbol_table_initialized) {
        init_symbol_table();
        symbol_table_initialized = 1;
        atexit(free_symbol_table);
    }
    
    if (input == NULL) {
        fprintf(stderr, "ERROR: Input string is NULL\n");
        return NULL;
    }
    
    Lexer* lexer = malloc(sizeof(Lexer));
    
    if (lexer == NULL) {
        fprintf(stderr, "ERROR: Failed to allocate lexer\n");
        return NULL;
    }
    
    memset(lexer, 0, sizeof(Lexer));
    
    lexer->source = input;
    lexer->source_length = (uint32_t)strlen(input);
    
    if (lexer->source_length == 0)
        fprintf(stderr, "WARNING: Empty source string\n");
    
    lexer->position = 0;
    lexer->line = 1;
    lexer->column = 1;
    lexer->token_count = 0;
    lexer->token_capacity = INITIAL_TOKEN_CAPACITY;
    lexer->tokens = malloc
        ( lexer->token_capacity
        * sizeof(Token)
    );
    
    if (lexer->tokens == NULL) {
        fprintf(stderr, "ERROR: Failed to allocate token array\n");
        free(lexer);
        return NULL;
    }
    
    memset(lexer->tokens, 0, lexer->token_capacity * sizeof(Token));
    
    return lexer;
}

/*
 * Free all memory allocated by lexer
 * Handles nested deallocation of tokens and values
 * @param lexer: Lexer instance to deallocate
 */
void lexer__free_lexer(Lexer* lexer) {
    if (lexer == NULL) {
        fprintf(stderr, "WARNING: Attempt to free NULL lexer\n");
        return;
    }
    
    /* Free dynamically allocated token values */
    if (lexer->tokens != NULL) {
        for (uint16_t token_index = 0; 
             token_index < lexer->token_count && token_index < UINT16_MAX; 
             token_index++) {
            if (lexer->tokens[token_index].value != NULL) {
                free(lexer->tokens[token_index].value);
                lexer->tokens[token_index].value = NULL;
            }
        }
        free(lexer->tokens);
        lexer->tokens = NULL;
    }
    
    free(lexer);
}

/*
 * Main tokenization function
 * Processes input string and generates token stream
 * @param lexer: Initialized lexer instance
 */
void lexer__tokenize(Lexer* lexer) {
    const char* source = lexer->source;
    
    while (lexer->position < lexer->source_length) {
        skip_whitespace(lexer);
        
        if (lexer->position >= lexer->source_length) break;
        
        char current_char = source[lexer->position];
        
        if (current_char == '\'') { // Character literal
            Token character_token = parse_char_literal(lexer);
            add_token_to_lexer
                ( lexer
                , character_token.type
                , character_token.value
                , character_token.length
                );
            
            if (character_token.value != NULL) free(character_token.value);
            continue;
        } else if (current_char == '"') { // String literal
            Token string_token = parse_string_literal(lexer);
            add_token_to_lexer
                ( lexer
                , string_token.type
                , string_token.value
                , string_token.length
                );
            
            if (string_token.value != NULL) free(string_token.value);
            continue;
        }
        
        /* Operator token */
        uint8_t operator_length;
        TokenType operator_type = lookup_operator
            ( lexer
            , &operator_length
        );
        
        if (operator_type != TOKEN_ERROR) {
            add_token_to_lexer
                ( lexer
                , operator_type
                , source + lexer->position
                , operator_length
            );
            
            lexer->position += operator_length;
            lexer->column += operator_length;
            continue;
        }
        
        /* Identifier or keyword */
        if (
            (current_char >= 'a' && current_char <= 'z')
            || (current_char >= 'A' && current_char <= 'Z')
            || current_char == '_'
        ) {
            uint32_t identifier_start = lexer->position;
            
            while (
                lexer->position < lexer->source_length
                && is_identifier_character
                    (source[lexer->position])
            ) {
                lexer->position++;
                lexer->column++;
            }
            
            uint32_t identifier_length = lexer->position - identifier_start;
            const char* identifier_string = source + identifier_start;
            
            TokenType identifier_type = lookup_symbol
                ( identifier_string
                , (uint8_t)identifier_length
            );
            
            add_token_to_lexer
                ( lexer
                , identifier_type
                , identifier_string
                , identifier_length
            );
        } else if ( ( current_char >= '0' && current_char <= '9' )
            || (
                current_char == '.'
                && lexer->position + 1 < lexer->source_length
                && (
                    (
                        source[lexer->position + 1] >= '0'
                        && source[lexer->position + 1] <= '9'
                    ) || source[lexer->position + 1] == '('
                )
            ) || (
                (current_char == '-' || current_char == '+')
                && lexer->position + 1 < lexer->source_length
                && (source[lexer->position + 1] >= '0'
                && source[lexer->position + 1] <= '9')
            )
        ) {
            Token number_token = parse_number_literal(lexer);
            add_token_to_lexer
                ( lexer
                , number_token.type
                , number_token.value
                , number_token.length
            );
            
            if (number_token.value != NULL) free(number_token.value);
        } else { // Invalid character
            errhandler__report_error
                ( lexer->line
                , lexer->column
                , "syntax"
                , "Unexpected character in source code"
            );
            
            lexer->position++;
            lexer->column++;
        }
    }
    
    /* Add end-of-file marker token */
    add_token_to_lexer
        ( lexer
        , TOKEN_EOF
        , ""
        , 0
    );
}
