#ifndef PAXSY_TOKEN_H
#define PAXSY_TOKEN_H

/*
 * token.h - token kinds and the token record produced by the lexer.
 *
 * The complete list of token kinds is written once as an X-macro so that
 * token.c can derive both the debug spelling table and the keyword lookup
 * from the same source of truth.
 *
 * Spelling conventions used in the table below:
 *   TK_KW_*   keywords of the Paxsy language
 *   TK_*_LIT  literal tokens carrying source text
 *   remaining entries are operators and punctuation
 */

#define PAXSY_TOKEN_KINDS(X)              \
    X(TK_EOF,         "end of file")      \
    X(TK_IDENT,        "identifier")      \
    X(TK_INT_LIT,      "integer literal") \
    X(TK_FLOAT_LIT,    "float literal")   \
    X(TK_STRING_LIT,   "string literal")  \
    X(TK_CHAR_LIT,     "char literal")    \
    X(TK_KW_LET,       "let")             \
    X(TK_KW_FUNC,      "func")            \
    X(TK_KW_COMPTIME,  "comptime")        \
    X(TK_KW_MODULE,    "module")          \
    X(TK_KW_USE,       "use")             \
    X(TK_KW_IF,        "if")              \
    X(TK_KW_ELSE,      "else")            \
    X(TK_KW_WHILE,     "while")           \
    X(TK_KW_DO,        "do")              \
    X(TK_KW_FOR,       "for")             \
    X(TK_KW_MATCH,     "match")           \
    X(TK_KW_CASE,      "case")            \
    X(TK_KW_BREAK,     "break")           \
    X(TK_KW_CONTINUE,  "continue")        \
    X(TK_KW_PASS,      "pass")            \
    X(TK_KW_RETURN,    "return")          \
    X(TK_KW_DEFER,     "defer")           \
    X(TK_KW_VOLATILE,  "volatile")        \
    X(TK_KW_ROUTINE,   "routine")         \
    X(TK_KW_ASYNC,     "async")           \
    X(TK_KW_AWAIT,     "await")           \
    X(TK_KW_SELECT,    "select")          \
    X(TK_KW_ON,        "on")              \
    X(TK_KW_CLOSE,     "close")           \
    X(TK_KW_CHAN,      "chan")            \
    X(TK_KW_AS,        "as")              \
    X(TK_KW_AND,       "and")             \
    X(TK_KW_OR,        "or")              \
    X(TK_KW_NOT,       "not")             \
    X(TK_KW_ERROR,     "error")           \
    X(TK_KW_TRY,       "try")             \
    X(TK_KW_CATCH,     "catch")           \
    X(TK_KW_ALLOC,     "alloc")           \
    X(TK_KW_FREE,      "free")            \
    X(TK_KW_ASM,       "asm")             \
    X(TK_KW_STRUCT,    "Struct")          \
    X(TK_KW_UNION,     "Union")           \
    X(TK_KW_ENUM,      "Enum")            \
    X(TK_LPAREN,       "(")               \
    X(TK_RPAREN,       ")")               \
    X(TK_LBRACE,       "{")               \
    X(TK_RBRACE,       "}")               \
    X(TK_LBRACKET,     "[")               \
    X(TK_RBRACKET,     "]")               \
    X(TK_COMMA,        ",")               \
    X(TK_SEMI,         ";")               \
    X(TK_COLON,        ":")               \
    X(TK_DOT,          ".")               \
    X(TK_COLONCOLON,   "::")              \
    X(TK_DOTDOT,       "..")              \
    X(TK_PIPE,         "|")               \
    X(TK_BANG,         "!")               \
    X(TK_PLUS,         "+")               \
    X(TK_MINUS,        "-")               \
    X(TK_STAR,         "*")               \
    X(TK_SLASH,        "/")               \
    X(TK_PERCENT,      "%")               \
    X(TK_ASSIGN,       "=")               \
    X(TK_PLUS_EQ,      "+=")              \
    X(TK_MINUS_EQ,     "-=")              \
    X(TK_STAR_EQ,     "*=")               \
    X(TK_SLASH_EQ,    "/=")               \
    X(TK_PERCENT_EQ,  "%=")               \
    X(TK_AMP_EQ,      "&=")               \
    X(TK_DOLLAR_EQ,   "$=")               \
    X(TK_CARET_EQ,    "^=")               \
    X(TK_SHL_EQ,      "<<=")              \
    X(TK_SHR_EQ,      ">>=")              \
    X(TK_USHR_EQ,     ">>>=")             \
    X(TK_EQ,          "==")               \
    X(TK_NE,          "!=")               \
    X(TK_LT,          "<")                \
    X(TK_GT,          ">")                \
    X(TK_LE,          "<=")               \
    X(TK_GE,          ">=")               \
    X(TK_AMP,         "&")                \
    X(TK_DOLLAR,      "$")                \
    X(TK_CARET,       "^")                \
    X(TK_TILDE,       "~")                \
    X(TK_SHL,         "<<")               \
    X(TK_SHR,         ">>")               \
    X(TK_USHR,        ">>>")              \
    X(TK_AT,          "@")                \
    X(TK_SENDRECV,    "<-")

/* Build the TokenKind enum from the X-macro list. */
#define PAXSY_EXPAND_ENUM(name, spelling) name,
typedef enum { PAXSY_TOKEN_KINDS(PAXSY_EXPAND_ENUM) TK_COUNT } TokenKind;
#undef PAXSY_EXPAND_ENUM

/*
 * A token is a slice of the source buffer; no text is copied.
 * line/col describe the position of the first character so that
 * diagnostics can point at the exact location.
 */
typedef struct {
    TokenKind kind;
    const char *start;
    int len;
    int line, col;
} Token;

/* Debug spelling of a token kind, used by --tokens and AST dumps. */
const char *token_kind_name(TokenKind kind);

/* Returns TK_IDENT when the text is not a keyword. */
TokenKind token_lookup_keyword(const char *text, int len);

#endif /* PAXSY_TOKEN_H */
