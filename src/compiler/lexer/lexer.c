#include "lexer.h"
#include "../parser/literals.h"
#include "../../interfaces/errhandler/errhandler.h"
#include "../../interfaces/utils/memory.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>

#define HASH_TABLE_SIZE 64
#define INITIAL_TOKEN_CAPACITY 8

/*
 * Intern pool – stores unique copies of token values.
 * Each entry holds a reference count.
 */
typedef struct InternEntry {
    const char* string;
    uint32_t hash;
    uint32_t refcount;
    struct InternEntry* next;
} InternEntry;

static InternEntry* intern_table[HASH_TABLE_SIZE] = {0};

/*
 * FNV‑1a hash for a string of known length.
 */
static inline uint32_t intern_hash(const char* str, uint32_t len) {
    uint32_t hash = 2166136261u;
    while (len--) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash;
}

/*
 * Acquire an interned string. Increments reference count.
 * Returns pointer to the interned string, or NULL on allocation failure.
 */
const char* intern_string_acquire(const char* str, uint32_t len) {
    uint32_t hash = intern_hash(str, len);
    uint32_t bucket = hash & (HASH_TABLE_SIZE - 1);
    InternEntry* entry = intern_table[bucket];
    while (entry) {
        if (entry->hash == hash && entry->string &&
            strlen(entry->string) == len && memcmp(entry->string, str, len) == 0) {
            entry->refcount++;
            return entry->string;
        }
        entry = entry->next;
    }

    /* Not found – create new entry */
    char* copy = (char*)memory_allocate_zero(len + 1);
    if (!copy) return NULL;
    memcpy(copy, str, len);
    copy[len] = '\0';

    InternEntry* new_entry = (InternEntry*)memory_allocate_zero(sizeof(InternEntry));
    if (!new_entry) {
        memory_free_safe((void**)&copy);
        return NULL;
    }
    new_entry->string = copy;
    new_entry->hash = hash;
    new_entry->refcount = 1;
    new_entry->next = intern_table[bucket];
    intern_table[bucket] = new_entry;
    return copy;
}

/*
 * Release an interned string. Decrements reference count.
 * When count reaches zero, the entry is removed and freed.
 */
void intern_string_release(const char* str) {
    if (!str) return;
    uint32_t len = (uint32_t)strlen(str);
    uint32_t hash = intern_hash(str, len);
    uint32_t bucket = hash & (HASH_TABLE_SIZE - 1);
    InternEntry** prev = &intern_table[bucket];
    InternEntry* entry = *prev;
    while (entry) {
        if (entry->hash == hash && entry->string == str) {
            entry->refcount--;
            if (entry->refcount == 0) {
                *prev = entry->next;
                memory_free_safe((void**)&entry->string);
                memory_free_safe((void**)&entry);
            }
            return;
        }
        prev = &entry->next;
        entry = entry->next;
    }
}

/*
 * Free all interned strings (called at exit).
 */
static void free_intern_pool(void) {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        InternEntry* entry = intern_table[i];
        while (entry) {
            InternEntry* next = entry->next;
            memory_free_safe((void**)&entry->string);
            memory_free_safe((void**)&entry);
            entry = next;
        }
    }
    memset(intern_table, 0, sizeof(intern_table));
}

/*
 * Symbol table entry (linked list for collisions).
 */
typedef struct SymbolEntry {
    const char* symbol;
    TokenType token_type;
    uint8_t symbol_length;
    struct SymbolEntry* next;
} SymbolEntry;

static SymbolEntry* symbol_table[HASH_TABLE_SIZE] = {0};
static SymbolEntry* symbol_block = NULL;

/*
 * FNV‑1a hash for a string of known length.
 * Returns a hash value modulo HASH_TABLE_SIZE.
 */
static inline uint8_t hash_symbol(const char* str, uint8_t len) {
    uint32_t hash = 2166136261u;
    while (len--) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash & (HASH_TABLE_SIZE - 1);
}

/*
 * Initialise the symbol table with all keywords and operators.
 * All entries are allocated in one contiguous block.
 */
static void init_symbol_table(void) {
    static const struct {
        const char* symbol;
        TokenType token_type;
    } symbol_definitions[] = {
        /* Keywords */
        {"use", TOKEN_USE},
        {"func", TOKEN_FUNC},
        {"let", TOKEN_LET},
        {"comptime", TOKEN_COMPTIME},
        {"if", TOKEN_IF},
        {"else", TOKEN_ELSE},
        {"match", TOKEN_MATCH},
        {"case", TOKEN_CASE},
        {"while", TOKEN_WHILE},
        {"do", TOKEN_DO},
        {"for", TOKEN_FOR},
        {"break", TOKEN_BREAK},
        {"continue", TOKEN_CONTINUE},
        {"return", TOKEN_RETURN},
        {"defer", TOKEN_DEFER},
        {"try", TOKEN_TRY},
        {"catch", TOKEN_CATCH},
        {"error", TOKEN_ERROR},
        {"volatile", TOKEN_VOLATILE},
        {"asm", TOKEN_ASM},
        {"alloc", TOKEN_ALLOC},
        {"free", TOKEN_FREE},
        {"pass", TOKEN_PASS},
        {"module", TOKEN_MODULE},
        {"extern", TOKEN_EXTERN},
        {"none", TOKEN_NONE},
        {"true", TOKEN_TRUE},
        {"false", TOKEN_FALSE},
        {"not", TOKEN_NOT},
        {"and", TOKEN_AND},
        {"or", TOKEN_OR},
        {"as", TOKEN_AS},                    /* new keyword */
        {"routine", TOKEN_ROUTINE},
        {"async", TOKEN_ASYNC},
        {"await", TOKEN_AWAIT},
        {"chan", TOKEN_CHAN},
        {"select", TOKEN_SELECT},
        {"on", TOKEN_ON},
        {"close", TOKEN_CLOSE},

        /* Types */
        {"Int", TOKEN_INT},
        {"UInt", TOKEN_UINT},
        {"Real", TOKEN_REAL},
        {"Char", TOKEN_CHAR},
        {"Bool", TOKEN_BOOL},
        {"Void", TOKEN_VOID},
        {"Auto", TOKEN_AUTO},
        {"Int8", TOKEN_INT8},
        {"Int16", TOKEN_INT16},
        {"Int32", TOKEN_INT32},
        {"Int64", TOKEN_INT64},
        {"Int128", TOKEN_INT128},
        {"Int256", TOKEN_INT256},
        {"UInt8", TOKEN_UINT8},
        {"UInt16", TOKEN_UINT16},
        {"UInt32", TOKEN_UINT32},
        {"UInt64", TOKEN_UINT64},
        {"UInt128", TOKEN_UINT128},
        {"UInt256", TOKEN_UINT256},
        {"Real32", TOKEN_REAL32},
        {"Real64", TOKEN_REAL64},
        {"UTF8", TOKEN_UTF8},
        {"UTF16", TOKEN_UTF16},
        {"UTF32", TOKEN_UTF32},
        {"Struct", TOKEN_STRUCT},
        {"Union", TOKEN_UNION},
        {"Enum", TOKEN_ENUM},

        /* Single‑character operators & punctuation */
        {"+", TOKEN_PLUS},
        {"-", TOKEN_MINUS},
        {"*", TOKEN_STAR},
        {"/", TOKEN_SLASH},
        {"%", TOKEN_PERCENT},
        {"=", TOKEN_EQUAL},
        {">", TOKEN_GREATER},
        {"<", TOKEN_LESS},
        {"!", TOKEN_BANG},
        {"?", TOKEN_QUESTION},
        {"~", TOKEN_TILDE},
        {"$", TOKEN_DOLLAR},
        {"&", TOKEN_AMPERSAND},
        {"^", TOKEN_CARET},
        {"@", TOKEN_AT},
        {":", TOKEN_COLON},
        {";", TOKEN_SEMICOLON},
        {",", TOKEN_COMMA},
        {".", TOKEN_DOT},
        {"(", TOKEN_LPAREN},
        {")", TOKEN_RPAREN},
        {"{", TOKEN_LBRACE},
        {"}", TOKEN_RBRACE},
        {"[", TOKEN_LBRACKET},
        {"]", TOKEN_RBRACKET},
        {"|", TOKEN_PIPE},

        /* Multi‑character operators */
        {"==", TOKEN_EQUAL_EQUAL},
        {"<>", TOKEN_NOT_EQUAL},
        {">=", TOKEN_GREATER_EQUAL},
        {"<=", TOKEN_LESS_EQUAL},
        {"<<", TOKEN_LSHIFT},
        {">>", TOKEN_RSHIFT},
        {"<<<", TOKEN_LSHIFT_LOGICAL},
        {">>>", TOKEN_RSHIFT_LOGICAL},
        {"+=", TOKEN_PLUS_EQUAL},
        {"-=", TOKEN_MINUS_EQUAL},
        {"*=", TOKEN_STAR_EQUAL},
        {"/=", TOKEN_SLASH_EQUAL},
        {"%=", TOKEN_PERCENT_EQUAL},
        {"$=", TOKEN_DOLLAR_EQUAL},
        {"&=", TOKEN_AMPERSAND_EQUAL},
        {"^=", TOKEN_CARET_EQUAL},
        {"<<=", TOKEN_LSHIFT_EQUAL},
        {">>=", TOKEN_RSHIFT_EQUAL},
        {"<<<=", TOKEN_LSHIFT_LOGICAL_EQUAL},
        {">>>=", TOKEN_RSHIFT_LOGICAL_EQUAL},
        {"::", TOKEN_COLON_COLON},
        {"<-", TOKEN_ARROW},
        {"..", TOKEN_RANGE},
    };

    const size_t count = ARRAY_SIZE(symbol_definitions);

    symbol_block = (SymbolEntry*)memory_allocate_zero(count * sizeof(SymbolEntry));
    if (!symbol_block) return;

    for (size_t i = 0; i < count; i++) {
        uint8_t len = (uint8_t)strlen(symbol_definitions[i].symbol);
        uint8_t hash = hash_symbol(symbol_definitions[i].symbol, len);

        SymbolEntry* entry = &symbol_block[i];
        entry->symbol = symbol_definitions[i].symbol;
        entry->token_type = symbol_definitions[i].token_type;
        entry->symbol_length = len;
        entry->next = symbol_table[hash];
        symbol_table[hash] = entry;
    }
}

/*
 * Look up a string in the symbol table.
 */
static inline TokenType lookup_symbol(const char* str, uint8_t len) {
    SymbolEntry* entry = symbol_table[hash_symbol(str, len)];
    while (entry) {
        if (entry->symbol_length == len && memcmp(entry->symbol, str, len) == 0) {
            return entry->token_type;
        }
        entry = entry->next;
    }
    return TOKEN_IDENTIFIER;
}

/*
 * Free the symbol table (registered with atexit).
 */
static void free_symbol_table(void) {
    memory_free_safe((void**)&symbol_block);
    memset(symbol_table, 0, sizeof(symbol_table));
}

/*
 * Skip whitespace and newlines, updating line/column counters.
 * Uses memchr to find next non‑whitespace quickly.
 */
static inline void skip_whitespace(Lexer* lexer) {
    const char* source = lexer->source;
    uint32_t pos = lexer->position;
    uint32_t line = lexer->line;
    uint32_t col = lexer->column;

    while (pos < lexer->source_length) {
        /* Skip spaces and tabs quickly */
        const char* p = source + pos;
        size_t rem = lexer->source_length - pos;
        if (rem > 0) {
            size_t span = strspn(p, " \t");
            pos += (uint32_t)span;
            col += (uint32_t)span;
        }
        if (pos >= lexer->source_length) break;

        if (source[pos] == '\n') {
            pos++;
            line++;
            col = 1;
            continue;
        }
        /* Other whitespace (rare) handled one by one */
        if (u__char_is_whitespace(source[pos])) {
            pos++;
            col++;
            continue;
        }
        break;
    }

    lexer->position = pos;
    lexer->line = line;
    lexer->column = col;
}

/*
 * Skip a comment (single‑line // or multi‑line \/\* ... \*\/).
 * Returns 1 if a comment was skipped, 0 otherwise.
 */
static int skip_comment(Lexer* lexer) {
    const char* source = lexer->source;
    uint32_t pos = lexer->position;
    uint32_t line = lexer->line;
    uint32_t col = lexer->column;

    if (pos >= lexer->source_length) return 0;

    /* Single‑line comment */
    if (source[pos] == '/' && pos + 1 < lexer->source_length && source[pos + 1] == '/') {
        pos += 2;
        col += 2;
        while (pos < lexer->source_length && source[pos] != '\n') {
            pos++;
            col++;
        }
        lexer->position = pos;
        lexer->column = col;
        return 1;
    }

    /* Multi‑line comment */
    if (source[pos] == '/' && pos + 1 < lexer->source_length && source[pos + 1] == '*') {
        uint32_t start_col = col;
        pos += 2;
        col += 2;
        while (pos + 1 < lexer->source_length) {
            if (source[pos] == '*' && source[pos + 1] == '/') {
                pos += 2;
                col += 2;
                lexer->position = pos;
                lexer->column = col;
                return 1;
            }
            if (source[pos] == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
            pos++;
        }
        errhandler__report_error(
            line, start_col,
            "lexer",
            "Unterminated multi‑line comment"
        );
        lexer->position = pos;
        lexer->line = line;
        lexer->column = col;
        return 1;
    }

    return 0;
}

/*
 * Try to recognise a multi‑character operator at the current position.
 * Uses a switch on the first character for fast dispatch.
 * If found, sets *op_len and returns its token type; otherwise returns TOKEN_ERRORCODE.
 */
static inline TokenType lookup_operator(Lexer* lexer, uint8_t* op_len) {
    const char* current = lexer->source + lexer->position;
    uint32_t remaining = lexer->source_length - lexer->position;
    if (remaining == 0) return TOKEN_ERRORCODE;

    char c = current[0];
    /* Quick reject for non‑operator characters */
    switch (c) {
        case '+': case '-': case '*': case '/': case '%': case '=':
        case ':': case '.': case ',': case ';': case '(': case ')':
        case '[': case ']': case '{': case '}': case '&': case '@':
        case '~': case '$': case '^': case '?': case '!': case '>':
        case '<': case '|':
            break;
        default:
            return TOKEN_ERRORCODE;
    }

    /* Try lengths from 4 down to 1 */
    for (uint8_t len = 4; len > 0; len--) {
        if (remaining >= len) {
            TokenType type = lookup_symbol(current, len);
            if (type != TOKEN_IDENTIFIER) {
                *op_len = len;
                return type;
            }
        }
    }
    return TOKEN_ERRORCODE;
}

/*
 * Append a new token to the lexer's token list.
 * The token array is resized when needed.
 * Value is interned.
 */
static void add_token(Lexer* lexer, TokenType type,
                      const char* value, uint32_t len) {
    if (is_null(lexer)) return;

    if (lexer->token_count >= UINT64_MAX) {
        errhandler__report_error_ex(
            ERROR_LEVEL_ERROR,
            lexer->line, lexer->column, 1,
            "lexer",
            "Too many tokens (maximum exceeded)"
        );
        return;
    }

    if (lexer->token_count >= lexer->token_capacity) {
        const uint32_t new_capacity = lexer->token_capacity > UINT32_MAX / 2
            ? UINT32_MAX : lexer->token_capacity * 2;
        if (new_capacity <= lexer->token_capacity) {
            errhandler__report_error_ex(
                ERROR_LEVEL_ERROR,
                lexer->line, lexer->column, 1,
                "lexer",
                "Token array capacity overflow"
            );
            return;
        }

        Token* new_array = (Token*)memory_reallocate_zero(
            lexer->tokens,
            lexer->token_capacity * sizeof(Token),
            new_capacity * sizeof(Token)
        );
        if (is_null(new_array)) {
            errhandler__report_error_ex(
                ERROR_LEVEL_FATAL,
                lexer->line, lexer->column, 1,
                "lexer",
                "Memory allocation failed for token array"
            );
            return;
        }
        lexer->tokens = new_array;
        lexer->token_capacity = new_capacity;
    }

    Token* token = &lexer->tokens[lexer->token_count];
    memset(token, 0, sizeof(Token));

    token->type = (uint16_t)type;
    token->line = lexer->line;
    token->column = (len > 0 && lexer->column > len ? lexer->column - len : 1);
    token->length = len;

    if (!is_null(value)) {
        /* Intern the value (an empty value interns as the empty string) */
        const char* interned = intern_string_acquire(value, len);
        if (is_null(interned)) {
            errhandler__report_error_ex(
                ERROR_LEVEL_FATAL,
                lexer->line, lexer->column, 1,
                "lexer",
                "String interning failed"
            );
            return;
        }
        token->value = interned;
    } else {
        token->value = NULL;
    }

    lexer->token_count++;
}

/*
 * Initialise a new lexer.
 */
Lexer* lexer__init_lexer(const char* input) {
    static uint8_t symbol_table_initialized = 0;

    if (!symbol_table_initialized) {
        init_symbol_table();
        symbol_table_initialized = 1;
        atexit(free_symbol_table);
        atexit(free_intern_pool);
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
        errhandler__report_error_ex(
            ERROR_LEVEL_FATAL,
            0, 0, 1,
            "lexer",
            "Memory allocation failed for lexer tokens"
        );
        return NULL;
    }

    return lexer;
}

/*
 * Free a lexer and release all interned token values.
 */
void lexer__free_lexer(Lexer* lexer) {
    if (is_null(lexer)) return;

    if (!is_null(lexer->tokens)) {
        for (uint64_t i = 0; i < lexer->token_count; i++) {
            if (lexer->tokens[i].value) {
                intern_string_release(lexer->tokens[i].value);
            }
        }
        memory_free_safe((void**)&lexer->tokens);
    }

    memory_free_safe((void**)&lexer);
}

/*
 * Main tokenisation routine.
 */
void lexer__tokenize(Lexer* lexer) {
    const char* source = lexer->source;

    while (lexer->position < lexer->source_length) {
        skip_whitespace(lexer);
        if (lexer->position >= lexer->source_length) break;

        if (skip_comment(lexer)) {
            continue;
        }

        char c = source[lexer->position];

        /* String and character literals */
        if (c == '\'' || c == '"') {
            Token lit = literal__parse_concatenated(lexer);
            if (lit.type != TOKEN_ERRORCODE) {
                add_token(lexer, (TokenType)lit.type, lit.value, lit.length);
                intern_string_release(lit.value); /* release temporary copy */
            }
            continue;
        }

        /* Multi‑character operator */
        uint8_t op_len;
        TokenType op_type = lookup_operator(lexer, &op_len);
        if (op_type != TOKEN_ERRORCODE) {
            add_token(lexer, op_type, source + lexer->position, op_len);
            lexer->position += op_len;
            lexer->column += op_len;
            continue;
        }

        /* Identifier or keyword */
        if (u__char_is_identifier_start(c)) {
            uint32_t start = lexer->position;
            while (lexer->position < lexer->source_length &&
                   u__char_is_identifier_char(source[lexer->position])) {
                lexer->position++;
                lexer->column++;
            }
            uint32_t len = lexer->position - start;
            TokenType type = lookup_symbol(source + start, (uint8_t)len);
            add_token(lexer, type, source + start, len);
            continue;
        }

        /* Number literal (including floating‑point with leading dot) */
        if (u__char_is_digit(c) ||
            (c == '.' && lexer->position + 1 < lexer->source_length &&
             u__char_is_digit(source[lexer->position + 1]))) {
            Token num = literal__parse_number(lexer);
            if (num.type != TOKEN_ERRORCODE) {
                add_token(lexer, (TokenType)num.type, num.value, num.length);
                intern_string_release(num.value);
            }
            continue;
        }

        /* Unknown character – report error and skip */
        errhandler__report_error(
            lexer->line, lexer->column,
            "lexer",
            "Unexpected character in source code"
        );
        lexer->position++;
        lexer->column++;
    }

    /* Append EOF token */
    add_token(lexer, TOKEN_EOF, "", 0);
}
