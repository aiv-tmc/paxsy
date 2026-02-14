#ifndef LEXER_H
#define LEXER_H

#include <stdint.h>
#include "../utils/str_utils.h"

/**
 * @enum TokenType
 * @brief All possible token kinds produced by the lexer.
 * 
 * The enumeration covers literals, keywords, operators, punctuation,
 * and special markers like EOF and ERROR.
 */
typedef enum {
    /* Literals */
    TOKEN_NUMBER,          /**< Numeric literal (integer or real) */
    TOKEN_CHAR,            /**< Character literal, e.g. 'a' */
    TOKEN_STRING,          /**< String literal, e.g. "hello" */
    
    /* Keywords */
    TOKEN_IF,              /**< `if` keyword */
    TOKEN_ELSE,            /**< `else` keyword */
    TOKEN_NOP,             /**< `nop` keyword (no operation) */
    TOKEN_HALT,            /**< `halt` keyword */
    TOKEN_JUMP,            /**< `jump` keyword */
    TOKEN_FREE,            /**< `free` keyword */
    TOKEN_SIZEOF,          /**< `sizeof` keyword */
    TOKEN_PARSEOF,         /**< `parseof` keyword */
    TOKEN_REALLOC,         /**< `realloc` keyword */
    TOKEN_ALLOC,           /**< `alloc` keyword */
    TOKEN_SIGNAL,          /**< `signal` keyword */
    TOKEN_PUSH,            /**< `push` keyword */
    TOKEN_POP,             /**< `pop` keyword */
    TOKEN_RETURN,          /**< `return` keyword */
    TOKEN_NONE,            /**< `none` keyword */
    TOKEN_NULL,            /**< `null` keyword */
    
    /* Token categories (used for symbols that belong to a group) */
    TOKEN_STATE,           /**< State keyword: func, var, obj, struct, class */
    TOKEN_TYPE,            /**< Type keyword: Int, Real, Char, Void */
    TOKEN_ACCMOD,          /**< Access modifier: public, protected, private */
    TOKEN_MODIFIER,        /**< Type modifier: const, fixed, unsigned, signed, etc. */
    TOKEN_LOGICAL,         /**< Logical operator keyword: or, and */
    TOKEN_ID,              /**< Identifier (variable/function name) */
     
    /* Punctuation */
    TOKEN_PERCENT,         /**< `%` */
    TOKEN_COLON,           /**< `:` */
    TOKEN_DOT,             /**< `.` */
    TOKEN_SEMICOLON,       /**< `;` */
    TOKEN_EQUAL,           /**< `=` */
    TOKEN_COMMA,           /**< `,` */
    
    /* Arithmetic operators */
    TOKEN_PLUS,            /**< `+` */
    TOKEN_MINUS,           /**< `-` */
    TOKEN_STAR,            /**< `*` */
    TOKEN_SLASH,           /**< `/` */
    
    /* Special symbols */
    TOKEN_QUESTION,        /**< `?` */
    TOKEN_TILDE,           /**< `~` */
    TOKEN_NE_TILDE,        /**< `!~` (not‑tilde) */
    TOKEN_PIPE,            /**< `|` */
    TOKEN_AMPERSAND,       /**< `&` */
    TOKEN_BANG,            /**< `!` */
    TOKEN_CARET,           /**< `^` */
    TOKEN_AT,              /**< `@` */
    TOKEN_GT,              /**< `>` */
    TOKEN_LT,              /**< `<` */
    
    /* Bitwise shift operators */
    TOKEN_SHR,             /**< `>>` shift right */
    TOKEN_SHL,             /**< `<<` shift left */
    TOKEN_SAR,             /**< `>>>` arithmetic shift right */
    TOKEN_SAL,             /**< `<<<` arithmetic shift left */
    TOKEN_ROR,             /**< `>>>>` rotate right */
    TOKEN_ROL,             /**< `<<<<` rotate left */
    
    /* Comparison operators */
    TOKEN_GE,              /**< `>=` greater or equal */
    TOKEN_LE,              /**< `<=` less or equal */
    TOKEN_DOUBLE_EQ,       /**< `==` equal */
    TOKEN_NE,              /**< `!=` not equal */
    
    /* Compound assignment operators */
    TOKEN_PLUS_EQ,         /**< `+=` */
    TOKEN_MINUS_EQ,        /**< `-=` */
    TOKEN_STAR_EQ,         /**< `*=` */
    TOKEN_SLASH_EQ,        /**< `/=` */
    TOKEN_PERCENT_EQ,      /**< `%=` */
    TOKEN_PIPE_EQ,         /**< `|=` */
    TOKEN_AMPERSAND_EQ,    /**< `&=` */
    TOKEN_CARET_EQ,        /**< `^=` */
    TOKEN_SHL_EQ,          /**< `<<=` */
    TOKEN_SHR_EQ,          /**< `>>=` */
    TOKEN_SAL_EQ,          /**< `<<<=` (shift arithmetic left assign) */
    TOKEN_SAR_EQ,          /**< `>>>=` (shift arithmetic right assign) */
    TOKEN_ROL_EQ,          /**< `<<<<=` (rotate left assign) */
    TOKEN_ROR_EQ,          /**< `>>>>=` (rotate right assign) */
    
    /* Multi-character operators */
    TOKEN_DOUBLE_AMPERSAND,/**< `&&` logical AND */
    TOKEN_DOUBLE_AT,       /**< `@@` */
    TOKEN_DOUBLE_PLUS,     /**< `++` increment */
    TOKEN_DOUBLE_MINUS,    /**< `--` decrement */
    TOKEN_INDICATOR,       /**< `->` or `::` (member access / scope) */
    TOKEN_THEN,            /**< `=>` (then arrow) */
    
    /* Brackets and braces */
    TOKEN_LCURLY,          /**< `{` */
    TOKEN_RCURLY,          /**< `}` */
    TOKEN_LBRACE,          /**< `[` */
    TOKEN_RBRACE,          /**< `]` */
    TOKEN_LPAREN,          /**< `(` */
    TOKEN_RPAREN,          /**< `)` */
    
    /* Special tokens */
    TOKEN_EOF,             /**< End‑of‑file marker */
    TOKEN_ERROR            /**< Error token (unrecognised input) */
} TokenType;

/**
 * @struct Token
 * @brief Represents a single token produced by the lexer.
 */
typedef struct {
    TokenType type;        /**< Type of the token (see TokenType) */
    char* value;           /**< Dynamically allocated string holding the token's text */
    uint16_t line;         /**< Line number where the token starts (1‑based) */
    uint16_t column;       /**< Column number where the token starts (1‑based) */
    uint16_t length;       /**< Length of the token's text in characters */
} Token;

/**
 * @struct Lexer
 * @brief Main lexer state, holds the source code and the list of tokens.
 */
typedef struct {
    const char* source;    /**< Pointer to the entire source code string */
    uint32_t source_length;/**< Length of the source code */
    uint32_t position;     /**< Current scanning position (byte index) */
    uint16_t line;         /**< Current line number (for error reporting) */
    uint16_t column;       /**< Current column number (for error reporting) */
    Token* tokens;         /**< Dynamically allocated array of tokens */
    uint64_t token_count;  /**< Number of tokens currently stored */
    uint16_t token_capacity;/**< Allocated capacity of the tokens array */
} Lexer;

/**
 * @brief Array mapping TokenType values to human‑readable names.
 * 
 * Useful for debugging and error messages. Defined in lexer.c.
 */
extern const char* token_names[];

/**
 * @brief Initialises a new lexer for the given input string.
 * 
 * Allocates and initialises a Lexer structure. The symbol table is
 * initialised once (the first time this function is called).
 * 
 * @param input Null‑terminated source code string.
 * @return Pointer to a new Lexer, or NULL on allocation failure.
 */
Lexer* lexer__init_lexer(const char* input);

/**
 * @brief Frees all memory associated with a lexer.
 * 
 * Releases the token array, each token's value string, and the lexer itself.
 * 
 * @param lexer Lexer to free (may be NULL).
 */
void lexer__free_lexer(Lexer* lexer);

/**
 * @brief Performs lexical analysis on the lexer's source code.
 * 
 * Scans the source, recognises tokens, and appends them to lexer->tokens.
 * In case of an unrecognised character, an error is reported via the error
 * handler and scanning continues. Finally, a TOKEN_EOF marker is added.
 * 
 * @param lexer Lexer instance to operate on (must be valid).
 */
void lexer__tokenize(Lexer* lexer);

#endif
