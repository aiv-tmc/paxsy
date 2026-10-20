#include "skipcom.h"
#include "../../interfaces/errhandler/errhandler.h"
#include "../../interfaces/utils/utils.h"
#include <string.h>
#include <stdlib.h>

/*
 * Lexical states for the preprocessor state machine.
 */
enum State {
    STATE_NORMAL = 0,
    STATE_SINGLE_COMMENT,
    STATE_MULTI_COMMENT,
    STATE_STRING,
    STATE_CHAR
};

/*
 * Preprocessor state.
 */
typedef struct {
    const char* input;
    size_t      pos;
    char*       output;
    size_t      out_len;
    size_t      out_cap;
    int         line;
    int         column;
    int         state;
} PreprocessorState;

/*
 * Ensure output buffer has at least 'needed' free bytes.
 * Returns 1 on success, 0 on allocation failure.
 */
static int ensure_output_capacity(PreprocessorState* state, size_t needed) {
    if (state->out_len + needed < state->out_cap) return 1;
    size_t new_cap = state->out_cap * 2;
    if (new_cap < state->out_len + needed) new_cap = state->out_len + needed + 1024;
    char* new_out = realloc(state->output, new_cap);
    if (!new_out) return 0;
    state->output = new_out;
    state->out_cap = new_cap;
    return 1;
}

/*
 * Append a single character to the output.
 */
static void add_char(PreprocessorState* state, char c) {
    if (ensure_output_capacity(state, 1)) {
        state->output[state->out_len++] = c;
    }
}

/*
 * Check for backslash‑newline line continuation at current position.
 */
static inline int is_line_continuation(PreprocessorState* state) {
    const char* inp = state->input;
    size_t p = state->pos;
    if (inp[p] == '\\') {
        char nxt = inp[p + 1];
        if (nxt == '\n') return 1;
        if (nxt == '\r' && inp[p + 2] == '\n') return 1;
    }
    return 0;
}

/*
 * Skip a backslash‑newline sequence (update line/column, do not output).
 */
static void handle_line_continuation(PreprocessorState* state) {
    char nxt = state->input[state->pos + 1];
    state->pos++;          /* consume backslash */
    state->column++;
    if (nxt == '\r' && state->input[state->pos + 1] == '\n') {
        state->pos += 2;   /* consume \r\n */
        state->line++;
        state->column = 1;
    } else if (nxt == '\n' || nxt == '\r') {
        state->pos++;       /* consume newline */
        state->line++;
        state->column = 1;
    }
}

/*
 * Process characters inside a single‑line comment.
 */
static void process_single_comment(PreprocessorState* state) {
    if (is_line_continuation(state)) {
        handle_line_continuation(state);
        return;
    }
    char cur = state->input[state->pos];
    if (cur == '\n') {
        add_char(state, '\n');
        state->pos++;
        state->line++;
        state->column = 1;
        state->state = STATE_NORMAL;
    } else {
        state->pos++;
        state->column++;
        /* comment characters are not output */
    }
}

/*
 * Process characters inside a multi‑line comment.
 */
static void process_multi_comment(PreprocessorState* state) {
    if (is_line_continuation(state)) {
        handle_line_continuation(state);
        return;
    }
    char cur = state->input[state->pos];
    char nxt = state->input[state->pos + 1];
    if (cur == '*' && nxt == '/') {
        state->pos += 2;
        state->column += 2;
        state->state = STATE_NORMAL;
    } else {
        if (cur == '\n') {
            state->line++;
            state->column = 1;
        } else {
            state->column++;
        }
        state->pos++;
    }
}

/*
 * Process characters inside a string literal.
 */
static void process_string(PreprocessorState* state) {
    char cur = state->input[state->pos];
    char nxt = state->input[state->pos + 1];
    add_char(state, cur);
    state->pos++;
    state->column++;
    if (cur == '\\' && nxt != '\0') {
        add_char(state, nxt);
        state->pos++;
        state->column++;
    } else if (cur == '"') {
        state->state = STATE_NORMAL;
    } else if (cur == '\n') {
        state->line++;
        state->column = 1;
    }
}

/*
 * Process characters inside a character literal.
 */
static void process_char(PreprocessorState* state) {
    char cur = state->input[state->pos];
    char nxt = state->input[state->pos + 1];
    add_char(state, cur);
    state->pos++;
    state->column++;
    if (cur == '\\' && nxt != '\0') {
        add_char(state, nxt);
        state->pos++;
        state->column++;
    } else if (cur == '\'') {
        state->state = STATE_NORMAL;
    } else if (cur == '\n') {
        state->line++;
        state->column = 1;
    }
}

/*
 * Process a character when in normal (non‑comment, non‑literal) state.
 */
static void process_normal(PreprocessorState* state) {
    char cur = state->input[state->pos];
    char nxt = state->input[state->pos + 1];
    if (cur == '/' && nxt == '/') {
        state->state = STATE_SINGLE_COMMENT;
        state->pos += 2;
        state->column += 2;
    } else if (cur == '/' && nxt == '*') {
        state->state = STATE_MULTI_COMMENT;
        state->pos += 2;
        state->column += 2;
    } else if (cur == '"') {
        add_char(state, '"');
        state->state = STATE_STRING;
        state->pos++;
        state->column++;
    } else if (cur == '\'') {
        add_char(state, '\'');
        state->state = STATE_CHAR;
        state->pos++;
        state->column++;
    } else {
        add_char(state, cur);
        if (cur == '\n') {
            state->line++;
            state->column = 1;
        } else {
            state->column++;
        }
        state->pos++;
    }
}

/*
 * Main preprocessing loop.
 * Returns 0 on success, 1 on allocation failure.
 */
static int preprocess_buffer(PreprocessorState* state) {
    while (state->input[state->pos] != '\0') {
        if (is_line_continuation(state)) {
            handle_line_continuation(state);
            continue;
        }
        switch (state->state) {
            case STATE_NORMAL:
                process_normal(state);
                break;
            case STATE_SINGLE_COMMENT:
                process_single_comment(state);
                break;
            case STATE_MULTI_COMMENT:
                process_multi_comment(state);
                break;
            case STATE_STRING:
                process_string(state);
                break;
            case STATE_CHAR:
                process_char(state);
                break;
        }
    }

    /* Check for unclosed multi‑line comment */
    if (state->state == STATE_MULTI_COMMENT) {
        errhandler__report_error(
            state->line, state->column,
            "preprocessor",
            "Unterminated multi‑line comment at end of file"
        );
    }

    if (ensure_output_capacity(state, 1)) {
        state->output[state->out_len] = '\0';
    }
    return (state->output == NULL) ? 1 : 0;
}

/*
 * Public entry point.
 */
char* preprocess(const char* input, const char* filename, int* error) {
    (void)filename;
    if (!input) {
        if (error) *error = 1;
        return NULL;
    }

    PreprocessorState state;
    memset(&state, 0, sizeof(state));
    state.input = input;
    state.pos = 0;
    state.line = 1;
    state.column = 1;
    state.state = STATE_NORMAL;

    size_t init_cap = strlen(input) + 1;
    state.output = malloc(init_cap);
    if (!state.output) {
        if (error) *error = 1;
        return NULL;
    }
    state.out_cap = init_cap;
    state.out_len = 0;

    int ret = preprocess_buffer(&state);
    if (ret != 0) {
        free(state.output);
        if (error) *error = 1;
        return NULL;
    }

    if (error) *error = 0;
    return state.output;
}
