#ifndef LEXER_H
#define LEXER_H

#include <stdint.h>
#include <stddef.h>
#include "../../interfaces/utils/utils.h"

/*
 * All possible token kinds.
 */
typedef enum {
    /* Keywords */
    TOKEN_USE,
    TOKEN_FUNC,
    TOKEN_LET,
    TOKEN_COMPTIME,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_MATCH,
    TOKEN_CASE,
    TOKEN_WHILE,
    TOKEN_DO,
    TOKEN_FOR,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_RETURN,
    TOKEN_DEFER,
    TOKEN_TRY,
    TOKEN_CATCH,
    TOKEN_ERROR,
    TOKEN_VOLATILE,
    TOKEN_ASM,
    TOKEN_ALLOC,
    TOKEN_FREE,
    TOKEN_PASS,
    TOKEN_MODULE,
    TOKEN_EXTERN,
    TOKEN_NONE,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NOT,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_AS,
    TOKEN_ROUTINE,
    TOKEN_ASYNC,
    TOKEN_AWAIT,
    TOKEN_CHAN,
    TOKEN_SELECT,
    TOKEN_ON,
    TOKEN_CLOSE,

    /* Types */
    TOKEN_INT,
    TOKEN_UINT,
    TOKEN_REAL,
    TOKEN_CHAR,
    TOKEN_BOOL,
    TOKEN_VOID,
    TOKEN_AUTO,
    TOKEN_INT8,
    TOKEN_INT16,
    TOKEN_INT32,
    TOKEN_INT64,
    TOKEN_INT128,
    TOKEN_INT256,
    TOKEN_UINT8,
    TOKEN_UINT16,
    TOKEN_UINT32,
    TOKEN_UINT64,
    TOKEN_UINT128,
    TOKEN_UINT256,
    TOKEN_REAL32,
    TOKEN_REAL64,
    TOKEN_UTF8,
    TOKEN_UTF16,
    TOKEN_UTF32,
    TOKEN_STRUCT,
    TOKEN_UNION,
    TOKEN_ENUM,

    /* Single‑character operators & punctuation */
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_EQUAL,
    TOKEN_GREATER,
    TOKEN_LESS,
    TOKEN_BANG,
    TOKEN_QUESTION,
    TOKEN_TILDE,
    TOKEN_DOLLAR,
    TOKEN_AMPERSAND,
    TOKEN_CARET,
    TOKEN_AT,
    TOKEN_COLON,
    TOKEN_SEMICOLON,
    TOKEN_COMMA,
    TOKEN_DOT,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_PIPE,

    /* Multi‑character operators */
    TOKEN_EQUAL_EQUAL,
    TOKEN_NOT_EQUAL,
    TOKEN_GREATER_EQUAL,
    TOKEN_LESS_EQUAL,
    TOKEN_LSHIFT,
    TOKEN_RSHIFT,
    TOKEN_LSHIFT_LOGICAL,
    TOKEN_RSHIFT_LOGICAL,
    TOKEN_PLUS_EQUAL,
    TOKEN_MINUS_EQUAL,
    TOKEN_STAR_EQUAL,
    TOKEN_SLASH_EQUAL,
    TOKEN_PERCENT_EQUAL,
    TOKEN_DOLLAR_EQUAL,
    TOKEN_AMPERSAND_EQUAL,
    TOKEN_CARET_EQUAL,
    TOKEN_LSHIFT_EQUAL,
    TOKEN_RSHIFT_EQUAL,
    TOKEN_LSHIFT_LOGICAL_EQUAL,
    TOKEN_RSHIFT_LOGICAL_EQUAL,
    TOKEN_COLON_COLON,
    TOKEN_ARROW,
    TOKEN_RANGE,

    /* Literals & special */
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER_LITERAL,
    TOKEN_STRING_LITERAL,
    TOKEN_CHAR_LITERAL,
    TOKEN_INTERPOLATED_STRING,
    TOKEN_EOF,
    TOKEN_ERRORCODE
} TokenType;

/*
 * Token structure – compact layout using bit‑fields.
 */
typedef struct {
    uint16_t type;          /* TokenType, fits in 16 bits */
    uint32_t line;
    uint32_t column;
    uint32_t length;
    const char* value;      /* points into intern pool */
} Token;

/*
 * Lexer state.
 */
typedef struct {
    const char* source;
    uint32_t source_length;
    uint32_t position;
    uint32_t line;
    uint32_t column;
    Token* tokens;
    uint64_t token_count;
    uint32_t token_capacity;
} Lexer;

Lexer* lexer__init_lexer(const char* input);
void lexer__free_lexer(Lexer* lexer);
void lexer__tokenize(Lexer* lexer);

#endif
