// include.c
#include "include.h"
#include "../../preprocessor.h"
#include "../../preprocessor_state.h"
#include "../../../errhandler/errhandler.h"
#include "../../../utils/common.h"
#include <stdio.h>
#include <string.h>
#include <libgen.h>   // for dirname
#include <unistd.h>   // for access

/** Dynamically growing array of already included files (canonical paths). */
static char** included_files = NULL;
static size_t included_count = 0;
static size_t included_capacity = 0;

// Add a path to the registry if not already present.
static void add_included_file(const char* path) {
    if (!path) return;
    for (size_t i = 0; i < included_count; ++i)
        if (streq(included_files[i], path)) return;

    if (included_count >= included_capacity) {
        size_t new_cap = included_capacity == 0 ? 16 : included_capacity << 1;
        char** new_arr = (char**)memory_reallocate_zero(included_files,
                                                        included_capacity * sizeof(char*),
                                                        new_cap * sizeof(char*));
        if (!new_arr) {
            errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                     "Failed to grow included files registry");
            return;
        }
        included_files = new_arr;
        included_capacity = new_cap;
    }
    char* copy = strduplic(path);
    if (copy) included_files[included_count++] = copy;
}

static int is_file_included(const char* path) {
    for (size_t i = 0; i < included_count; ++i)
        if (streq(included_files[i], path)) return 1;
    return 0;
}

void free_included_registry(void) {
    for (size_t i = 0; i < included_count; ++i)
        memory_free_safe((void**)&included_files[i]);
    memory_free_safe((void**)&included_files);
    included_count = included_capacity = 0;
}

// Check if file exists and is readable (Linux style).
static int file_exists(const char *path) {
    return access(path, R_OK) == 0;
}

// Build absolute path from a base file and a relative part.
// If base_path contains a directory part, it is used; otherwise the current
// working directory is used as fallback.
static char *build_full_path(const char *base_path, const char *relative_path) {
    if (!relative_path) return NULL;

    // If relative_path is already absolute, just duplicate it.
    if (relative_path[0] == '/')
        return strduplic(relative_path);

    char *full = NULL;
    if (base_path && base_path[0] != '\0') {
        char *base_copy = strduplic(base_path);
        if (!base_copy) return NULL;
        char *dir = dirname(base_copy);   // dirname may modify the string
        if (dir && dir[0] != '\0' && !streq(dir, ".")) {
            size_t len = strlen(dir) + 1 + strlen(relative_path) + 1;
            full = (char*)memory_allocate_zero(len);
            if (full) {
                snprintf(full, len, "%s/%s", dir, relative_path);
            }
        }
        memory_free_safe((void**)&base_copy);
    }

    if (!full) {
        // No directory part in base_path – use relative_path as is.
        full = strduplic(relative_path);
    }
    return full;
}

// Append a string to the preprocessor output buffer, updating line/column.
static void append_to_output(PreprocessorState *state, const char *content) {
    if (!content) return;

    size_t len = strlen(content);
    for (size_t i = 0; i < len; i++) {
        if (state->output_pos + 1 >= state->output_capacity) {
            size_t new_cap = state->output_capacity << 1;
            char *new_out = (char*)memory_reallocate_zero(state->output,
                                                          state->output_capacity,
                                                          new_cap);
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

        state->output[state->output_pos++] = content[i];
        if (content[i] == '\n') {
            state->line++;
            state->column = 1;
        } else {
            state->column++;
        }
    }
}

// Parse a filename argument (no quotes) and ensure .hp extension.
static char *parse_filename_argument(const char *args, PreprocessorState *state) {
    while (*args && char_is_whitespace(*args)) args++;
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
        while (*args && !char_is_whitespace(*args)) args++;
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

    char *base = (char*)memory_allocate_zero(len + 5);
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

    if (!str_endw(base, ".hp")) {
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
        memcpy(base + len, ".hp", 4);
    }

    return base;
}

// Common routine to include a file (check, read, register, preprocess).
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

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(file);
        return 0; // empty file – nothing to include
    }

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

    // Register before recursive preprocessing to detect cycles
    add_included_file(full_path);

    int error = preprocess_content(state, content, full_path);
    memory_free_safe((void**)&content);

    return error;
}

// Find a library file in standard Linux locations.
static char *find_library_file(const char *libname, PreprocessorState *state) {
    char *full_path = NULL;

    // 1. Same directory as current file
    if (state->current_file && state->current_file[0] != '\0') {
        full_path = build_full_path(state->current_file, libname);
        if (full_path && file_exists(full_path))
            return full_path;
        memory_free_safe((void**)&full_path);
    }

    // 2. Current working directory
    if (file_exists(libname))
        return strduplic(libname);

    // 3. ./lib/ subdirectory
    char lib_path[1024];
    snprintf(lib_path, sizeof(lib_path), "lib/%s", libname);
    if (file_exists(lib_path))
        return strduplic(lib_path);

    // 4. System-wide installation paths (Linux only)
    snprintf(lib_path, sizeof(lib_path), "/usr/lib/paxsy/incl/%s", libname);
    if (file_exists(lib_path))
        return strduplic(lib_path);

    snprintf(lib_path, sizeof(lib_path), "/usr/local/lib/paxsy/incl/%s", libname);
    if (file_exists(lib_path))
        return strduplic(lib_path);

    return NULL;
}

// #import directive: include a .hp file using a path relative to current file.
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

    if (is_file_included(full_path)) {
        memory_free_safe((void**)&full_path);
        return;
    }

    int err = include_file(state, full_path,
                           state->directive_start_line,
                           state->directive_start_column);
    memory_free_safe((void**)&full_path);
    if (err) return; // error already reported
}

// #using directive: include a library .hp file from standard locations.
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
