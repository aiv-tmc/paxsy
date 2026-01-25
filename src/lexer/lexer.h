#ifndef LEXER_H
#define LEXER_H

#include <stdint.h>

/*
 * Token types for lexical analysis
 * Organized by category for better cache locality
 */
typedef enum {
    /* Literals */
    TOKEN_NUMBER,
    TOKEN_CHAR,
    TOKEN_STRING,
    
    /* Keywords */
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_NOP,
    TOKEN_HALT,
    TOKEN_JUMP,
    TOKEN_FREE,
    TOKEN_SIZEOF,
    TOKEN_PARSEOF,
    TOKEN_REALLOC,
    TOKEN_ALLOC,
    TOKEN_INTER,
    TOKEN_SIGNAL,
    TOKEN_PUSH,
    TOKEN_POP,
    TOKEN_RETURN,
    TOKEN_NONE,
    TOKEN_NULL,
    
    /* Token categories */
    TOKEN_STATE,
    TOKEN_TYPE,
    TOKEN_ACCMOD,
    TOKEN_MODIFIER,
    TOKEN_LOGICAL,
    TOKEN_ID,
     
    /* Punctuation */
    TOKEN_PERCENT,
    TOKEN_COLON,
    TOKEN_DOT,
    TOKEN_SEMICOLON,
    TOKEN_EQUAL,
    TOKEN_COMMA,
    
    /* Arithmetic operators */
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    
    /* Special symbols */
    TOKEN_QUESTION,
    TOKEN_TILDE,
    TOKEN_NE_TILDE,
    TOKEN_PIPE,
    TOKEN_AMPERSAND,
    TOKEN_BANG,
    TOKEN_CARET,
    TOKEN_AT,
    TOKEN_GT,
    TOKEN_LT,
    
    /* Bitwise shift operators */
    TOKEN_SHR,
    TOKEN_SHL,
    TOKEN_SAR,
    TOKEN_SAL,
    TOKEN_ROR,
    TOKEN_ROL,
    
    /* Comparison operators */
    TOKEN_GE,
    TOKEN_LE,
    TOKEN_DOUBLE_EQ,
    TOKEN_NE,
    
    /* Compound assignment operators */
    TOKEN_PLUS_EQ,
    TOKEN_MINUS_EQ,
    TOKEN_STAR_EQ,
    TOKEN_SLASH_EQ,
    TOKEN_PERCENT_EQ,
    TOKEN_PIPE_EQ,
    TOKEN_AMPERSAND_EQ,
    TOKEN_CARET_EQ,
    TOKEN_SHL_EQ,
    TOKEN_SHR_EQ,
    TOKEN_SAL_EQ,
    TOKEN_SAR_EQ,
    TOKEN_ROL_EQ,
    TOKEN_ROR_EQ,
    
    /* Multi-character operators */
    TOKEN_DOUBLE_AMPERSAND,
    TOKEN_DOUBLE_AT,
    TOKEN_DOUBLE_PLUS,
    TOKEN_DOUBLE_MINUS,
    TOKEN_INDICATOR,
    TOKEN_THEN,
    
    /* Brackets and arrows */
    TOKEN_LCURLY,
    TOKEN_RCURLY,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    
    /* Special tokens */
    TOKEN_EOF,
    TOKEN_ERROR
} TokenType;

/*
 * Token structure for storing lexical unit information
 * Optimized for memory usage with minimal field sizes
 */
typedef struct {
    TokenType type;          // Token type identifier
    char* value;             // Dynamically allocated value string
    uint16_t line;           // Source line number (1-indexed)
    uint16_t column;         // Source column number (1-indexed)
    uint16_t length;         // Length of token value in bytes
} Token;

/*
 * Lexer structure for tokenization state
 * Uses optimal integer sizes for memory efficiency
 */
typedef struct {
    const char* source;           // Source code string
    uint32_t source_length;       // Total length of source
    uint32_t position;           // Current reading position
    uint16_t line;               // Current line number
    uint16_t column;             // Current column number
    Token* tokens;               // Dynamic array of tokens
    uint16_t token_count;        // Number of tokens stored
    uint16_t token_capacity;     // Current capacity of token array
} Lexer;

/* Global token name array for debugging and error reporting */
extern const char* token_names[];

/*
 * Initialize lexer with source code input
 * @param input: Source code string to tokenize
 * @return: Pointer to initialized Lexer or NULL on allocation failure
 */
Lexer* lexer__init_lexer(const char* input);

/*
 * Free all memory allocated by lexer
 * @param lexer: Lexer instance to deallocate
 */
void lexer__free_lexer(Lexer* lexer);

/*
 * Perform lexical analysis on input source code
 * @param lexer: Initialized lexer instance
 */
void lexer__tokenize(Lexer* lexer);

#endif
