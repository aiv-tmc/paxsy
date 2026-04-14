#include "conditional.h"
#include "../define/macro.h"
#include "../../../errhandler/errhandler.h"
#include "../../../utils/char_utils.h"
#include "../../../utils/str_utils.h"
#include "../../../utils/memory_utils.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>   /* for fwrite in preprocessor_output stub */

/* Align size to multiple of alignment (power of two) */
#define ALIGN_SIZE(sz, align) (((sz) + (align) - 1) & ~((align) - 1))

/* Initial stack capacity for conditional frames */
#define INITIAL_STACK_CAPACITY 4

/* Maximum length of an identifier or numeric constant */
#define MAX_TOKEN_LEN 255

/* Forward declaration for preprocessor output function (now defined at bottom) */
void preprocessor_output(PreprocessorState *state, const char *data, size_t len);

ConditionalContext *conditional_context_create(void) {
    ConditionalContext *ctx = memory_allocate_zero(sizeof(ConditionalContext));
    if (!ctx) return NULL;

    ctx->capacity = INITIAL_STACK_CAPACITY;
    ctx->stack = memory_allocate_zero(sizeof(ConditionalFrame) * ctx->capacity);
    if (!ctx->stack) {
        free(ctx);
        return NULL;
    }
    ctx->count = 0;
    return ctx;
}

void conditional_context_destroy(ConditionalContext *ctx) {
    if (ctx) {
        free(ctx->stack);
        free(ctx);
    }
}

int conditional_should_output(PreprocessorState *state) {
    if (!state->conditional_ctx || state->conditional_ctx->count == 0)
        return 1;  /* Not inside any conditional block */
    ConditionalFrame *top = &state->conditional_ctx->stack[state->conditional_ctx->count - 1];
    return !top->skip;
}

/* Push a new frame, growing the stack if necessary */
static int conditional_push(PreprocessorState *state, ConditionalFrame frame) {
    ConditionalContext *ctx = state->conditional_ctx;
    if (!ctx) return 0;

    if (ctx->count >= ctx->capacity) {
        size_t new_cap = ctx->capacity * 2;
        size_t old_bytes = ctx->capacity * sizeof(ConditionalFrame);
        size_t new_bytes = new_cap * sizeof(ConditionalFrame);

        ConditionalFrame *new_stack = memory_reallocate_zero(ctx->stack, old_bytes, new_bytes);
        if (!new_stack) return 0;

        ctx->stack = new_stack;
        ctx->capacity = new_cap;
    }

    ctx->stack[ctx->count++] = frame;
    return 1;
}

/* Return the top frame, or NULL if stack is empty */
static ConditionalFrame *conditional_top(PreprocessorState *state) {
    ConditionalContext *ctx = state->conditional_ctx;
    if (!ctx || ctx->count == 0) return NULL;
    return &ctx->stack[ctx->count - 1];
}

/* Pop the top frame */
static void conditional_pop(PreprocessorState *state) {
    ConditionalContext *ctx = state->conditional_ctx;
    if (ctx && ctx->count > 0)
        ctx->count--;
}

typedef struct ExprParser {
    const char *p;           /* Current position */
    PreprocessorState *state;/* Preprocessor state (for macro lookup) */
    int error;               /* Non‑zero if a syntax or evaluation error occurred */
} ExprParser;

/* Forward declarations for recursive descent */
static int64_t parse_conditional(ExprParser *parser);
static int64_t parse_logical_or(ExprParser *parser);
static int64_t parse_logical_and(ExprParser *parser);
static int64_t parse_equality(ExprParser *parser);
static int64_t parse_relational(ExprParser *parser);
static int64_t parse_shift(ExprParser *parser);
static int64_t parse_additive(ExprParser *parser);
static int64_t parse_multiplicative(ExprParser *parser);
static int64_t parse_unary(ExprParser *parser);
static int64_t parse_primary(ExprParser *parser);
static int64_t parse_defined(ExprParser *parser);
static int64_t parse_number(ExprParser *parser);
static int64_t parse_identifier(ExprParser *parser);

/* Skip whitespace and newline characters */
static void skip_ws(ExprParser *parser) {
    while (u__char_is_whitespace(*parser->p) || *parser->p == '\n' || *parser->p == '\r')
        parser->p++;
}

/* Check next character after skipping whitespace */
static int peek_char(ExprParser *parser, char c) {
    skip_ws(parser);
    return *parser->p == c;
}

/* Check a two‑character operator after skipping whitespace */
static int peek_op2(ExprParser *parser, const char *op) {
    skip_ws(parser);
    return parser->p[0] == op[0] && parser->p[1] == op[1];
}

/* Consume a single character if it matches */
static int expect_char(ExprParser *parser, char c) {
    skip_ws(parser);
    if (*parser->p == c) {
        parser->p++;
        return 1;
    }
    return 0;
}

/* Consume a two‑character operator if it matches */
static int expect_op2(ExprParser *parser, const char *op) {
    skip_ws(parser);
    if (parser->p[0] == op[0] && parser->p[1] == op[1]) {
        parser->p += 2;
        return 1;
    }
    return 0;
}

static int evaluate_if_expression(const char *expr, PreprocessorState *state, int64_t *result) {
    ExprParser parser;
    parser.p = expr;
    parser.state = state;
    parser.error = 0;

    skip_ws(&parser);
    if (*parser.p == '\0') {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 state->line, state->column,
                                 "preproc", "Empty #if expression");
        return 0;
    }

    *result = parse_conditional(&parser);
    skip_ws(&parser);
    if (*parser.p != '\0' && !parser.error) {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 state->line, state->column,
                                 "preproc", "Trailing tokens in #if expression");
        return 0;
    }
    return !parser.error;
}

/* conditional ::= logical-or ( '?' conditional ':' conditional )? */
static int64_t parse_conditional(ExprParser *parser) {
    int64_t cond = parse_logical_or(parser);
    if (parser->error) return 0;

    if (expect_char(parser, '?')) {
        int64_t true_val = parse_conditional(parser);
        if (parser->error) return 0;
        if (!expect_char(parser, ':')) {
            errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                     parser->state->line, parser->state->column,
                                     "preproc", "Missing ':' in ternary operator");
            parser->error = 1;
            return 0;
        }
        int64_t false_val = parse_conditional(parser);
        if (parser->error) return 0;
        return cond ? true_val : false_val;
    }
    return cond;
}

static int64_t parse_logical_or(ExprParser *parser) {
    int64_t left = parse_logical_and(parser);
    if (parser->error) return 0;
    while (peek_op2(parser, "or")) {
        parser->p += 2;
        int64_t right = parse_logical_and(parser);
        if (parser->error) return 0;
        left = left || right;
    }
    return left;
}

static int64_t parse_logical_and(ExprParser *parser) {
    int64_t left = parse_equality(parser);
    if (parser->error) return 0;
    while (peek_op2(parser, "and")) {
        parser->p += 3;
        int64_t right = parse_equality(parser);
        if (parser->error) return 0;
        left = left && right;
    }
    return left;
}

/* equality ::= relational ( ('=='|'!=') relational )* */
static int64_t parse_equality(ExprParser *parser) {
    int64_t left = parse_relational(parser);
    if (parser->error) return 0;
    while (1) {
        if (peek_op2(parser, "==")) {
            parser->p += 2;
            int64_t right = parse_relational(parser);
            if (parser->error) return 0;
            left = left == right;
        } else if (peek_op2(parser, "!=")) {
            parser->p += 2;
            int64_t right = parse_relational(parser);
            if (parser->error) return 0;
            left = left != right;
        } else {
            break;
        }
    }
    return left;
}

/* relational ::= shift ( ('<'|'>'|'<='|'>=') shift )* */
static int64_t parse_relational(ExprParser *parser) {
    int64_t left = parse_shift(parser);
    if (parser->error) return 0;
    while (1) {
        if (peek_op2(parser, "<=")) {
            parser->p += 2;
            int64_t right = parse_shift(parser);
            if (parser->error) return 0;
            left = left <= right;
        } else if (peek_op2(parser, ">=")) {
            parser->p += 2;
            int64_t right = parse_shift(parser);
            if (parser->error) return 0;
            left = left >= right;
        } else if (expect_char(parser, '<')) {
            int64_t right = parse_shift(parser);
            if (parser->error) return 0;
            left = left < right;
        } else if (expect_char(parser, '>')) {
            int64_t right = parse_shift(parser);
            if (parser->error) return 0;
            left = left > right;
        } else {
            break;
        }
    }
    return left;
}

/* Perform a logical right shift (zero‑fill) */
static uint64_t logical_right_shift(uint64_t val, int shift) {
    if (shift <= 0) return val;
    if (shift >= 64) return 0;
    return val >> shift;
}

/* Perform an arithmetic right shift (sign‑extend) */
static int64_t arithmetic_right_shift(int64_t val, int shift) {
    if (shift <= 0) return val;
    if (shift >= 63) return (val < 0) ? -1 : 0;
    return val >> shift;
}

/* Rotate left (ROL) */
static uint64_t rotate_left(uint64_t val, int shift) {
    if (shift <= 0) return val;
    shift &= 63;  /* modulo 64 */
    if (shift == 0) return val;
    return (val << shift) | (val >> (64 - shift));
}

/* Rotate right (ROR) */
static uint64_t rotate_right(uint64_t val, int shift) {
    if (shift <= 0) return val;
    shift &= 63;  /* modulo 64 */
    if (shift == 0) return val;
    return (val >> shift) | (val << (64 - shift));
}

static int64_t parse_shift(ExprParser *parser) {
    int64_t left = parse_additive(parser);
    if (parser->error) return 0;

    while (1) {
        if (peek_op2(parser, "<<")) {
            parser->p += 2;
            int64_t right = parse_additive(parser);
            if (parser->error) return 0;
            left = left << right;
        } else if (peek_op2(parser, ">>")) {
            parser->p += 2;
            int64_t right = parse_additive(parser);
            if (parser->error) return 0;
            left = (int64_t)logical_right_shift((uint64_t)left, (int)right);
        } else if (u__str_startw(parser->p, "<<<")) {
            parser->p += 3;
            int64_t right = parse_additive(parser);
            if (parser->error) return 0;
            left = left << right;
        } else if (u__str_startw(parser->p, ">>>")) {
            parser->p += 3;
            int64_t right = parse_additive(parser);
            if (parser->error) return 0;
            left = arithmetic_right_shift(left, (int)right);
        } else if (u__str_startw(parser->p, "<<<<")) {
            parser->p += 4;
            int64_t right = parse_additive(parser);
            if (parser->error) return 0;
            left = (int64_t)rotate_left((uint64_t)left, (int)right);
        } else if (u__str_startw(parser->p, ">>>>")) {
            parser->p += 4;
            int64_t right = parse_additive(parser);
            if (parser->error) return 0;
            left = (int64_t)rotate_right((uint64_t)left, (int)right);
        } else {
            break;
        }
    }
    return left;
}

/* additive ::= multiplicative ( ('+'|'-') multiplicative )* */
static int64_t parse_additive(ExprParser *parser) {
    int64_t left = parse_multiplicative(parser);
    if (parser->error) return 0;
    while (1) {
        if (expect_char(parser, '+')) {
            int64_t right = parse_multiplicative(parser);
            if (parser->error) return 0;
            left = left + right;
        } else if (expect_char(parser, '-')) {
            int64_t right = parse_multiplicative(parser);
            if (parser->error) return 0;
            left = left - right;
        } else {
            break;
        }
    }
    return left;
}

/* multiplicative ::= unary ( ('*'|'/'|'%') unary )* */
static int64_t parse_multiplicative(ExprParser *parser) {
    int64_t left = parse_unary(parser);
    if (parser->error) return 0;
    while (1) {
        if (expect_char(parser, '*')) {
            int64_t right = parse_unary(parser);
            if (parser->error) return 0;
            left = left * right;
        } else if (expect_char(parser, '/')) {
            int64_t right = parse_unary(parser);
            if (parser->error) return 0;
            if (right == 0) {
                errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                         parser->state->line, parser->state->column,
                                         "preproc", "Division by zero in #if expression");
                parser->error = 1;
                return 0;
            }
            left = left / right;
        } else if (expect_char(parser, '%')) {
            int64_t right = parse_unary(parser);
            if (parser->error) return 0;
            if (right == 0) {
                errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                         parser->state->line, parser->state->column,
                                         "preproc", "Modulo by zero in #if expression");
                parser->error = 1;
                return 0;
            }
            left = left % right;
        } else {
            break;
        }
    }
    return left;
}

/* unary ::= ('+'|'-'|'!'|'~') unary | primary */
static int64_t parse_unary(ExprParser *parser) {
    skip_ws(parser);
    if (expect_char(parser, '+')) {
        return parse_unary(parser);
    } else if (expect_char(parser, '-')) {
        return -parse_unary(parser);
    } else if (expect_char(parser, '!')) {
        return !parse_unary(parser);
    } else if (expect_char(parser, '~')) {
        return ~parse_unary(parser);
    } else {
        return parse_primary(parser);
    }
}

/* primary ::= '(' conditional ')' | number | defined | identifier */
static int64_t parse_primary(ExprParser *parser) {
    skip_ws(parser);
    if (expect_char(parser, '(')) {
        int64_t val = parse_conditional(parser);
        skip_ws(parser);
        if (!expect_char(parser, ')')) {
            errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                     parser->state->line, parser->state->column,
                                     "preproc", "Missing ')' in expression");
            parser->error = 1;
            return 0;
        }
        return val;
    }
    if (u__char_is_digit(*parser->p)) {
        return parse_number(parser);
    }
    if (u__str_startw(parser->p, "defined")) {
        return parse_defined(parser);
    }
    if (u__char_is_identifier_start(*parser->p)) {
        return parse_identifier(parser);
    }
    errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                             parser->state->line, parser->state->column,
                             "preproc", "Unexpected character in expression");
    parser->error = 1;
    return 0;
}

/* defined ::= 'defined' identifier | 'defined' '(' identifier ')' */
static int64_t parse_defined(ExprParser *parser) {
    parser->p += 7;  /* skip "defined" */
    skip_ws(parser);

    int paren = expect_char(parser, '(');
    skip_ws(parser);

    if (!u__char_is_identifier_start(*parser->p)) {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 parser->state->line, parser->state->column,
                                 "preproc", "defined() requires an identifier");
        parser->error = 1;
        return 0;
    }
    const char *id_start = parser->p;
    while (u__char_is_identifier_char(*parser->p))
        parser->p++;
    size_t id_len = parser->p - id_start;
    char id[MAX_TOKEN_LEN + 1];
    if (id_len > MAX_TOKEN_LEN) id_len = MAX_TOKEN_LEN;
    memcpy(id, id_start, id_len);
    id[id_len] = '\0';

    if (paren) {
        skip_ws(parser);
        if (!expect_char(parser, ')')) {
            errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                     parser->state->line, parser->state->column,
                                     "preproc", "Missing ')' in defined()");
            parser->error = 1;
            return 0;
        }
    }

    const Macro *m = macro_table_find(parser->state->macro_table, id);
    return (m != NULL) ? 1 : 0;
}

/* Parse a decimal integer constant */
static int64_t parse_number(ExprParser *parser) {
    const char *start = parser->p;
    while (u__char_is_digit(*parser->p))
        parser->p++;
    size_t len = parser->p - start;
    char buf[MAX_TOKEN_LEN + 1];
    if (len > MAX_TOKEN_LEN) len = MAX_TOKEN_LEN;
    memcpy(buf, start, len);
    buf[len] = '\0';

    char *endptr;
    errno = 0;
    long long val = strtoll(buf, &endptr, 10);
    if (errno == ERANGE || (size_t)(endptr - buf) != len) {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 parser->state->line, parser->state->column,
                                 "preproc", "Integer constant overflow or invalid");
        parser->error = 1;
        return 0;
    }
    return (int64_t)val;
}

/* Parse an identifier and substitute its macro value if it expands to an integer */
static int64_t parse_identifier(ExprParser *parser) {
    const char *start = parser->p;
    while (u__char_is_identifier_char(*parser->p))
        parser->p++;
    size_t len = parser->p - start;
    char id[MAX_TOKEN_LEN + 1];
    if (len > MAX_TOKEN_LEN) len = MAX_TOKEN_LEN;
    memcpy(id, start, len);
    id[len] = '\0';

    const Macro *m = macro_table_find(parser->state->macro_table, id);
    if (!m || m->has_parameters) {
        return 0;  /* undefined or function‑like macro evaluates to 0 */
    }

    char *endptr;
    long long val = strtoll(m->value, &endptr, 0);
    if (endptr == m->value || *endptr != '\0') {
        return 0;  /* Not a valid integer token */
    }
    return (int64_t)val;
}

void DPPF__if(PreprocessorState *state, const char *args) {
    int parent_skip = !conditional_should_output(state);
    int64_t cond_val = 0;

    if (!evaluate_if_expression(args, state, &cond_val)) {
        cond_val = 0;  /* On error treat as false */
    }

    ConditionalFrame frame;
    frame.parent_skip = parent_skip;
    if (parent_skip) {
        frame.skip = 1;
        frame.taken = 0;
    } else {
        frame.skip = (cond_val == 0);
        frame.taken = (cond_val != 0);
    }
    frame.else_seen = 0;

    if (!conditional_push(state, frame)) {
        errhandler__report_error(ERROR_CODE_PP_MACRO_DEF_FAILED,
                                 state->line, state->column,
                                 "preproc", "Out of memory for conditional stack");
    }
}

void DPPF__elif(PreprocessorState *state, const char *args) {
    ConditionalFrame *top = conditional_top(state);
    if (!top) {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 state->line, state->column,
                                 "preproc", "#elif without matching #if");
        return;
    }
    if (top->else_seen) {
        errhandler__report_error(ERROR_CODE_PP_DUPLICATE_DIR,
                                 state->line, state->column,
                                 "preproc", "#elif after #else");
        return;
    }

    /* Determine parent skip status */
    int parent_skip = 0;
    if (state->conditional_ctx->count > 1) {
        ConditionalFrame *parent = &state->conditional_ctx->stack[state->conditional_ctx->count - 2];
        parent_skip = parent->skip;
    }

    if (parent_skip || top->taken) {
        top->skip = 1;
    } else {
        int64_t cond_val = 0;
        if (!evaluate_if_expression(args, state, &cond_val)) {
            cond_val = 0;
        }
        top->skip = (cond_val == 0);
        top->taken = (cond_val != 0);
    }
}

void DPPF__else(PreprocessorState *state, const char *args) {
    (void)args;  /* #else has no arguments */

    ConditionalFrame *top = conditional_top(state);
    if (!top) {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 state->line, state->column,
                                 "preproc", "#else without matching #if");
        return;
    }
    if (top->else_seen) {
        errhandler__report_error(ERROR_CODE_PP_DUPLICATE_DIR,
                                 state->line, state->column,
                                 "preproc", "Duplicate #else");
        return;
    }

    int parent_skip = 0;
    if (state->conditional_ctx->count > 1) {
        ConditionalFrame *parent = &state->conditional_ctx->stack[state->conditional_ctx->count - 2];
        parent_skip = parent->skip;
    }

    if (parent_skip) {
        top->skip = 1;
    } else {
        top->skip = top->taken;
        top->taken = 1;
    }
    top->else_seen = 1;
}

void DPPF__endif(PreprocessorState *state, const char *args) {
    (void)args;  /* #endif has no arguments */

    if (!conditional_top(state)) {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 state->line, state->column,
                                 "preproc", "#endif without matching #if");
        return;
    }
    conditional_pop(state);
}

/*
 * Recognizes an asm("...") call at the current position of the line.
 * If found, it writes the assembly text directly to the output buffer.
 * Returns 1 if an asm call was processed, 0 otherwise.
 */
int preprocessor_handle_asm(PreprocessorState *state, const char *line) {
    const char *p = line;

    /* Skip leading whitespace */
    while (u__char_is_whitespace(*p) || *p == '\n' || *p == '\r')
        p++;

    if (!u__str_startw(p, "asm"))
        return 0;

    p += 3;  /* skip "asm" */
    while (u__char_is_whitespace(*p) || *p == '\n' || *p == '\r')
        p++;

    if (*p != '(')
        return 0;
    p++;  /* skip '(' */

    /* Locate the closing quote */
    const char *start_quote = NULL;
    char quote_char = 0;
    if (*p == '"' || *p == '\'') {
        quote_char = *p;
        start_quote = p;
        p++;
    } else {
        /* No opening quote – not a valid asm call */
        return 0;
    }

    /* Find the matching closing quote */
    const char *end_quote = NULL;
    while (*p) {
        if (*p == quote_char && (p == start_quote || *(p-1) != '\\')) {
            end_quote = p;
            break;
        }
        p++;
    }
    if (!end_quote)
        return 0;

    p = end_quote + 1;

    /* Expect closing parenthesis */
    while (u__char_is_whitespace(*p) || *p == '\n' || *p == '\r')
        p++;
    if (*p != ')')
        return 0;

    /* The assembly text is between start_quote+1 and end_quote */
    size_t asm_len = end_quote - (start_quote + 1);
    char *asm_text = malloc(asm_len + 1);
    if (!asm_text)
        return 0;
    memcpy(asm_text, start_quote + 1, asm_len);
    asm_text[asm_len] = '\0';

    /* Output the assembly text using the preprocessor's output mechanism. */
    preprocessor_output(state, asm_text, asm_len);
    preprocessor_output(state, "\n", 1);

    free(asm_text);
    return 1;
}

/* --------------------------------------------------------------------------
 * Stub / fallback implementations for missing functions.
 * These are required to satisfy the linker. Replace with actual implementations
 * if they already exist elsewhere in the project.
 * -------------------------------------------------------------------------- */

/*
 * Output function for the preprocessor.
 * In a real implementation this would write to the output file or buffer.
 * Here it simply writes to stdout.
 */
void preprocessor_output(PreprocessorState *state, const char *data, size_t len) {
    (void)state;  /* unused */
    fwrite(data, 1, len, stdout);
}

/*
 * Handlers for #ifdef and #ifndef directives.
 * These should eventually be moved to a dedicated file (e.g., ifdef.c).
 */
void DPPF__ifdef(PreprocessorState *state, const char *args) {
    int parent_skip = !conditional_should_output(state);
    const Macro *m = macro_table_find(state->macro_table, args);
    int cond = (m != NULL);

    ConditionalFrame frame;
    frame.parent_skip = parent_skip;
    if (parent_skip) {
        frame.skip = 1;
        frame.taken = 0;
    } else {
        frame.skip = !cond;
        frame.taken = cond;
    }
    frame.else_seen = 0;

    conditional_push(state, frame);
}

void DPPF__ifndef(PreprocessorState *state, const char *args) {
    int parent_skip = !conditional_should_output(state);
    const Macro *m = macro_table_find(state->macro_table, args);
    int cond = (m == NULL);

    ConditionalFrame frame;
    frame.parent_skip = parent_skip;
    if (parent_skip) {
        frame.skip = 1;
        frame.taken = 0;
    } else {
        frame.skip = !cond;
        frame.taken = cond;
    }
    frame.else_seen = 0;

    conditional_push(state, frame);
}
