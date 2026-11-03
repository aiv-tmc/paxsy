#include "common.h"
#include "token.h"

/*
 * token.c - tables derived from the token-kind list and keyword lookup.
 * Keeping the spelling table next to the X-macro guarantees that the
 * diagnostic output always names the token kind that exists in the enum.
 */

static const char *const kind_names[TK_COUNT] = {
#define PAXSY_EXPAND_NAME(name, spelling) spelling,
    PAXSY_TOKEN_KINDS(PAXSY_EXPAND_NAME)
#undef PAXSY_EXPAND_NAME
};

const char *token_kind_name(TokenKind kind) {
    if (kind < 0 || kind >= TK_COUNT) return "<invalid token>";
    return kind_names[kind];
}

/*
 * Keyword table. Lookup is a linear scan; the table is small and this is
 * called once per identifier during lexing.
 */
static const struct {
    const char *word;
    TokenKind kind;
} kKeywords[] = {
    { "let", TK_KW_LET },         { "func", TK_KW_FUNC },
    { "comptime", TK_KW_COMPTIME }, { "module", TK_KW_MODULE },
    { "use", TK_KW_USE },
    { "if", TK_KW_IF },           { "else", TK_KW_ELSE },
    { "while", TK_KW_WHILE },     { "do", TK_KW_DO },
    { "for", TK_KW_FOR },         { "match", TK_KW_MATCH },
    { "case", TK_KW_CASE },       { "break", TK_KW_BREAK },
    { "continue", TK_KW_CONTINUE }, { "pass", TK_KW_PASS },
    { "return", TK_KW_RETURN },   { "defer", TK_KW_DEFER },
    { "volatile", TK_KW_VOLATILE }, { "routine", TK_KW_ROUTINE },
    { "async", TK_KW_ASYNC },     { "await", TK_KW_AWAIT },
    { "select", TK_KW_SELECT },   { "on", TK_KW_ON },
    { "close", TK_KW_CLOSE },     { "chan", TK_KW_CHAN },
    { "as", TK_KW_AS },           { "and", TK_KW_AND },
    { "or", TK_KW_OR },           { "not", TK_KW_NOT },
    { "error", TK_KW_ERROR },     { "try", TK_KW_TRY },
    { "catch", TK_KW_CATCH },     { "alloc", TK_KW_ALLOC },
    { "free", TK_KW_FREE },       { "asm", TK_KW_ASM },
    { "Struct", TK_KW_STRUCT },   { "Union", TK_KW_UNION },
    { "Enum", TK_KW_ENUM },
    { NULL, TK_EOF }
};

TokenKind token_lookup_keyword(const char *text, int len) {
    for (int i = 0; kKeywords[i].word; i++) {
        const char *w = kKeywords[i].word;
        if ((int)strlen(w) == len && memcmp(w, text, (size_t)len) == 0)
            return kKeywords[i].kind;
    }
    return TK_IDENT;
}
