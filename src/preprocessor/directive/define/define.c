#include "define.h"
#include "../../preprocessor_state.h"
#include "../../../errhandler/errhandler.h"
#include "../../../utils/common.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/*
 * Forward declarations of internal helper functions.
 */
static ResultCode parse_object_like_macro(PreprocessorState *state, char *args);
static ResultCode parse_function_like_macro(PreprocessorState *state, char *name_start);
static char *extract_macro_name(char *str, size_t *name_len);
static char *skip_macro_whitespace(char *str);
static char *find_macro_replacement_end(char *str);
static ResultCode add_macro_to_table(PreprocessorState *state,
                                     char *name,
                                     char *value,
                                     int is_function_like,
                                     const char **params,
                                     size_t param_count,
                                     size_t name_offset);

/*
 * DPPF__define - Process a #define directive.
 *
 * The directive may define an object-like macro (no parameters) or a
 * function-like macro (with a parenthesised parameter list). Whitespace
 * between the macro name and the opening parenthesis of a function-like
 * macro is permitted.
 *
 * Parameters:
 *   state - The current preprocessor state, containing the macro table.
 *   args  - The raw argument string following the "define" keyword.
 */
void DPPF__define(PreprocessorState *state, char *args) {
    assert(state != NULL);
    assert(args != NULL);

    /* Skip leading whitespace after the directive keyword. */
    char *ptr = skip_macro_whitespace(args);
    if (is_empty(ptr)) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 6,   /* Position after "define". */
            "preproc",
            "Empty #define directive");
        return;
    }

    /*
     * Locate the macro name. The name consists of identifier characters only.
     * We advance `ptr` until the first non‑identifier character is found.
     */
    char *name_start = ptr;
    while (*ptr && u__char_is_identifier_char(*ptr)) {
        ptr++;
    }

    /*
     * Determine the macro type. After the name, whitespace may appear.
     * If the next non‑whitespace character is '(', the macro is function‑like.
     * Otherwise, it is an object‑like macro.
     */
    char *after_name = ptr;
    ptr = skip_macro_whitespace(ptr);
    ResultCode result;

    if (*ptr == '(') {
        result = parse_function_like_macro(state, name_start);
    } else {
        result = parse_object_like_macro(state, name_start);
    }

    if (result != RESULT_OK) {
        /* Error details have already been reported by the parsing routines. */
        return;
    }
}

/*
 * parse_object_like_macro - Parse a simple #define without parameters.
 *
 * Format: #define NAME [replacement text]
 *
 * The replacement text may be empty. Trailing whitespace is trimmed.
 *
 * Parameters:
 *   state - Preprocessor state.
 *   args  - Pointer to the start of the macro name.
 *
 * Returns:
 *   RESULT_OK on success, otherwise an error code.
 */
static ResultCode parse_object_like_macro(PreprocessorState *state, char *args) {
    size_t name_len;
    char *name = extract_macro_name(args, &name_len);
    if (name == NULL) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 7 + (int)(args - state->directive_buffer),
            "preproc",
            "Invalid macro name");
        return RESULT_INVALID_ARGUMENT;
    }

    /* Locate the replacement text. */
    char *value_start = skip_macro_whitespace(args + name_len);
    char *value_end = find_macro_replacement_end(value_start);

    /* Trim any trailing whitespace from the replacement text. */
    while (value_end > value_start && u__char_is_whitespace(*(value_end - 1))) {
        value_end--;
    }

    /* Allocate and copy the replacement text (may be empty). */
    size_t value_len = value_end - value_start;
    char *value = malloc(value_len + 1);
    if (value == NULL) {
        free(name);
        return RESULT_OUT_OF_MEMORY;
    }
    if (value_len > 0) {
        memcpy(value, value_start, value_len);
    }
    value[value_len] = '\0';

    /* Add the macro to the table. Object-like macros have no parameters. */
    ResultCode result = add_macro_to_table(state, name, value, 0, NULL, 0, name_len);

    free(name);
    free(value);
    return result;
}

/*
 * parse_function_like_macro - Parse a #define with a parameter list.
 *
 * Format: #define NAME( param1 , param2 , ... ) [replacement text]
 *
 * Whitespace is allowed everywhere except inside parameter names.
 * Variadic macros (using ...) are currently not supported and will cause
 * an error. An empty parameter list, e.g. #define FOO() value, is valid.
 *
 * Parameters:
 *   state      - Preprocessor state.
 *   name_start - Pointer to the beginning of the macro name.
 *
 * Returns:
 *   RESULT_OK on success, otherwise an error code.
 */
static ResultCode parse_function_like_macro(PreprocessorState *state, char *name_start) {
    size_t name_len;
    char *name = extract_macro_name(name_start, &name_len);
    if (name == NULL) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 7 + (int)(name_start - state->directive_buffer),
            "preproc",
            "Invalid macro name");
        return RESULT_INVALID_ARGUMENT;
    }

    /* Move past the name and any whitespace to find the opening parenthesis. */
    char *ptr = name_start + name_len;
    ptr = skip_macro_whitespace(ptr);
    if (*ptr != '(') {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 7 + (int)(ptr - state->directive_buffer),
            "preproc",
            "Expected '(' after macro name");
        free(name);
        return RESULT_INVALID_ARGUMENT;
    }
    ptr++;  /* Skip '(' */

    /*
     * Parse the parameter list. Parameters are separated by commas.
     * The list ends with a closing parenthesis.
     */
    char *param_names[MAX_MACRO_PARAMS];
    size_t param_count = 0;
    ResultCode result = RESULT_OK;

    while (*ptr && *ptr != ')') {
        /* Skip whitespace before the parameter name. */
        ptr = skip_macro_whitespace(ptr);
        if (*ptr == ')') {
            /* Reached the end of an empty parameter list. */
            break;
        }

        /* Variadic macros (...) are not implemented. */
        if (*ptr == '.' && ptr[1] == '.' && ptr[2] == '.') {
            errhandler__report_error(
                ERROR_CODE_PP_INVALID_DIR,
                state->directive_start_line,
                state->directive_start_column + 7 + (int)(ptr - state->directive_buffer),
                "preproc",
                "Variadic macros are not yet supported");
            result = RESULT_ERROR;
            break;
        }

        /* A parameter name must start with an identifier character. */
        if (!u__char_is_identifier_start(*ptr)) {
            errhandler__report_error(
                ERROR_CODE_PP_INVALID_DIR,
                state->directive_start_line,
                state->directive_start_column + 7 + (int)(ptr - state->directive_buffer),
                "preproc",
                "Invalid parameter name");
            result = RESULT_INVALID_ARGUMENT;
            break;
        }

        /* Extract the parameter name. */
        char *param_start = ptr;
        while (u__char_is_identifier_char(*ptr)) {
            ptr++;
        }
        size_t param_len = ptr - param_start;

        /* Allocate and store the parameter name. */
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
                MAX_MACRO_PARAMS);
            result = RESULT_INVALID_ARGUMENT;
            break;
        }

        /* Skip whitespace after the parameter name. */
        ptr = skip_macro_whitespace(ptr);

        /* Expect a comma or a closing parenthesis. */
        if (*ptr == ',') {
            ptr++;
        } else if (*ptr != ')') {
            errhandler__report_error(
                ERROR_CODE_PP_INVALID_DIR,
                state->directive_start_line,
                state->directive_start_column + 7 + (int)(ptr - state->directive_buffer),
                "preproc",
                "Expected ',' or ')' in parameter list");
            result = RESULT_INVALID_ARGUMENT;
            break;
        }
    }

    /* On error, free all resources allocated so far and return. */
    if (result != RESULT_OK) {
        free(name);
        for (size_t i = 0; i < param_count; i++) {
            free(param_names[i]);
        }
        return result;
    }

    /* Verify that the parameter list is properly closed. */
    if (*ptr != ')') {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 7 + (int)(ptr - state->directive_buffer),
            "preproc",
            "Expected ')' to close parameter list");
        free(name);
        for (size_t i = 0; i < param_count; i++) {
            free(param_names[i]);
        }
        return RESULT_INVALID_ARGUMENT;
    }
    ptr++;  /* Skip ')' */

    /* Locate the replacement text. */
    ptr = skip_macro_whitespace(ptr);
    char *value_start = ptr;
    char *value_end = find_macro_replacement_end(value_start);

    /* Trim trailing whitespace. */
    while (value_end > value_start && u__char_is_whitespace(*(value_end - 1))) {
        value_end--;
    }

    /* Allocate and copy the replacement text. */
    size_t value_len = value_end - value_start;
    char *value = malloc(value_len + 1);
    if (value == NULL) {
        free(name);
        for (size_t i = 0; i < param_count; i++) {
            free(param_names[i]);
        }
        return RESULT_OUT_OF_MEMORY;
    }
    if (value_len > 0) {
        memcpy(value, value_start, value_len);
    }
    value[value_len] = '\0';

    /*
     * Add the function-like macro to the table.  We pass the local
     * param_names array directly; macro_table_add will make its own deep
     * copies, so we can safely free the local strings afterwards.
     */
    result = add_macro_to_table(state, name, value, 1,
                                (const char**)param_names, param_count, name_len);

    free(name);
    free(value);
    /* Free the temporary parameter name strings (the table has its own copies). */
    for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
    }

    return result;
}

/*
 * add_macro_to_table - Helper to insert a macro and report errors.
 *
 * This function wraps macro_table_add() and, on failure, reports a
 * preprocessor error with the appropriate location information.
 *
 * Parameters:
 *   state           - Preprocessor state.
 *   name            - Macro name (ownership transferred on success).
 *   value           - Replacement text (ownership transferred on success).
 *   is_function_like- 1 for function-like, 0 for object-like.
 *   params          - Array of parameter name strings (deep-copied by table).
 *   param_count     - Number of parameters.
 *   name_offset     - Offset of the macro name within the directive line.
 *
 * Returns:
 *   RESULT_OK on success, RESULT_ERROR on failure.
 */
static ResultCode add_macro_to_table(PreprocessorState *state,
                                     char *name,
                                     char *value,
                                     int is_function_like,
                                     const char **params,
                                     size_t param_count,
                                     size_t name_offset) {
    if (macro_table_add(state->macro_table, name, value,
                        is_function_like, params, param_count)) {
        return RESULT_OK;
    }

    /* Failure – report the error and return. */
    errhandler__report_error(
        ERROR_CODE_PP_MACRO_DEF_FAILED,
        state->directive_start_line,
        state->directive_start_column + 7 + (int)name_offset,
        "preproc",
        "Failed to define macro: %s",
        name);
    return RESULT_ERROR;
}

/*
 * extract_macro_name - Extract a C identifier from the beginning of a string.
 *
 * The function verifies that the first character is a valid identifier start,
 * then consumes all subsequent identifier characters. The caller is
 * responsible for freeing the returned string.
 *
 * Parameters:
 *   str      - Input string.
 *   name_len - Output: length of the extracted name (excluding null).
 *
 * Returns:
 *   Newly allocated string containing the macro name, or NULL on error.
 */
static char *extract_macro_name(char *str, size_t *name_len) {
    assert(str != NULL);
    assert(name_len != NULL);

    if (!u__char_is_identifier_start(*str)) {
        return NULL;
    }

    char *end = str;
    while (u__char_is_identifier_char(*end)) {
        end++;
    }

    *name_len = end - str;

    char *name = malloc(*name_len + 1);
    if (name == NULL) {
        return NULL;
    }

    memcpy(name, str, *name_len);
    name[*name_len] = '\0';
    return name;
}

/*
 * skip_macro_whitespace - Advance past spaces and tabs.
 *
 * Newline characters are NOT skipped, because they mark the end of the
 * preprocessor directive line.
 *
 * Parameters:
 *   str - Input string pointer.
 *
 * Returns:
 *   Pointer to the first non-whitespace character.
 */
static char *skip_macro_whitespace(char *str) {
    if (str == NULL) {
        return str;
    }
    while (u__char_is_whitespace(*str)) {
        str++;
    }
    return str;
}

/*
 * find_macro_replacement_end - Locate the end of the macro replacement text.
 *
 * The replacement text continues until a newline character is encountered,
 * which signals the end of the preprocessor directive.
 *
 * Parameters:
 *   str - Start of the replacement text.
 *
 * Returns:
 *   Pointer to the first newline character (or to the terminating null).
 */
static char *find_macro_replacement_end(char *str) {
    if (str == NULL) {
        return str;
    }
    while (*str && !u__char_is_line_break(*str)) {
        str++;
    }
    return str;
}

/*
 * DPPF__undef - Process an #undef directive.
 *
 * The directive removes a previously defined macro. It is not an error
 * to undefine a macro that does not exist; a warning is emitted in that case.
 *
 * Parameters:
 *   state - Preprocessor state.
 *   args  - The raw argument string following the "undef" keyword.
 */
void DPPF__undef(PreprocessorState *state, char *args) {
    assert(state != NULL);
    assert(args != NULL);

    char *ptr = skip_macro_whitespace(args);
    if (is_empty(ptr)) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 5,   /* Position after "undef". */
            "preproc",
            "Empty #undef directive");
        return;
    }

    /* Extract the macro name. */
    char *name_start = ptr;
    while (u__char_is_identifier_char(*ptr)) {
        ptr++;
    }

    size_t name_len = ptr - name_start;
    if (name_len == 0) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 6,
            "preproc",
            "Invalid macro name in #undef");
        return;
    }

    /* Enforce maximum name length. */
    if (name_len >= MAX_MACRO_NAME_LEN) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 6,
            "preproc",
            "Macro name too long in #undef (maximum %d characters)",
            MAX_MACRO_NAME_LEN - 1);
        return;
    }

    /* Copy the name safely into a local buffer. */
    char name[MAX_MACRO_NAME_LEN];
    size_t copy_len = (name_len + 1 < MAX_MACRO_NAME_LEN) ? name_len + 1
                                                          : MAX_MACRO_NAME_LEN;
    u__str_copy_safe(name, name_start, copy_len);

    /* Ensure no extra characters appear after the macro name. */
    ptr = skip_macro_whitespace(ptr);
    if (*ptr != '\0' && !u__char_is_line_break(*ptr)) {
        errhandler__report_error(
            ERROR_CODE_PP_INVALID_DIR,
            state->directive_start_line,
            state->directive_start_column + 6 + (int)(ptr - args),
            "preproc",
            "Extra characters after macro name in #undef");
        return;
    }

    /* Attempt to remove the macro. Warn if it was not defined. */
    if (!macro_table_remove(state->macro_table, name)) {
        errhandler__report_error(
            ERROR_CODE_PP_UNDEFINED,
            state->directive_start_line,
            state->directive_start_column + 6,
            "preproc",
            "Undefining undefined macro: %s",
            name);
    }
}
