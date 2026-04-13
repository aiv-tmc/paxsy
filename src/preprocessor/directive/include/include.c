#include "include.h"
#include "../../preprocessor.h"
#include "../../preprocessor_state.h"
#include "../../../errhandler/errhandler.h"
#include "../../../utils/common.h"
#include <stdio.h>
#include <string.h>
#include <libgen.h>
#include <unistd.h>

/* Dynamically growing array of canonical paths of already included files. */
static char** included_files = NULL;
static size_t included_count = 0;
static size_t included_capacity = 0;

/*
 * Adds a file path to the inclusion registry if it is not already present.
 * The path is duplicated and stored permanently; duplicates are silently ignored.
 */
static void add_included_file(const char* path) {
    if (!path) return;

    /* Check for existing entry. */
    for (size_t i = 0; i < included_count; ++i) {
        if (u__streq(included_files[i], path)) return;
    }

    /* Grow the dynamic array if needed. */
    if (included_count >= included_capacity) {
        size_t new_cap = included_capacity == 0 ? 16 : included_capacity << 1;
        char** new_arr = (char**)memory_reallocate_zero(
            included_files,
            included_capacity * sizeof(char*),
            new_cap * sizeof(char*)
        );
        if (!new_arr) {
            errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                     "Failed to grow included files registry");
            return;
        }
        included_files = new_arr;
        included_capacity = new_cap;
    }

    /* Store a copy of the path. */
    char* copy = u__strduplic(path);
    if (copy) included_files[included_count++] = copy;
}

/*
 * Checks whether a given canonical path has already been included.
 * Returns 1 if yes, 0 otherwise.
 */
static int is_file_included(const char* path) {
    for (size_t i = 0; i < included_count; ++i) {
        if (u__streq(included_files[i], path)) return 1;
    }
    return 0;
}

/*
 * Frees the entire inclusion registry.
 * Should be called once at the end of preprocessing.
 */
void free_included_registry(void) {
    for (size_t i = 0; i < included_count; ++i) {
        memory_free_safe((void**)&included_files[i]);
    }
    memory_free_safe((void**)&included_files);
    included_count = included_capacity = 0;
}

/*
 * Checks if a file exists and is readable.
 * Returns non-zero if accessible, zero otherwise.
 */
static int file_exists(const char *path) {
    return access(path, R_OK) == 0;
}

/*
 * Builds an absolute or canonical path from a base file path and a relative path.
 * If base_path contains a directory component, it is used; otherwise the current
 * working directory is assumed. The returned string must be freed by the caller.
 */
static char *build_full_path(const char *base_path, const char *relative_path) {
    if (!relative_path) return NULL;

    /* Already absolute – just duplicate. */
    if (relative_path[0] == '/')
        return u__strduplic(relative_path);

    char *full = NULL;
    if (base_path && base_path[0] != '\0') {
        char *base_copy = u__strduplic(base_path);
        if (!base_copy) return NULL;
        char *dir = dirname(base_copy);   /* dirname may modify the string */
        if (dir && dir[0] != '\0' && !u__streq(dir, ".")) {
            size_t len = strlen(dir) + 1 + strlen(relative_path) + 1;
            full = (char*)memory_allocate_zero(len);
            if (full) {
                snprintf(full, len, "%s/%s", dir, relative_path);
            }
        }
        memory_free_safe((void**)&base_copy);
    }

    /* Fallback: no directory part – use relative_path as is. */
    if (!full) {
        full = u__strduplic(relative_path);
    }
    return full;
}

/*
 * Appends a string to the preprocessor output buffer.
 * Updates the line and column counters of the PreprocessorState.
 * Automatically grows the output buffer as needed.
 */
static void append_to_output(PreprocessorState *state, const char *content) {
    if (!content) return;

    size_t len = strlen(content);
    for (size_t i = 0; i < len; i++) {
        /* Ensure enough capacity. */
        if (state->output_pos + 1 >= state->output_capacity) {
            size_t new_cap = state->output_capacity << 1;
            char *new_out = (char*)memory_reallocate_zero(
                state->output,
                state->output_capacity,
                new_cap
            );
            if (!new_out) {
                errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION,
                                         state->directive_start_line,
                                         state->directive_start_column,
                                         "memory",
                                         "Out of memory during directive processing");
                return;
            }
            state->output = new_out;
            state->output_capacity = new_cap;
        }

        /* Write character and update position. */
        state->output[state->output_pos++] = content[i];
        if (content[i] == '\n') {
            state->line++;
            state->column = 1;
        } else {
            state->column++;
        }
    }
}

/*
 * Parses a filename argument from a directive (e.g., #import "foo.hp" or #import foo).
 * Handles optional double quotes and automatically appends the .hp extension if missing.
 * Returns a newly allocated string; caller must free it.
 */
static char *parse_filename_argument(const char *args, PreprocessorState *state) {
    /* Skip leading whitespace. */
    while (*args && u__char_is_whitespace(*args)) args++;
    if (*args == '\0') {
        errhandler__report_error(ERROR_CODE_PP_UNKNOW_DIR,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "preproc",
                                 "Expected filename after directive");
        return NULL;
    }

    const char *start = args;
    const char *end = NULL;

    /* Handle quoted filename. */
    if (*args == '"') {
        start = args + 1;
        end = strchr(start, '"');
        if (!end) {
            errhandler__report_error(ERROR_CODE_PP_UNKNOW_DIR,
                                     state->directive_start_line,
                                     state->directive_start_column,
                                     "preproc",
                                     "Missing closing quote in filename");
            return NULL;
        }
        args = end + 1;
    } else {
        /* Unquoted: read until whitespace or end. */
        while (*args && !u__char_is_whitespace(*args)) args++;
        end = args;
    }

    size_t len = end - start;
    if (len == 0) {
        errhandler__report_error(ERROR_CODE_PP_UNKNOW_DIR,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "preproc",
                                 "Empty filename");
        return NULL;
    }

    /* Allocate space for base name + possible extension. */
    char *base = (char*)memory_allocate_zero(len + 5);  /* +5 for ".hp" and null */
    if (!base) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "memory",
                                 "Out of memory processing directive");
        return NULL;
    }
    memcpy(base, start, len);
    base[len] = '\0';

    /* Append .hp extension if not already present. */
    if (!u__str_endw(base, ".hp")) {
        size_t new_len = len + 4;
        char *with_ext = (char*)memory_reallocate_zero(base, len + 1, new_len + 1);
        if (!with_ext) {
            memory_free_safe((void**)&base);
            errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION,
                                     state->directive_start_line,
                                     state->directive_start_column,
                                     "memory",
                                     "Out of memory adding .hp extension");
            return NULL;
        }
        base = with_ext;
        memcpy(base + len, ".hp", 4);  /* copies the dot, h, p, and null */
    }

    return base;
}

/*
 * Core routine to include a file:
 * - Checks existence, opens and reads the file.
 * - Registers the file to prevent cyclic inclusions.
 * - Recursively preprocesses the file's content.
 * Returns 0 on success, -1 on error (error already reported).
 */
static int include_file(PreprocessorState *state, const char *full_path,
                        int err_line, int err_col) {
    if (!file_exists(full_path)) {
        errhandler__report_error(ERROR_CODE_IO_FILE_NOT_FOUND,
                                 err_line, err_col, "file",
                                 "File '%s' not found", full_path);
        return -1;
    }

    FILE *file = fopen(full_path, "r");
    if (!file) {
        errhandler__report_error(ERROR_CODE_IO_READ,
                                 err_line, err_col, "file",
                                 "Cannot open file '%s'", full_path);
        return -1;
    }

    /* Determine file size. */
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(file);
        return 0;   /* empty file – nothing to include */
    }

    /* Read entire content into memory. */
    char *content = (char*)memory_allocate_zero(file_size + 1);
    if (!content) {
        fclose(file);
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION,
                                 err_line, err_col, "memory",
                                 "Out of memory reading file '%s'", full_path);
        return -1;
    }

    size_t bytes_read = fread(content, 1, file_size, file);
    content[bytes_read] = '\0';
    fclose(file);

    /* Register before recursive preprocessing to detect cycles. */
    add_included_file(full_path);

    int error = preprocess_content(state, content, full_path);
    memory_free_safe((void**)&content);
    return error;
}

/*
 * Searches for a library file in standard locations.
 * Returns a newly allocated path if found, otherwise NULL.
 */
static char *find_library_file(const char *libname, PreprocessorState *state) {
    char *full_path = NULL;

    /* 1. Same directory as the current file being preprocessed. */
    if (state->current_file && state->current_file[0] != '\0') {
        full_path = build_full_path(state->current_file, libname);
        if (full_path && file_exists(full_path))
            return full_path;
        memory_free_safe((void**)&full_path);
    }

    /* 2. Current working directory. */
    if (file_exists(libname))
        return u__strduplic(libname);

    /* 3. ./lib/ subdirectory. */
    char lib_path[1024];
    snprintf(lib_path, sizeof(lib_path), "lib/%s", libname);
    if (file_exists(lib_path))
        return u__strduplic(lib_path);

    /* 4. System‑wide installation paths (Linux only). */
    snprintf(lib_path, sizeof(lib_path), "/usr/lib/paxsy/incl/%s", libname);
    if (file_exists(lib_path))
        return u__strduplic(lib_path);

    snprintf(lib_path, sizeof(lib_path), "/usr/local/lib/paxsy/incl/%s", libname);
    if (file_exists(lib_path))
        return u__strduplic(lib_path);

    return NULL;
}

/*
 * Implementation of the #import directive.
 * The argument is a filename relative to the current file.
 * If the file has not been included before, it is opened and preprocessed.
 */
void DPPF__import(PreprocessorState *state, char *args) {
    char *relative = parse_filename_argument(args, state);
    if (!relative) return;

    char *full_path = build_full_path(state->current_file, relative);
    memory_free_safe((void**)&relative);
    if (!full_path) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "memory",
                                 "Out of memory building #import path");
        return;
    }

    /* Skip if already included. */
    if (is_file_included(full_path)) {
        memory_free_safe((void**)&full_path);
        return;
    }

    int err = include_file(state, full_path,
                           state->directive_start_line,
                           state->directive_start_column);
    memory_free_safe((void**)&full_path);
    if (err) return;   /* error already reported */
}

/*
 * Implementation of the #using directive.
 * The argument is a library base name (without path). The function searches
 * standard locations and includes the first matching .hp file.
 */
void DPPF__using(PreprocessorState *state, char *args) {
    char *libname = parse_filename_argument(args, state);
    if (!libname) return;

    char *libpath = find_library_file(libname, state);
    if (!libpath) {
        errhandler__report_error(ERROR_CODE_IO_FILE_NOT_FOUND,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "file",
                                 "Library file '%s' not found in standard locations",
                                 libname);
        memory_free_safe((void**)&libname);
        return;
    }
    memory_free_safe((void**)&libname);

    /* Avoid duplicate inclusion. */
    if (is_file_included(libpath)) {
        memory_free_safe((void**)&libpath);
        return;
    }

    int err = include_file(state, libpath,
                           state->directive_start_line,
                           state->directive_start_column);
    memory_free_safe((void**)&libpath);
    if (err) return;
}
