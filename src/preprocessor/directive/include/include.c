#include "include.h"
#include "../../preprocessor.h"
#include "../../../errhandler/errhandler.h"
#include "../../../utils/common.h"
#include <stdio.h>
#include <string.h>

/** Dynamically growing array of included file paths. */
static char** included_files = NULL;
/** Number of files currently stored in the registry. */
static size_t included_count = 0;
/** Capacity of the included_files array. */
static size_t included_capacity = 0;

/**
 * @brief Adds a file path to the inclusion registry if not already present.
 * @param path Canonical path to the file.
 *
 * The function silently ignores NULL paths and duplicates.
 * If the registry array needs to grow, it uses a doubling strategy.
 */
static void add_included_file(const char* path) {
    if (!path) return;

    /* Check for duplicates */
    for (size_t i = 0; i < included_count; ++i) {
        if (streq(included_files[i], path))
            return;
    }

    /* Grow array if necessary (capacity doubles) */
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

    /* Store a copy of the path */
    char* copy = strduplic(path);
    if (copy)
        included_files[included_count++] = copy;
}

/**
 * @brief Checks whether a file has already been included.
 * @param path Canonical file path.
 * @return 1 if already included, 0 otherwise.
 */
static int is_file_included(const char* path) {
    for (size_t i = 0; i < included_count; ++i) {
        if (streq(included_files[i], path))
            return 1;
    }
    return 0;
}

/**
 * @brief Frees all memory allocated for the inclusion registry.
 *
 * This function is intended to be called during program shutdown.
 * It releases both the stored strings and the container array.
 */
void free_included_registry(void) {
    for (size_t i = 0; i < included_count; ++i)
        memory_free_safe((void**)&included_files[i]);
    memory_free_safe((void**)&included_files);
    included_count = included_capacity = 0;
}

/**
 * @brief Checks if a file exists and is readable.
 * @param path File system path.
 * @return 1 if file exists, 0 otherwise.
 */
static int file_exists(const char *path) {
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return 1; }
    return 0;
}

/**
 * @brief Constructs an absolute path from a base file path and a relative path.
 * @param base_path     Path of the currently processed file (may be NULL or empty).
 * @param relative_path Relative path to append (must be non‑NULL).
 * @return Newly allocated string containing the combined path, or NULL on failure.
 *
 * If base_path contains a directory part (i.e., a slash), it is used as the
 * directory; otherwise the relative_path is returned as is (or duplicated).
 */
static char *build_full_path(const char *base_path, const char *relative_path) {
    if (!relative_path) return NULL;

    if (base_path && base_path[0] != '\0') {
        const char *last_slash = strrchr(base_path, '/');
        if (last_slash) {
            size_t dir_len = last_slash - base_path + 1;  // include the '/'
            size_t full_len = dir_len + strlen(relative_path);
            char *full_path = (char*)memory_allocate_zero(full_len + 1);
            if (full_path) {
                memcpy(full_path, base_path, dir_len);
                memcpy(full_path + dir_len, relative_path, strlen(relative_path) + 1);
            }
            return full_path;
        }
    }
    /* No directory part in base_path – just duplicate the relative path */
    return strduplic(relative_path);
}

/**
 * @brief Appends a string to the preprocessor output buffer, updating line/column.
 * @param state   Preprocessor state containing the output buffer.
 * @param content String to append (may be NULL).
 *
 * The output buffer is dynamically resized (doubling capacity) when necessary.
 */
static void append_to_output(PreprocessorState *state, const char *content) {
    if (!content) return;

    size_t len = strlen(content);
    for (size_t i = 0; i < len; i++) {
        /* Ensure capacity (grow by factor 2) */
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

/**
 * @brief Extracts the file name from a quoted string and ensures a .hp extension.
 * @param args  Directive arguments (e.g. "mylib" or "sub/file.hp").
 * @param state Preprocessor state (for error reporting).
 * @return Newly allocated string with the .hp extension guaranteed, or NULL on error.
 *
 * The function parses a string enclosed in double quotes. If the extracted
 * part does not already end with ".hp", the suffix is appended automatically.
 * Memory for the returned string must be freed by the caller.
 */
static char *extract_quoted_path(const char *args, PreprocessorState *state) {
    const char *p = args;
    while (*p && char_is_whitespace(*p)) p++;

    if (*p != '"') {
        errhandler__report_error(ERROR_CODE_PP_UNKNOW_DIR,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "preproc",
                                 "Expected '\"' after directive");
        return NULL;
    }
    p++;  /* skip opening quote */

    const char *start = p;
    while (*p && *p != '"') p++;

    if (*p != '"') {
        errhandler__report_error(ERROR_CODE_PP_UNKNOW_DIR,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "preproc",
                                 "Missing closing '\"' in directive");
        return NULL;
    }

    size_t len = p - start;
    char *base = (char*)memory_allocate_zero(len + 5);  /* extra space for ".hp" */
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

    /* Add .hp extension if not already present */
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
        memcpy(base + len, ".hp", 4);  /* includes null terminator */
    }

    return base;
}

/**
 * @brief Handles the #import directive: includes a .hp file from a relative path.
 * @param state Preprocessor state.
 * @param args  Directive arguments (quoted file name, e.g. "sub/file").
 */
void DPPF__import(PreprocessorState *state, char *args) {
    /* Extract the file name (automatically appends .hp if missing) */
    char *relative = extract_quoted_path(args, state);
    if (!relative) return;

    /* Build full path relative to the current file's directory */
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

    /* Skip if already included */
    if (is_file_included(full_path)) {
        memory_free_safe((void**)&full_path);
        return;
    }

    /* Verify that the file exists */
    if (!file_exists(full_path)) {
        errhandler__report_error(ERROR_CODE_IO_FILE_NOT_FOUND,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "file",
                                 "File '%s' not found in #import directive",
                                 full_path);
        memory_free_safe((void**)&full_path);
        return;
    }

    /* Open and read the entire file */
    FILE *file = fopen(full_path, "r");
    if (!file) {
        errhandler__report_error(ERROR_CODE_IO_READ,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "file",
                                 "Cannot open file '%s' in #import directive",
                                 full_path);
        memory_free_safe((void**)&full_path);
        return;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(file);
        memory_free_safe((void**)&full_path);
        return;  /* empty file – nothing to include */
    }

    char *content = (char*)memory_allocate_zero(file_size + 1);
    if (!content) {
        fclose(file);
        memory_free_safe((void**)&full_path);
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "memory",
                                 "Out of memory reading file '%s'",
                                 full_path);
        return;
    }

    size_t bytes_read = fread(content, 1, file_size, file);
    content[bytes_read] = '\0';
    fclose(file);

    /* Register the file as included */
    add_included_file(full_path);

    /* Recursively preprocess the included content */
    int error = 0;
    char *processed = preprocess(content, full_path, &error);
    memory_free_safe((void**)&content);
    memory_free_safe((void**)&full_path);

    if (error) {
        memory_free_safe((void**)&processed);
        return;
    }

    if (processed) {
        append_to_output(state, processed);
        memory_free_safe((void**)&processed);
    }
}

/**
 * @brief Searches for a library file in standard system locations.
 * @param libname Base name of the library (without .hp, e.g. "mylib").
 * @param state   Preprocessor state (provides current file for relative lookup).
 * @return Newly allocated full path to the library file, or NULL if not found.
 *
 * Search order:
 *   1. Same directory as the currently processed file (if any).
 *   2. Current working directory (plain libname).
 *   3. ./lib/ subdirectory.
 *   4. System‑specific installation path (Windows, macOS, Linux/POSIX).
 */
static char *find_library_file(const char *libname, PreprocessorState *state) {
    char *full_path = NULL;

    /* 1. Relative to current file's directory */
    if (state->current_file && state->current_file[0] != '\0') {
        full_path = build_full_path(state->current_file, libname);
        if (full_path && file_exists(full_path))
            return full_path;
        memory_free_safe((void**)&full_path);
    }

    /* 2. Current directory */
    if (file_exists(libname))
        return strduplic(libname);

    /* 3. ./lib/ subdirectory */
    char lib_path[512];
    snprintf(lib_path, sizeof(lib_path), "lib/%s", libname);
    if (file_exists(lib_path))
        return strduplic(lib_path);

    /* 4. System‑specific installation paths */
#if PLATFORM_WINDOWS
    snprintf(lib_path, sizeof(lib_path), "C:\\Program Files\\paxsy\\lib\\incl\\%s", libname);
#elif PLATFORM_MACOS
    snprintf(lib_path, sizeof(lib_path), "/usr/local/lib/paxsy/incl/%s", libname);
#elif PLATFORM_LINUX || PLATFORM_POSIX
    snprintf(lib_path, sizeof(lib_path), "/usr/lib/paxsy/incl/%s", libname);
#else
    snprintf(lib_path, sizeof(lib_path), "./%s", libname);
#endif
    if (file_exists(lib_path))
        return strduplic(lib_path);

    return NULL;
}

/**
 * @brief Handles the #using directive: includes a library .hp file.
 * @param state Preprocessor state.
 * @param args  Directive arguments (quoted library name, e.g. "mylib").
 */
void DPPF__using(PreprocessorState *state, char *args) {
    /* Extract library name and automatically append .hp */
    char *libname = extract_quoted_path(args, state);
    if (!libname) return;

    /* Locate the library file in the standard search paths */
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

    /* Skip if already included */
    if (is_file_included(libpath)) {
        memory_free_safe((void**)&libname);
        memory_free_safe((void**)&libpath);
        return;
    }

    /* Open and read the library file */
    FILE *file = fopen(libpath, "r");
    if (!file) {
        errhandler__report_error(ERROR_CODE_IO_READ,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "file",
                                 "Cannot open library file '%s'",
                                 libpath);
        memory_free_safe((void**)&libname);
        memory_free_safe((void**)&libpath);
        return;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(file);
        memory_free_safe((void**)&libname);
        memory_free_safe((void**)&libpath);
        return;
    }

    char *content = (char*)memory_allocate_zero(file_size + 1);
    if (!content) {
        fclose(file);
        memory_free_safe((void**)&libname);
        memory_free_safe((void**)&libpath);
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION,
                                 state->directive_start_line,
                                 state->directive_start_column,
                                 "memory",
                                 "Out of memory reading library file '%s'",
                                 libpath);
        return;
    }

    size_t bytes_read = fread(content, 1, file_size, file);
    content[bytes_read] = '\0';
    fclose(file);

    /* Register the library file as included */
    add_included_file(libpath);

    /* Recursively preprocess the library content */
    int error = 0;
    char *processed = preprocess(content, libpath, &error);
    memory_free_safe((void**)&content);

    if (error) {
        memory_free_safe((void**)&processed);
        memory_free_safe((void**)&libname);
        memory_free_safe((void**)&libpath);
        return;
    }

    if (processed) {
        append_to_output(state, processed);
        memory_free_safe((void**)&processed);
    }

    memory_free_safe((void**)&libname);
    memory_free_safe((void**)&libpath);
}
