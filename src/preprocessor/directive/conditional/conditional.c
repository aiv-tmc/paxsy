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

/**
 * @brief Creates a new conditional compilation context.
 *
 * Allocates and initializes a ConditionalContext structure with an empty stack.
 *
 * @return Pointer to newly created context, or NULL on allocation failure.
 */
ConditionalContext* conditional_context_create(void) {
    ConditionalContext* ctx = memory_allocate_zero(sizeof(ConditionalContext));
    if (!ctx) return NULL;

    ctx->capacity = 8;                                   /* initial stack capacity */
    ctx->stack = memory_allocate_zero(sizeof(ConditionalFrame) * ctx->capacity);
    if (!ctx->stack) {
        free(ctx);
        return NULL;
    }
    ctx->count = 0;
    return ctx;
}

/**
 * @brief Destroys a conditional compilation context and frees its resources.
 *
 * @param ctx Context to destroy (may be NULL).
 */
void conditional_context_destroy(ConditionalContext* ctx) {
    if (ctx) {
        free(ctx->stack);
        free(ctx);
    }
}

/**
 * @brief Checks whether the current preprocessor output should be emitted.
 *
 * The decision is based on the topmost conditional frame.
 *
 * @param state Preprocessor state.
 * @return 1 if output should be emitted, 0 otherwise.
 */
int conditional_should_output(PreprocessorState* state) {
    if (!state->conditional_ctx || state->conditional_ctx->count == 0)
        return 1;                                         /* not inside any conditional */
    ConditionalFrame* top = &state->conditional_ctx->stack[state->conditional_ctx->count - 1];
    return !top->skip;                                    /* emit if skip flag is false */
}

/**
 * @brief Pushes a new conditional frame onto the stack.
 *
 * Expands the stack capacity if necessary.
 *
 * @param state Preprocessor state.
 * @param frame Frame to push.
 * @return 1 on success, 0 on memory allocation failure.
 */
static int conditional_push(PreprocessorState* state, ConditionalFrame frame) {
    ConditionalContext* ctx = state->conditional_ctx;
    if (!ctx) return 0;

    if (ctx->count >= ctx->capacity) {
        /* Double the capacity using bitwise shift for efficiency */
        size_t old_size = ctx->capacity * sizeof(ConditionalFrame);
        size_t new_cap = ctx->capacity << 1;
        size_t new_size = old_size << 1;                  /* new_cap * sizeof(frame) = 2 * old_size */

        ConditionalFrame* new_stack = memory_reallocate_zero(
            ctx->stack,
            old_size,
            new_size
        );
        if (!new_stack) return 0;

        ctx->stack = new_stack;
        ctx->capacity = new_cap;
    }

    ctx->stack[ctx->count++] = frame;
    return 1;
}

/**
 * @brief Returns a pointer to the topmost conditional frame.
 *
 * @param state Preprocessor state.
 * @return Pointer to top frame, or NULL if stack is empty.
 */
static ConditionalFrame* conditional_top(PreprocessorState* state) {
    ConditionalContext* ctx = state->conditional_ctx;
    if (!ctx || ctx->count == 0) return NULL;
    return &ctx->stack[ctx->count - 1];
}

/**
 * @brief Pops the topmost conditional frame from the stack.
 *
 * @param state Preprocessor state.
 */
static void conditional_pop(PreprocessorState* state) {
    ConditionalContext* ctx = state->conditional_ctx;
    if (ctx && ctx->count > 0)
        ctx->count--;
}

/**
 * @brief Parser state structure for evaluating #if expressions.
 */
typedef struct ExprParser {
    const char* p;           /*!< Current position in the expression string */
    PreprocessorState* state;/*!< Preprocessor state (for macro lookup) */
    int error;               /*!< Non‑zero if a syntax or evaluation error occurred */
} ExprParser;

/* Forward declarations of recursive parser functions */
static int64_t parse_conditional(ExprParser* parser);
static int64_t parse_logical_or(ExprParser* parser);
static int64_t parse_logical_and(ExprParser* parser);
static int64_t parse_equality(ExprParser* parser);
static int64_t parse_relational(ExprParser* parser);
static int64_t parse_shift(ExprParser* parser);
static int64_t parse_additive(ExprParser* parser);
static int64_t parse_multiplicative(ExprParser* parser);
static int64_t parse_unary(ExprParser* parser);
static int64_t parse_primary(ExprParser* parser);
static int64_t parse_defined(ExprParser* parser);
static int64_t parse_number(ExprParser* parser);
static int64_t parse_identifier(ExprParser* parser);

/**
 * @brief Skips whitespace characters (space, tab, newline, carriage return).
 */
static void skip_ws(ExprParser* parser) {
    while (char_is_whitespace(*parser->p) || *parser->p == '\n' || *parser->p == '\r')
        parser->p++;
}

/**
 * @brief Checks whether the next character (after skipping whitespace) equals c.
 */
static int peek_char(ExprParser* parser, char c) {
    skip_ws(parser);
    return *parser->p == c;
}

/**
 * @brief Checks whether the next two characters (after skipping whitespace) match the given operator.
 */
static int peek_op2(ExprParser* parser, const char* op) {
    skip_ws(parser);
    return parser->p[0] == op[0] && parser->p[1] == op[1];
}

/**
 * @brief Consumes a single character if it matches c (after skipping whitespace).
 */
static int expect_char(ExprParser* parser, char c) {
    skip_ws(parser);
    if (*parser->p == c) {
        parser->p++;
        return 1;
    }
    return 0;
}

/**
 * @brief Consumes a two‑character operator if it matches op (after skipping whitespace).
 */
static int expect_op2(ExprParser* parser, const char* op) {
    skip_ws(parser);
    if (parser->p[0] == op[0] && parser->p[1] == op[1]) {
        parser->p += 2;
        return 1;
    }
    return 0;
}

/**
 * @brief Evaluates a constant expression for #if or #elif.
 *
 * Performs macro substitution (via parse_identifier) and handles the defined() operator.
 *
 * @param expr  The expression string (after the directive keyword).
 * @param state Preprocessor state.
 * @param result Pointer to store the evaluated integer value.
 * @return 1 on successful evaluation, 0 on error.
 */
static int evaluate_if_expression(const char* expr, PreprocessorState* state, int64_t* result) {
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

/**
 * @brief Parses a conditional expression (ternary operator ?:).
 *
 * Grammar: logical-or ( '?' conditional ':' conditional )?
 */
static int64_t parse_conditional(ExprParser* parser) {
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

/**
 * @brief Parses logical OR (||).
 */
static int64_t parse_logical_or(ExprParser* parser) {
    int64_t left = parse_logical_and(parser);
    if (parser->error) return 0;
    while (peek_op2(parser, "||")) {
        parser->p += 2;
        int64_t right = parse_logical_and(parser);
        if (parser->error) return 0;
        left = left || right;
    }
    return left;
}

/**
 * @brief Parses logical AND (&&).
 */
static int64_t parse_logical_and(ExprParser* parser) {
    int64_t left = parse_equality(parser);
    if (parser->error) return 0;
    while (peek_op2(parser, "&&")) {
        parser->p += 2;
        int64_t right = parse_equality(parser);
        if (parser->error) return 0;
        left = left && right;
    }
    return left;
}

/**
 * @brief Parses equality operators (==, !=).
 */
static int64_t parse_equality(ExprParser* parser) {
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
        } else
            break;
    }
    return left;
}

/**
 * @brief Parses relational operators (<, >, <=, >=).
 */
static int64_t parse_relational(ExprParser* parser) {
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
        } else
            break;
    }
    return left;
}

/**
 * @brief Parses shift operators (<<, >>).
 */
static int64_t parse_shift(ExprParser* parser) {
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
            left = left >> right;
        } else
            break;
    }
    return left;
}

/**
 * @brief Parses additive operators (+, -).
 */
static int64_t parse_additive(ExprParser* parser) {
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
        } else
            break;
    }
    return left;
}

/**
 * @brief Parses multiplicative operators (*, /, %).
 */
static int64_t parse_multiplicative(ExprParser* parser) {
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
        } else
            break;
    }
    return left;
}

/**
 * @brief Parses unary operators (+, -, !, ~).
 */
static int64_t parse_unary(ExprParser* parser) {
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

/**
 * @brief Parses primary expressions: parentheses, numbers, defined(), identifiers.
 */
static int64_t parse_primary(ExprParser* parser) {
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
    if (char_is_digit(*parser->p)) {
        return parse_number(parser);
    }
    if (str_startw(parser->p, "defined")) {
        return parse_defined(parser);
    }
    if (char_is_identifier_start(*parser->p)) {
        return parse_identifier(parser);
    }
    errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                             parser->state->line, parser->state->column,
                             "preproc", "Unexpected character in expression");
    parser->error = 1;
    return 0;
}

/**
 * @brief Parses the defined() operator.
 *
 * Syntax: defined identifier or defined ( identifier ).
 */
static int64_t parse_defined(ExprParser* parser) {
    if (!str_startw(parser->p, "defined")) {
        parser->error = 1;
        return 0;
    }
    parser->p += 7;               /* skip "defined" */
    skip_ws(parser);

    int paren = expect_char(parser, '(');
    skip_ws(parser);

    if (!char_is_identifier_start(*parser->p)) {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 parser->state->line, parser->state->column,
                                 "preproc", "defined() requires an identifier");
        parser->error = 1;
        return 0;
    }
    const char* id_start = parser->p;
    while (char_is_identifier_char(*parser->p))
        parser->p++;
    size_t id_len = parser->p - id_start;
    char id[256];
    if (id_len >= sizeof(id)) id_len = sizeof(id) - 1;
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

    const Macro* m = macro_table_find(parser->state->macro_table, id);
    return (m != NULL) ? 1 : 0;   /* defined macro -> 1, else 0 */
}

/**
 * @brief Parses an integer constant (decimal only, as per preprocessor restrictions).
 */
static int64_t parse_number(ExprParser* parser) {
    const char* start = parser->p;
    while (char_is_digit(*parser->p))
        parser->p++;
    size_t len = parser->p - start;
    char buf[64];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';

    char* endptr;
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

/**
 * @brief Parses an identifier, substitutes its macro value if it expands to an integer.
 *
 * If the macro is not defined, is function‑like, or its value is not a valid integer,
 * the identifier evaluates to 0.
 */
static int64_t parse_identifier(ExprParser* parser) {
    const char* start = parser->p;
    while (char_is_identifier_char(*parser->p))
        parser->p++;
    size_t len = parser->p - start;
    char id[256];
    if (len >= sizeof(id)) len = sizeof(id) - 1;
    memcpy(id, start, len);
    id[len] = '\0';

    const Macro* m = macro_table_find(parser->state->macro_table, id);
    if (!m || m->has_parameters)                /* undefined or function‑like macro */
        return 0;

    char* endptr;
    long long val = strtoll(m->value, &endptr, 0);
    if (endptr == m->value || *endptr != '\0')  /* not a valid integer token */
        return 0;

    return (int64_t)val;
}

/**
 * @brief Handles #if directive.
 *
 * Evaluates the constant expression; if true, the block is processed,
 * otherwise it is skipped.
 */
void DPPF__if(PreprocessorState* state, const char* args) {
    int parent_skip = !conditional_should_output(state);   /* whether enclosing block is skipped */

    int64_t cond_val = 0;
    if (!evaluate_if_expression(args, state, &cond_val)) {
        cond_val = 0;                                      /* on error, treat as false */
    }

    ConditionalFrame frame;
    frame.parent_skip = parent_skip;
    if (parent_skip) {
        frame.skip = 1;                                    /* inherit skip from parent */
        frame.taken = 0;
    } else {
        frame.skip = (cond_val == 0);                      /* skip if condition false */
        frame.taken = (cond_val != 0);                     /* mark taken if condition true */
    }
    frame.else_seen = 0;

    if (!conditional_push(state, frame)) {
        errhandler__report_error(ERROR_CODE_PP_MACRO_DEF_FAILED,
                                 state->line, state->column,
                                 "preproc", "Out of memory for conditional stack");
    }
}

/**
 * @brief Handles #ifdef directive.
 *
 * Tests whether a macro is defined.
 */
void DPPF__ifdef(PreprocessorState* state, const char* args) {
    int parent_skip = !conditional_should_output(state);

    /* Extract macro name */
    const char* p = args;
    while (char_is_whitespace(*p) || *p == '\n' || *p == '\r') p++;
    if (!char_is_identifier_start(*p)) {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 state->line, state->column,
                                 "preproc", "#ifdef requires an identifier");
        return;
    }
    const char* id_start = p;
    while (char_is_identifier_char(*p)) p++;
    size_t id_len = p - id_start;
    char id[256];
    if (id_len >= sizeof(id)) id_len = sizeof(id) - 1;
    memcpy(id, id_start, id_len);
    id[id_len] = '\0';

    int defined = (macro_table_find(state->macro_table, id) != NULL);

    ConditionalFrame frame;
    frame.parent_skip = parent_skip;
    if (parent_skip) {
        frame.skip = 1;
        frame.taken = 0;
    } else {
        frame.skip = !defined;                             /* skip if macro not defined */
        frame.taken = defined;                             /* taken if macro defined */
    }
    frame.else_seen = 0;
    conditional_push(state, frame);
}

/**
 * @brief Handles #ifndef directive.
 *
 * Tests whether a macro is not defined.
 */
void DPPF__ifndef(PreprocessorState* state, const char* args) {
    int parent_skip = !conditional_should_output(state);

    const char* p = args;
    while (char_is_whitespace(*p) || *p == '\n' || *p == '\r') p++;
    if (!char_is_identifier_start(*p)) {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 state->line, state->column,
                                 "preproc", "#ifndef requires an identifier");
        return;
    }
    const char* id_start = p;
    while (char_is_identifier_char(*p)) p++;
    size_t id_len = p - id_start;
    char id[256];
    if (id_len >= sizeof(id)) id_len = sizeof(id) - 1;
    memcpy(id, id_start, id_len);
    id[id_len] = '\0';

    int defined = (macro_table_find(state->macro_table, id) != NULL);

    ConditionalFrame frame;
    frame.parent_skip = parent_skip;
    if (parent_skip) {
        frame.skip = 1;
        frame.taken = 0;
    } else {
        frame.skip = defined;                              /* skip if macro defined */
        frame.taken = !defined;                            /* taken if macro not defined */
    }
    frame.else_seen = 0;
    conditional_push(state, frame);
}

/**
 * @brief Handles #elif directive.
 *
 * Introduces an alternative condition in an #if/#ifdef/#ifndef block.
 * Must not appear after #else.
 */
void DPPF__elif(PreprocessorState* state, const char* args) {
    ConditionalFrame* top = conditional_top(state);
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

    /* Determine whether the enclosing block (parent) is skipped */
    int parent_skip = 0;
    if (state->conditional_ctx->count > 1) {
        ConditionalFrame* parent = &state->conditional_ctx->stack[state->conditional_ctx->count - 2];
        parent_skip = parent->skip;
    }

    if (parent_skip || top->taken) {
        /* If parent is skipped or a previous branch was already taken, skip this #elif */
        top->skip = 1;
    } else {
        int64_t cond_val = 0;
        if (!evaluate_if_expression(args, state, &cond_val)) {
            cond_val = 0;
        }
        top->skip = (cond_val == 0);                       /* skip if condition false */
        top->taken = (cond_val != 0);                      /* mark taken if condition true */
    }
}

/**
 * @brief Handles #else directive.
 *
 * Marks the alternative branch of a conditional. Must appear only once per #if group.
 */
void DPPF__else(PreprocessorState* state, const char* args) {
    (void)args;                                            /* #else has no argument */

    ConditionalFrame* top = conditional_top(state);
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
        ConditionalFrame* parent = &state->conditional_ctx->stack[state->conditional_ctx->count - 2];
        parent_skip = parent->skip;
    }

    if (parent_skip) {
        top->skip = 1;                                     /* parent skipped → whole block skipped */
    } else {
        top->skip = top->taken;                            /* if a branch was taken, skip #else; otherwise process it */
        top->taken = 1;                                    /* mark that a branch (the #else) is now taken */
    }
    top->else_seen = 1;
}

/**
 * @brief Handles #endif directive.
 *
 * Closes the current conditional block.
 */
void DPPF__endif(PreprocessorState* state, const char* args) {
    (void)args;                                            /* #endif has no argument */

    if (!conditional_top(state)) {
        errhandler__report_error(ERROR_CODE_PP_INVALID_DIR,
                                 state->line, state->column,
                                 "preproc", "#endif without matching #if");
        return;
    }
    conditional_pop(state);
}
