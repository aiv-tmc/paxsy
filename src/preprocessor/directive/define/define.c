#include "define.h"
#include "../../preprocessor_state.h"
#include "../../../errhandler/errhandler.h"
#include "../../../utils/common.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Forward declarations of internal helpers. */
static ResultCode parse_object_like_macro(PreprocessorState* state, char* args);
static ResultCode parse_function_like_macro(PreprocessorState* state, char* args);
static char* extract_macro_name(char* str, size_t* name_len);
static char* skip_macro_whitespace(char* str);
static char* find_macro_replacement_end(char* str);
static ResultCode add_macro_to_table(PreprocessorState* state, char* name,
                                      char* value, int is_function_like,
                                      char** params, size_t param_count,
                                      size_t name_offset);

/*
 * Process a #define directive.
 */
void DPPF__define(PreprocessorState* state, char* args) {
    assert(state != NULL);
    assert(args != NULL);

    /* Skip whitespace after the "define" keyword. */
    char* ptr = skip_macro_whitespace(args);
    if (is_empty(ptr)) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 6,   /* Position after "define". */
            "preproc",
            "Empty #define directive"
        );
        return;
    }

    /* Determine macro type: object-like or function-like. */
    char* name_start = ptr;
    while (*ptr && char_is_identifier_char(*ptr)) ptr++;

    ResultCode result = RESULT_ERROR;
    if (*ptr == '(') {
        result = parse_function_like_macro(state, name_start);
    } else {
        result = parse_object_like_macro(state, name_start);
    }

    if (result != RESULT_OK) {
        /* Error already reported by the parsing functions. */
        return;
    }
}

/*
 * Parse an object‑like macro (without parameters).
 * Format: #define NAME [replacement]
 */
static ResultCode parse_object_like_macro(PreprocessorState* state, char* args) {
    /* Extract macro name. */
    size_t name_len;
    char* name = extract_macro_name(args, &name_len);
    if (name == NULL) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 7 + (int)(args - state->directive_buffer),
            "preproc",
            "Invalid macro name"
        );
        return RESULT_INVALID_ARGUMENT;
    }

    /* Locate the replacement text. */
    char* value_start = skip_macro_whitespace(args + name_len);
    char* value_end = find_macro_replacement_end(value_start);

    /* Trim trailing whitespace. */
    while (value_end > value_start && char_is_whitespace(*(value_end - 1))) {
        value_end--;
    }

    /* Extract replacement text (may be empty). */
    size_t value_len = value_end - value_start;
    char* value;
    if (value_len > 0) {
        value = malloc(value_len + 1);
        if (value == NULL) {
            free(name);
            return RESULT_OUT_OF_MEMORY;
        }
        memcpy(value, value_start, value_len);
        value[value_len] = '\0';
    } else {
        value = malloc(1);
        if (value == NULL) {
            free(name);
            return RESULT_OUT_OF_MEMORY;
        }
        value[0] = '\0';
    }

    /* Add to macro table (object‑like → no parameters). */
    ResultCode result = add_macro_to_table(state, name, value, 0, NULL, 0, name_len);

    free(name);
    free(value);
    return result;
}

/*
 * Parse a function‑like macro (with parameters).
 * Format: #define NAME(param1, param2, ...) [replacement]
 */
static ResultCode parse_function_like_macro(PreprocessorState* state, char* args) {
    /* Extract macro name. */
    size_t name_len;
    char* name = extract_macro_name(args, &name_len);
    if (name == NULL) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 7 + (int)(args - state->directive_buffer),
            "preproc",
            "Invalid macro name"
        );
        return RESULT_INVALID_ARGUMENT;
    }

    /* Ensure '(' follows the name. */
    char* ptr = args + name_len;
    if (*ptr != '(') {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 7 + (int)(ptr - state->directive_buffer),
            "preproc",
            "Expected '(' after macro name"
        );
        free(name);
        return RESULT_INVALID_ARGUMENT;
    }
    ptr++;  /* Skip '(' */

    /* Parse parameter list. */
    char* param_names[MAX_MACRO_PARAMS];
    size_t param_count = 0;
    ResultCode result = RESULT_OK;

    while (*ptr && *ptr != ')') {
        /* Skip whitespace. */
        ptr = skip_macro_whitespace(ptr);
        if (*ptr == ')') break;

        /* Variadic macros (...) are not yet supported. */
        if (*ptr == '.' && ptr[1] == '.' && ptr[2] == '.') {
            errhandler__report_error(
                ERROR_CODE_PP_INVALID_DIR,
                state->directive_start_line,
                state->directive_start_column + 7 + (int)(ptr - state->directive_buffer),
                "preproc",
                "Variadic macros not yet supported"
            );
            result = RESULT_ERROR;
            break;
        }

        /* Parameter name must start with an identifier character. */
        if (!char_is_identifier_start(*ptr)) {
            errhandler__report_error(
                ERROR_CODE_PP_INVALID_DIR,
                state->directive_start_line,
                state->directive_start_column + 7 + (int)(ptr - state->directive_buffer),
                "preproc",
                "Invalid parameter name"
            );
            result = RESULT_INVALID_ARGUMENT;
            break;
        }

        /* Extract parameter name. */
        char* param_start = ptr;
        while (char_is_identifier_char(*ptr)) ptr++;
        size_t param_len = ptr - param_start;

        param_names[param_count] = malloc(param_len + 1);
        if (param_names[param_count] == NULL) {
            result = RESULT_OUT_OF_MEMORY;
            break;
        }
        memcpy(param_names[param_count], param_start, param_len);
        param_names[param_count][param_len] = '\0';
        param_count++;

        if (param_count >= MAX_MACRO_PARAMS) {
            errhandler__report_error(
                ERROR_CODE_PP_INVALID_DIR,
                state->directive_start_line,
                state->directive_start_column + 7 + (int)(ptr - state->directive_buffer),
                "preproc",
                "Too many macro parameters (maximum %d)",
                MAX_MACRO_PARAMS
            );
            result = RESULT_INVALID_ARGUMENT;
            break;
        }

        /* Skip whitespace after parameter. */
        ptr = skip_macro_whitespace(ptr);

        /* Expect either ',' or ')'. */
        if (*ptr == ',') {
            ptr++;
        } else if (*ptr != ')') {
            errhandler__report_error(
                ERROR_CODE_PP_INVALID_DIR,
                state->directive_start_line,
                state->directive_start_column + 7 + (int)(ptr - state->directive_buffer),
                "preproc",
                "Expected ',' or ')' in parameter list"
            );
            result = RESULT_INVALID_ARGUMENT;
            break;
        }
    }

    /* Clean up on error. */
    if (result != RESULT_OK) {
        free(name);
        for (size_t i = 0; i < param_count; i++) {
            free(param_names[i]);
        }
        return result;
    }

    /* Verify closing parenthesis. */
    if (*ptr != ')') {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 7 + (int)(ptr - state->directive_buffer),
            "preproc",
            "Expected ')' to close parameter list"
        );
        free(name);
        for (size_t i = 0; i < param_count; i++) {
            free(param_names[i]);
        }
        return RESULT_INVALID_ARGUMENT;
    }
    ptr++;  /* Skip ')' */

    /* Locate replacement text. */
    ptr = skip_macro_whitespace(ptr);
    char* value_start = ptr;
    char* value_end = find_macro_replacement_end(value_start);

    /* Trim trailing whitespace. */
    while (value_end > value_start && char_is_whitespace(*(value_end - 1))) {
        value_end--;
    }

    /* Extract replacement text (may be empty). */
    size_t value_len = value_end - value_start;
    char* value;
    if (value_len > 0) {
        value = malloc(value_len + 1);
        if (value == NULL) {
            free(name);
            for (size_t i = 0; i < param_count; i++) {
                free(param_names[i]);
            }
            return RESULT_OUT_OF_MEMORY;
        }
        memcpy(value, value_start, value_len);
        value[value_len] = '\0';
    } else {
        value = malloc(1);
        if (value == NULL) {
            free(name);
            for (size_t i = 0; i < param_count; i++) {
                free(param_names[i]);
            }
            return RESULT_OUT_OF_MEMORY;
        }
        value[0] = '\0';
    }

    /* Prepare parameter array for ownership transfer. */
    char** params = NULL;
    if (param_count > 0) {
        params = memory_duplicate(param_names, sizeof(char*) * param_count);
        if (params == NULL) {
            free(name);
            free(value);
            for (size_t i = 0; i < param_count; i++) {
                free(param_names[i]);
            }
            return RESULT_OUT_OF_MEMORY;
        }
    }

    /* Add to macro table. */
    result = add_macro_to_table(state, name, value, 1, params, param_count, name_len);

    /* Clean up temporary copies (the table now owns params if successful). */
    free(name);
    free(value);
    for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
    }

    return result;
}

/*
 * Helper to add a macro to the table, with error reporting.
 * The function takes ownership of 'name', 'value', and 'params' (if any).
 */
static ResultCode add_macro_to_table(PreprocessorState* state, char* name,
                                      char* value, int is_function_like,
                                      char** params, size_t param_count,
                                      size_t name_offset) {
    if (macro_table_add(state->macro_table, name, value, is_function_like,
                        params, param_count)) {
        return RESULT_OK;
    }

    /* Failure – report error. */
    errhandler__report_error(
        ERROR_CODE_PP_MACRO_DEF_FAILED,
        state->directive_start_line,
        state->directive_start_column + 7 + (int)name_offset,
        "preproc",
        "Failed to define macro: %s",
        name
    );

    return RESULT_ERROR;
}

/*
 * Extract a macro name from the beginning of a string.
 * The caller must free the returned string.
 */
static char* extract_macro_name(char* str, size_t* name_len) {
    assert(str != NULL);
    assert(name_len != NULL);

    if (!char_is_identifier_start(*str)) {
        return NULL;
    }

    char* end = str;
    while (char_is_identifier_char(*end)) end++;

    *name_len = end - str;

    char* name = malloc(*name_len + 1);
    if (name == NULL) return NULL;

    memcpy(name, str, *name_len);
    name[*name_len] = '\0';

    return name;
}

/*
 * Skip whitespace (space and tab only) in a macro definition.
 * Newlines are treated as end-of-directive and are not skipped here.
 */
static char* skip_macro_whitespace(char* str) {
    if (str == NULL) return str;
    while (char_is_whitespace(*str)) {
        str++;
    }
    return str;
}

/*
 * Find the end of the macro replacement text (before any newline).
 */
static char* find_macro_replacement_end(char* str) {
    if (str == NULL) return str;
    while (*str && !char_is_line_break(*str)) {
        str++;
    }
    return str;
}

/*
 * Process an #undef directive.
 */
void DPPF__undef(PreprocessorState* state, char* args) {
    assert(state != NULL);
    assert(args != NULL);

    char* ptr = skip_macro_whitespace(args);
    if (is_empty(ptr)) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 5,   /* Position after "undef". */
            "preproc",
            "Empty #undef directive"
        );
        return;
    }

    /* Extract macro name. */
    char* name_start = ptr;
    while (char_is_identifier_char(*ptr)) ptr++;

    size_t name_len = ptr - name_start;
    if (name_len == 0) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 6,
            "preproc",
            "Invalid macro name in #undef"
        );
        return;
    }

    /* Check name length limit. */
    if (name_len >= MAX_MACRO_NAME_LEN) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 6,
            "preproc",
            "Macro name too long in #undef (maximum %d characters)",
            MAX_MACRO_NAME_LEN - 1
        );
        return;
    }

    /* Copy name safely. */
    char name[MAX_MACRO_NAME_LEN];
    str_copy_safe(name, name_start, (name_len + 1 < MAX_MACRO_NAME_LEN) ? name_len + 1 : MAX_MACRO_NAME_LEN);

    /* Ensure no extra characters after the name. */
    ptr = skip_macro_whitespace(ptr);
    if (*ptr != '\0' && !char_is_line_break(*ptr)) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 6 + (int)(ptr - args),
            "preproc",
            "Extra characters after macro name in #undef"
        );
        return;
    }

    /* Remove the macro (warning if it did not exist). */
    if (!macro_table_remove(state->macro_table, name)) {
        errhandler__report_error(
            ERROR_CODE_PP_UNDEFINED,
            state->directive_start_line,
            state->directive_start_column + 6,
            "preproc",
            "Undefining undefined macro: %s",
            name
        );
    }
}
