#include "DPPF__include.h"
#include "../preprocessor.h"
#include "../../errhandler/errhandler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/**
 * Duplicate a string
 * @param s: String to duplicate
 * @return: Duplicated string or NULL on error
 */
static char* strdup(const char* s) {
    if (!s) return NULL;
    
    size_t len = strlen(s) + 1;
    char* copy = malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

/**
 * Extract file path from #incfile directive arguments
 * @param args: Directive arguments string
 * @param state: Preprocessor state
 * @return: Extracted file path or NULL on error
 */
static char* extract_file_path(const char* args, PreprocessorState* state) {
    const char* p = args;
    
    /* Skip leading whitespace */
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* Check for opening quote */
    if (*p != '"') {
        errhandler__report_error
            ( ERROR_CODE_PP_UNKNOW_DIR
            , state->directive_start_line
            , state->directive_start_column
            , "preproc"
            , "Expected '\"' after #incfile directive"
        );
        return NULL;
    }
    p++;
    
    /* Extract path between quotes */
    const char* start = p;
    while (*p && *p != '"') p++;
    
    /* Check for closing quote */
    if (*p != '"') {
        errhandler__report_error
            ( ERROR_CODE_PP_UNKNOW_DIR
            , state->directive_start_line
            , state->directive_start_column
            , "preproc"
            , "Missing closing '\"' in #incfile directive"
        );
        return NULL;
    }
    
    size_t len = p - start;
    char* path = malloc(len + 1);
    if (!path) {
        errhandler__report_error
            ( ERROR_CODE_MEMORY_ALLOCATION
            , state->directive_start_line
            , state->directive_start_column
            , "memory"
            , "Out of memory processing #incfile"
        );
        return NULL;
    }
    
    memcpy(path, start, len);
    path[len] = '\0';
    return path;
}

/**
 * Build full path from base directory and relative path
 * @param base_path: Base file path
 * @param relative_path: Relative path to resolve
 * @return: Full path or NULL on error
 */
static char* build_full_path(const char* base_path, const char* relative_path) {
    char* full_path = NULL;
    
    if (base_path && base_path[0] != '\0') {
        const char* last_slash = strrchr(base_path, '/');
        
        if (last_slash) {
            /* Extract directory from base path */
            size_t dir_len = last_slash - base_path + 1;
            size_t rel_len = strlen(relative_path);
            full_path = malloc(dir_len + rel_len + 1);
            
            if (full_path) {
                memcpy(full_path, base_path, dir_len);
                strcpy(full_path + dir_len, relative_path);
            }
        } else {
            /* No directory in base path */
            size_t len = strlen(relative_path);
            full_path = malloc(len + 1);
            if (full_path)
                strcpy(full_path, relative_path);
        }
    } else {
        /* No base path */
        size_t len = strlen(relative_path);
        full_path = malloc(len + 1);
        if (full_path)
            strcpy(full_path, relative_path);
    }
    
    return full_path;
}

/**
 * Check if a file exists
 * @param path: File path to check
 * @return: 1 if file exists, 0 otherwise
 */
static int file_exists(const char* path) {
    struct stat buffer;
    return (stat(path, &buffer) == 0);
}

/**
 * Extract library name from #inclib directive arguments and add .hp extension
 * @param args: Directive arguments string
 * @param state: Preprocessor state
 * @return: Library name with .hp extension or NULL on error
 */
static char* extract_library_name(const char* args, PreprocessorState* state) {
    const char* p = args;
    
    /* Skip leading whitespace */
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* Check for opening quote */
    if (*p != '"') {
        errhandler__report_error
            ( ERROR_CODE_PP_UNKNOW_DIR
            , state->directive_start_line
            , state->directive_start_column
            , "preproc"
            , "Expected '\"' after #inclib directive"
        );
        return NULL;
    }
    p++;
    
    /* Extract library name between quotes */
    const char* start = p;
    while (*p && *p != '"') p++;
    
    /* Check for closing quote */
    if (*p != '"') {
        errhandler__report_error
            ( ERROR_CODE_PP_UNKNOW_DIR
            , state->directive_start_line
            , state->directive_start_column
            , "preproc"
            , "Missing closing '\"' in #inclib directive"
        );
        return NULL;
    }
    
    size_t name_len = p - start;
    
    /* Allocate memory for name + .hp extension + null terminator */
    char* libname = malloc(name_len + 4); /* +3 for ".hp" +1 for '\0' */
    if (!libname) {
        errhandler__report_error
            ( ERROR_CODE_MEMORY_ALLOCATION
            , state->directive_start_line
            , state->directive_start_column
            , "memory"
            , "Out of memory processing #inclib"
        );
        return NULL;
    }
    
    /* Copy name and append .hp extension */
    memcpy(libname, start, name_len);
    libname[name_len] = '.';
    libname[name_len + 1] = 'h';
    libname[name_len + 2] = 'p';
    libname[name_len + 3] = '\0';
    
    return libname;
}

/**
 * Detect current operating system
 * @return: OS code (1=Windows, 2=macOS, 3=Linux, 4=FreeBSD, 5=Unix, 0=Unknown)
 */
static uint8_t detect_os(void) {
    #if defined(_WIN32) || defined(_WIN64)
        return 1;
    #elif defined(__APPLE__) || defined(__MACH__)
        return 2;
    #elif defined(__linux__) || defined(__unix__) || defined(__unix) || defined(__FreeBSD__) || defined(__BSD__)
        return 3;
    #else
        return 0;
    #endif
}

/**
 * Find library file in standard locations
 * @param libname: Library name with .hp extension
 * @param state: Preprocessor state
 * @return: Full path to library or NULL if not found
 */
static char* find_library_file(const char* libname, PreprocessorState* state) {
    char* full_path = NULL;
    
    /* Check in same directory as current file */
    if (state->current_file && state->current_file[0] != '\0') {
        full_path = build_full_path(state->current_file, libname);
        if (full_path && file_exists(full_path)) {
            return full_path;
        }
        free(full_path);
    }
    
    /* Check in current directory */
    if (file_exists(libname)) {
        full_path = strdup(libname);
        return full_path;
    }
    
    /* Check in lib subdirectory */
    char lib_path[512];
    snprintf(lib_path, sizeof(lib_path), "lib/%s", libname);
    if (file_exists(lib_path)) {
        full_path = strdup(lib_path);
        return full_path;
    }
    
    /* Check OS-specific standard library paths */
    uint8_t os = detect_os();
    
    if (os == 1) { /* Windows */
        snprintf(lib_path, sizeof(lib_path), "C:\\Program Files\\lib\\%s", libname);
    } else if (os == 2) { /* macOS */
        snprintf(lib_path, sizeof(lib_path), "/usr/local/lib/%s", libname);
    } else if (os == 3 || os == 4 || os == 5) { /* Linux/FreeBSD/Unix */
        snprintf(lib_path, sizeof(lib_path), "/usr/lib/%s", libname);
    } else {
        snprintf(lib_path, sizeof(lib_path), "./%s", libname);
    }
    
    if (file_exists(lib_path)) {
        full_path = strdup(lib_path);
        return full_path;
    }
    
    return NULL;
}

/**
 * Process #inclib directive - include library file
 * @param state: Preprocessor state
 * @param args: Directive arguments
 */
void DPPF__inclib(PreprocessorState* state, char* args) {
    char* libname = extract_library_name(args, state);
    if (!libname) return;
    
    /* Find library file */
    char* libpath = find_library_file(libname, state);
    if (!libpath) {
        errhandler__report_error
            ( ERROR_CODE_IO_FILE_NOT_FOUND
            , state->directive_start_line
            , state->directive_start_column
            , "file"
            , "Library file '%s' not found in standard locations"
            , libname
        );
        free(libname);
        return;
    }
    
    /* Read and process the library file */
    FILE* file = fopen(libpath, "r");
    if (!file) {
        errhandler__report_error
            ( ERROR_CODE_IO_READ
            , state->directive_start_line
            , state->directive_start_column
            , "file"
            , "Cannot open library file '%s'"
            , libpath
        );
        free(libname);
        free(libpath);
        return;
    }
    
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (file_size <= 0) {
        fclose(file);
        free(libname);
        free(libpath);
        return;
    }
    
    char* content = malloc(file_size + 1);
    if (!content) {
        fclose(file);
        free(libname);
        free(libpath);
        errhandler__report_error
            ( ERROR_CODE_MEMORY_ALLOCATION
            , state->directive_start_line
            , state->directive_start_column
            , "memory"
            , "Out of memory reading library file '%s'"
            , libpath
        );
        return;
    }
    
    size_t read = fread(content, 1, file_size, file);
    content[read] = '\0';
    fclose(file);
    
    /* Generate OS-specific include directive */
    uint8_t os = detect_os();
    char include_directive[512];
    
    if (os == 1) {
        snprintf
            ( include_directive
            , sizeof(include_directive)
            , "windows: Link with library '%s'\n"
            , libname
        );
    } else if (os == 2) {
        snprintf
            ( include_directive
            , sizeof(include_directive)
            , "macOS: Link with library '%s'\n"
            , libname
        );
    } else if (os == 3) {
        snprintf
            ( include_directive
            , sizeof(include_directive)
            , "UNIX-Like: Link with library '%s'\n"
            , libname
        );
    } else {
        snprintf
            ( include_directive
            , sizeof(include_directive)
            , "Unknown OS: Link with library '%s'\n"
            , libname
        );
    }
    
    /* Add include directive to output */
    size_t len = strlen(include_directive);
    for (size_t i = 0; i < len; i++) {
        if (state->output_pos + 1 >= state->output_capacity) {
            size_t new_capacity = state->output_capacity * 2;
            char* new_output = realloc(state->output, new_capacity);
            if (!new_output) {
                errhandler__report_error
                    ( ERROR_CODE_MEMORY_ALLOCATION
                    , state->directive_start_line
                    , state->directive_start_column
                    , "memory"
                    , "Out of memory during #inclib processing"
                );
                free(content);
                free(libname);
                free(libpath);
                return;
            }
            state->output = new_output;
            state->output_capacity = new_capacity;
        }
        state->output[state->output_pos++] = include_directive[i];
        
        if (include_directive[i] == '\n') {
            state->line++;
            state->column = 1;
        } else
            state->column++;
    }
    
    /* Preprocess and include library content */
    int error = 0;
    char* processed = preprocess(content, libpath, &error);
    free(content);
    
    if (error) {
        free(processed);
        free(libname);
        free(libpath);
        return;
    }
    
    if (processed) {
        size_t len = strlen(processed);
        if (len > 0) {
            for (size_t i = 0; i < len; i++) {
                if (state->output_pos + 1 >= state->output_capacity) {
                    size_t new_capacity = state->output_capacity * 2;
                    char* new_output = realloc(state->output, new_capacity);
                    if (!new_output) {
                        errhandler__report_error
                            ( ERROR_CODE_MEMORY_ALLOCATION
                            , state->directive_start_line
                            , state->directive_start_column
                            , "memory"
                            , "Out of memory during library content processing"
                        );
                        break;
                    }
                    state->output = new_output;
                    state->output_capacity = new_capacity;
                }
                state->output[state->output_pos++] = processed[i];
                
                if (processed[i] == '\n') {
                    state->line++;
                    state->column = 1;
                } else {
                    state->column++;
                }
            }
        }
        free(processed);
    }
    
    free(libname);
    free(libpath);
}

/**
 * Process #incfile directive - include file content
 * @param state: Preprocessor state
 * @param args: Directive arguments
 */
void DPPF__incfile(PreprocessorState* state, char* args) {
    char* relative_path = extract_file_path(args, state);
    if (!relative_path) return;
    
    char* full_path = build_full_path(state->current_file, relative_path);
    if (!full_path) {
        free(relative_path);
        errhandler__report_error
            ( ERROR_CODE_MEMORY_ALLOCATION
            , state->directive_start_line
            , state->directive_start_column
            , "memory"
            , "Out of memory processing #incfile path"
        );
        return;
    }
    
    /* Check if file exists */
    if (!file_exists(full_path)) {
        errhandler__report_error
            ( ERROR_CODE_IO_FILE_NOT_FOUND
            , state->directive_start_line
            , state->directive_start_column
            , "file"
            , "File '%s' not found in #incfile directive"
            , full_path
        );
        free(relative_path);
        free(full_path);
        return;
    }
    
    FILE* file = fopen(full_path, "r");
    if (!file) {
        errhandler__report_error
            ( ERROR_CODE_IO_READ
            , state->directive_start_line
            , state->directive_start_column
            , "file"
            , "Cannot open file '%s' in #incfile directive"
            , full_path
        );
        free(relative_path);
        free(full_path);
        return;
    }
    
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (file_size <= 0) {
        fclose(file);
        free(relative_path);
        free(full_path);
        return;
    }
    
    char* content = malloc(file_size + 1);
    if (!content) {
        fclose(file);
        free(relative_path);
        free(full_path);
        errhandler__report_error
            ( ERROR_CODE_MEMORY_ALLOCATION
            , state->directive_start_line
            , state->directive_start_column
            , "memory"
            , "Out of memory reading file '%s'"
            , full_path
        );
        return;
    }
    
    size_t read = fread(content, 1, file_size, file);
    content[read] = '\0';
    fclose(file);
    
    int error = 0;
    char* processed = preprocess(content, full_path, &error);
    free(content);
    
    if (error) {
        free(processed);
        free(relative_path);
        free(full_path);
        return;
    }
    
    if (processed) {
        size_t len = strlen(processed);
        if (len > 0) {
            for (size_t i = 0; i < len; i++) {
                if (state->output_pos + 1 >= state->output_capacity) {
                    size_t new_capacity = state->output_capacity * 2;
                    char* new_output = realloc(state->output, new_capacity);
                    if (!new_output) {
                        errhandler__report_error
                            ( ERROR_CODE_MEMORY_ALLOCATION
                            , state->directive_start_line
                            , state->directive_start_column
                            , "memory"
                            , "Out of memory during #incfile processing"
                        );
                        break;
                    }
                    state->output = new_output;
                    state->output_capacity = new_capacity;
                }
                state->output[state->output_pos++] = processed[i];
                
                if (processed[i] == '\n') {
                    state->line++;
                    state->column = 1;
                } else {
                    state->column++;
                }
            }
        }
        free(processed);
    }
    
    free(relative_path);
    free(full_path);
}
