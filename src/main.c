/*
 * paxsy is a compiler for the paxsy-lang language of the same name.
 *
 * Copyright (C) 2026 AIV <https://github.com/aiv-tmc>
 * Distributed under the MIT License (see LICENSE file).
 *
 * Version: beta 4 Rowan v0.4.2a - 2026APR17
 * Usage: paxsy [operations] <output> <source> ...
 *
 * main.c – Entry point and driver for the entire compilation pipeline.
 * This file parses command line arguments, coordinates the different
 * phases (preprocessing, lexing, parsing, semantic analysis, optimization,
 * code generation) and handles error reporting.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Internal compiler headers */
#include "preprocessor/preprocessor.h"
#include "preprocessor/defmacros/defmacros.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "output/output.h"
#include "semantic/semantic.h"
#include "optimizer/optimizer.h"
#include "errhandler/errhandler.h"

/* Utility functions */
#include "utils/str_utils.h"
#include "utils/char_utils.h"
#include "utils/memory_utils.h"
#include "utils/common.h"

/*
 * If the build system does not define these macros, use placeholders
 * so that -version still shows something meaningful.
 */
#ifndef GENERATION
#define GENERATION "missing"
#endif
#ifndef NAME
#define NAME "missing"
#endif
#ifndef VERSION
#define VERSION "missing"
#endif
#ifndef DATE
#define DATE "missing"
#endif

/* Default library and include paths embedded by the build system (Makefile) */
#ifndef PAXSY_LIBRARY_DIR
#define PAXSY_LIBRARY_DIR "/usr/local/lib/paxsy"
#endif
#ifndef PAXSY_INCLUDE_DIR
#define PAXSY_INCLUDE_DIR "/usr/local/include/paxsy"
#endif

/*
 * FlagSet – a bitmask that holds all boolean flags obtained from the command
 * line. Each bit corresponds to a distinct mode or option.
 */
typedef uint32_t FlagSet;

/* Flag values – powers of two so they can be combined with bitwise OR. */
enum {
    F_MODE_COMPILE       = 1U << 0,  /* Compile into a binary executable         */
    F_DEBUG_PREPROCESS   = 1U << 1,  /* Dump preprocessed code (TODO)            */
    F_DEBUG_LEXICAL      = 1U << 2,  /* Show lexer tokens                        */
    F_DEBUG_SYNTAX       = 1U << 3,  /* Show parser AST                          */
    F_DEBUG_SEMANTIC     = 1U << 4,  /* Show semantic analysis details           */
    F_DEBUG_IR           = 1U << 5,  /* Dump IR (TODO)                           */
    F_DEBUG_OPTIM        = 1U << 6,  /* Show optimization info                   */
    F_DEBUG_COMPILE      = 1U << 7,  /* Output compilation stage info (TODO)     */
    F_DEBUG_BUILD        = 1U << 8,  /* Output build info (TODO)                 */
    F_DEBUG_LINKER       = 1U << 9,  /* Output linker info (TODO)                */
    F_DEBUG_ALL          = 1U << 10, /* Enable all debug outputs                 */
    F_TIME               = 1U << 11, /* Measure compilation time (TODO)          */
    F_WALL               = 1U << 13, /* Enable basic warnings (TODO)             */
    F_WEXTRA             = 1U << 14, /* Enable extra warnings (TODO)             */
    F_WERROR             = 1U << 15, /* Treat warnings as errors                 */
    F_WIGNOR             = 1U << 16, /* Suppress all warnings                    */
    F_DEBUG_SYMBOLS      = 1U << 17  /* Generate debug symbols (like GCC -g)     */
};

/* Starting capacity for dynamic string arrays (filenames, etc.) */
#define FILENAMES_BLOCK 8

/* Forward declaration: callback type for debug output writers. */
typedef void (*OutputWriterEx)(FILE*, void*);

/*
 * Arguments – contains all parsed command-line options and source file list.
 * The output_file member stores the name of the final executable or object file.
 * filenames is a dynamically allocated array of source file paths.
 * target_* parameters can be set to "nativ" to auto-detect the host platform.
 */
typedef struct {
    FlagSet flags;              /* Bitmask of all enabled options               */
    char*   output_file;        /* Output file name (from -o or positional)     */
    char**  filenames;          /* Source files to process                      */
    size_t  file_count;         /* Number of source files                       */
    size_t  file_capacity;      /* Allocated capacity of the filenames array    */
    int     exit_code;          /* Exit code to return from main                */
    /* Target settings for the preprocessor (default "nativ" = auto-detect) */
    const char* target_arch;    /* Target CPU architecture                      */
    const char* target_core;    /* Target operating system core name            */
    const char* target_bits;    /* Target pointer size (32, 64, …)              */
} Arguments;

/* Adds a duplicate of `str` into a dynamic string array. */
static int          dynamic_string_push(char*** array,
                                        size_t* count,
                                        size_t* capacity,
                                        const char* str,
                                        const char* err_msg);

/* Expands @file arguments: reads the file and inserts its content as argv tokens. */
static int          expand_at_files(int* argc, char*** argv);

/* Reads the whole content of a file into a freshly allocated buffer. */
static char*        read_file_contents(const char* filename, size_t* out_size);

/* Calls a debug output callback if the corresponding flag bit is set. */
static void         write_debug_output(FlagSet flags,
                                       FlagSet required_flag,
                                       OutputWriterEx writer,
                                       void* userdata);

/* Splits a text into lines (each line is a separate allocated string). */
static const char** split_into_lines(const char* text, size_t* out_count);

/* Releases all memory used by an array of lines. */
static void         free_lines(const char** lines, size_t count);

/* Debug callbacks that know how to print internal structures. */
static void         lexer_output_writer(FILE* f, void* data);
static void         parser_output_writer(FILE* f, void* data);
static void         semantic_output_writer(FILE* f, void* data);
static void         optimizer_output_writer(FILE* f, void* data);

/* Auto-detection of the host platform (returns the target string). */
static const char*  detect_target_os(void);
static const char*  detect_target_arch(void);
static const char*  detect_target_bits(void);

/* Runs the full compilation pipeline for one source file. */
static int          process_one_file(const char* filename, FlagSet flags,
                                     const Arguments* args,
                                     SemanticContext** semantic_ctx);

/*
 * Checks whether `arg` starts with `prefix`. If `prefix` is followed by '=',
 * the part after '=' is returned in *out_rest.
 * Returns 1 for a match, 0 otherwise.
 */
static int arg_matches(const char* arg, const char* prefix, const char** out_rest);

/* Parses the value given to --debug-info and sets the corresponding flags. */
static void parse_debug_info(const char* value, FlagSet* flags);

/* Validators for the --tarch, --tcore and --tbits values. */
static int validate_target_arch(const char* value);
static int validate_target_core(const char* value);
static int validate_target_bits(const char* value);

/* Prints usage information (called for -help). */
static void print_usage(void);

/* Prints compiler version and copyright (called for -version). */
static void print_version(void);

/* Main argument parser; fills an Arguments structure. */
static int parse_arguments(int argc, char* argv[], Arguments* args);

/* Simple file reader helper used by @file expansion. */
static char* read_entire_file(const char* path, size_t* size);

/* Tokenises an input string into an argv-style array (respects quotes). */
static char** tokenize_args(const char* input, int* argc);

/* ================================================================== */
/*  Implementation of helper functions                                 */
/* ================================================================== */

/*
 * dynamic_string_push
 * Appends a copy of `str` to a dynamic char** array, growing the buffer
 * as needed. On error the function reports a memory error and returns 0.
 */
static int dynamic_string_push(char*** array,
                               size_t* count,
                               size_t* capacity,
                               const char* str,
                               const char* err_msg)
{
    /* Grow capacity if the array is full. */
    if (*count >= *capacity) {
        size_t new_cap = (*capacity == 0) ? FILENAMES_BLOCK : (*capacity * 2);
        char** new_arr = (char**)memory_reallocate_zero(
            *array, *capacity * sizeof(char*), new_cap * sizeof(char*));
        if (!new_arr) {
            errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                     "Failed to grow %s array", err_msg);
            return 0;
        }
        *array = new_arr;
        *capacity = new_cap;
    }

    /* Duplicate the string and store it. */
    char* copy = u__strdup_safe(str);
    if (!copy) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                 "Failed to duplicate %s", err_msg);
        return 0;
    }

    (*array)[(*count)++] = copy;
    return 1;
}

/*
 * arg_matches
 * Tests whether `arg` starts with `prefix`. If the character after the prefix
 * is '=', the portion after '=' is stored in *out_rest (if non‑NULL).
 * Returns 1 if the pattern matches, 0 otherwise.
 */
static int arg_matches(const char* arg, const char* prefix, const char** out_rest)
{
    if (out_rest) *out_rest = NULL;
    size_t plen = strlen(prefix);
    if (strncmp(arg, prefix, plen) != 0) return 0;

    if (arg[plen] == '\0') {
        return 1;   /* exact match, no value */
    }
    if (arg[plen] == '=') {
        if (out_rest) *out_rest = arg + plen + 1;
        return 1;
    }
    return 0;
}

/*
 * parse_debug_info
 * Translates a comma‑separated (or single) debug category into the
 * corresponding flag bits. Recognised values are: all, preprocess, lexical,
 * syntax, semantic, ir, optim, compile, build, linker. Unknown words are
 * silently ignored.
 */
static void parse_debug_info(const char* value, FlagSet* flags)
{
    if (!value) return;
    if (u__streq(value, "all")) {
        *flags |= F_DEBUG_PREPROCESS | F_DEBUG_LEXICAL | F_DEBUG_SYNTAX |
                  F_DEBUG_SEMANTIC | F_DEBUG_IR | F_DEBUG_OPTIM |
                  F_DEBUG_COMPILE | F_DEBUG_BUILD | F_DEBUG_LINKER;
        return;
    }
    if (u__streq(value, "preprocess")) {
        *flags |= F_DEBUG_PREPROCESS;
    } else if (u__streq(value, "lexical")) {
        *flags |= F_DEBUG_LEXICAL;
    } else if (u__streq(value, "syntax")) {
        *flags |= F_DEBUG_SYNTAX;
    } else if (u__streq(value, "semantic")) {
        *flags |= F_DEBUG_SEMANTIC;
    } else if (u__streq(value, "ir")) {
        *flags |= F_DEBUG_IR;
    } else if (u__streq(value, "optim")) {
        *flags |= F_DEBUG_OPTIM;
    } else if (u__streq(value, "compile")) {
        *flags |= F_DEBUG_COMPILE;
    } else if (u__streq(value, "build")) {
        *flags |= F_DEBUG_BUILD;
    } else if (u__streq(value, "linker")) {
        *flags |= F_DEBUG_LINKER;
    }
}

/* validators for target parameters */
static int validate_target_arch(const char* value)
{
    if (!value) return 0;
    return (u__streq(value, "x86") || u__streq(value, "x86_64") ||
            u__streq(value, "amd64") || u__streq(value, "arm") ||
            u__streq(value, "nativ"));
}

static int validate_target_core(const char* value)
{
    if (!value) return 0;
    /* NT is the internal name for Windows‑family targets */
    return (u__streq(value, "UNIX") || u__streq(value, "BSD") ||
            u__streq(value, "GNUHurd") || u__streq(value, "Linux") ||
            u__streq(value, "Darwin") || u__streq(value, "NT") ||
            u__streq(value, "nativ"));
}

static int validate_target_bits(const char* value)
{
    if (!value) return 0;
    return (u__streq(value, "64") || u__streq(value, "32") ||
            u__streq(value, "16") || u__streq(value, "8") ||
            u__streq(value, "nativ"));
}

/*
 * print_usage
 * Displays the command‑line syntax and a brief description of every flag.
 */
static void print_usage(void)
{
    printf("usage: paxsy \033[1m[operations] <output> <source>\033[0m ...\n"
           "operations:\n"
           "  \033[1m-help\033[0m                   Display this information.\n"
           "  \033[1m-version\033[0m                Display compiler version.\n"
           "  \033[1m--c=<format>\033[0m            Compile files into an executable file with the\n"
           "                          specified format.\n"
           "                           --c={{elf|exe|app}|nativ}\n"
           "  \033[1m-o\033[0m                      Compile a binary file (overrides output file).\n"
           "  \033[1m-shared\033[0m                 Compile shared object file.\n"
           "  \033[1m-time\033[0m                   Compile time output.\n"
           "  \033[1m-g\033[0m                      Generate debug information (analogous to GCC).\n"
           "  \033[1m-Wall\033[0m                   Includes all basic warnings.\n"
           "  \033[1m-Wextra\033[0m                 Includes extended warnings.\n"
           "  \033[1m-Werror\033[0m                 Turns all warnings into errors.\n"
           "  \033[1m-Wignor\033[0m                 Turns off warnings.\n"
           "  \033[1m--tarch=<arch>\033[0m          Specify the target processor architecture.\n"
           "                           --tarch={{x86|x86_64|amd64|arm}|nativ}\n"
           "  \033[1m--tcore=<core>\033[0m          Specify the target core of the system.\n"
           "                           --tcore={{UNIX|BSD|GNUHurd|Linux|Darwin|NT}|\n"
           "                             |nativ}\n"
           "  \033[1m--tbits=<bits>\033[0m          Specify the target bit size of the processor.\n"
           "                           --tbits={{64/32/16/8}|nativ}\n"
           "  \033[1m--debug-info=<mod>\033[0m      Debug output (off by default).\n"
           "                           --debug-info={{preprocess|lexical|syntax|\n"
           "                             |semantic|ir|optim|compile|build|linker}|all}\n"
           "\n"
           "Arguments may also be read from a file using @<filename>.\n"
           "\n"
           "For bug reporting instructions, please see:\n"
           "<https://github.com/aiv-tmc/paxsy/wiki/Flags>\n"
    );
}

/*
 * print_version
 * Shows the compiler generation, name, version, release date and license.
 */
static void print_version(void)
{
    printf("paxsy %s %s\n"
           "\033[1m%s\033[0m - \033[1m%s\033[0m\n"
           "\n"
           "Developed by AIV\n"
           "This free software is distributed under the MIT General Public License\n",
           GENERATION, NAME, VERSION, DATE);
}

/*
 * read_entire_file
 * Opens a file, reads its whole content into a newly allocated buffer and
 * returns it. The caller must free the buffer. On error NULL is returned.
 */
static char* read_entire_file(const char* path, size_t* size)
{
    FILE* f = fopen(path, "r");
    if (!f) {
        errhandler__report_error(ERROR_CODE_IO_READ, 0, 0, "file",
                                 "Cannot open flags file: %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char* buf = (char*)memory_allocate_zero((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read_bytes = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (read_bytes != (size_t)len) {
        memory_free_safe((void**)&buf);
        return NULL;
    }
    buf[len] = '\0';
    if (size) *size = (size_t)len;
    return buf;
}

/*
 * tokenize_args
 * Splits a raw string into an argv array, respecting single and double quotes.
 * The returned array and each element must be freed by the caller.
 * On failure NULL is returned.
 */
static char** tokenize_args(const char* input, int* argc)
{
    if (!input) return NULL;

    /* First pass: count tokens. */
    int count = 0;
    const char* p = input;
    int in_quote = 0;
    char quote_char = 0;
    while (*p) {
        if (!in_quote && u__char_is_whitespace(*p)) {
            ++p;
            continue;
        }
        if (!in_quote && (*p == '"' || *p == '\'')) {
            in_quote = 1;
            quote_char = *p;
            ++p;
            continue;
        }
        if (in_quote && *p == quote_char) {
            in_quote = 0;
            quote_char = 0;
            ++p;
            continue;
        }
        ++p;
        if (!in_quote && (u__char_is_whitespace(*p) || *p == '\0' || *p == '"' || *p == '\'')) {
            count++;
            while (u__char_is_whitespace(*p)) ++p;
        }
    }
    if (count == 0) {
        char** result = (char**)memory_allocate_zero(sizeof(char*));
        if (result) *result = NULL;
        *argc = 0;
        return result;
    }

    /* Second pass: allocate and copy tokens. */
    char** argv = (char**)memory_allocate_zero((count + 1) * sizeof(char*));
    if (!argv) return NULL;

    p = input;
    int idx = 0;
    while (*p) {
        while (!in_quote && u__char_is_whitespace(*p)) ++p;
        if (!*p) break;

        const char* start = p;
        while (*p) {
            if (!in_quote && (*p == '"' || *p == '\'')) {
                in_quote = 1;
                quote_char = *p;
                ++p;
                continue;
            }
            if (in_quote && *p == quote_char) {
                in_quote = 0;
                quote_char = 0;
                ++p;
                break;
            }
            if (!in_quote && u__char_is_whitespace(*p)) {
                break;
            }
            ++p;
        }
        size_t len = p - start;
        char* tok = (char*)memory_allocate_zero(len + 1);
        if (!tok) {
            for (int j = 0; j < idx; ++j) memory_free_safe((void**)&argv[j]);
            memory_free_safe((void**)&argv);
            return NULL;
        }
        memcpy(tok, start, len);
        tok[len] = '\0';
        argv[idx++] = tok;
    }
    argv[count] = NULL;
    *argc = count;
    return argv;
}

/*
 * expand_at_files
 * Scans the argument list for entries starting with '@'. For each such entry
 * the file named after the '@' is read, its content tokenised and inserted
 * into the argument list, replacing the original @file token.
 * argc and argv are updated to point to the new, expanded array.
 * Returns 0 on fatal error.
 */
static int expand_at_files(int* argc, char*** argv)
{
    int new_argc = *argc;
    char** new_argv = NULL;
    int out_idx = 0;

    /* First pass: count total arguments after expansion. */
    for (int i = 0; i < *argc; ++i) {
        if ((*argv)[i][0] == '@') {
            const char* fname = (*argv)[i] + 1;
            size_t fsize;
            char* content = read_entire_file(fname, &fsize);
            if (!content) {
                errhandler__report_error(ERROR_CODE_IO_READ, 0, 0, "file",
                                         "Failed to read flags file: %s", fname);
                return 0;
            }
            int sub_argc = 0;
            char** sub_argv = tokenize_args(content, &sub_argc);
            memory_free_safe((void**)&content);
            if (!sub_argv) return 0;
            new_argc += sub_argc - 1;  /* replace one @file with sub_argc items */
            for (int j = 0; j < sub_argc; ++j) memory_free_safe((void**)&sub_argv[j]);
            memory_free_safe((void**)&sub_argv);
        }
    }

    /* Allocate new argv with space for the expanded token list. */
    new_argv = (char**)memory_allocate_zero((new_argc + 1) * sizeof(char*));
    if (!new_argv) return 0;

    /* Second pass: fill new_argv. */
    for (int i = 0; i < *argc; ++i) {
        if ((*argv)[i][0] == '@') {
            const char* fname = (*argv)[i] + 1;
            size_t fsize;
            char* content = read_entire_file(fname, &fsize);
            if (!content) {
                for (int j = 0; j < out_idx; ++j) memory_free_safe((void**)&new_argv[j]);
                memory_free_safe((void**)&new_argv);
                return 0;
            }
            int sub_argc = 0;
            char** sub_argv = tokenize_args(content, &sub_argc);
            memory_free_safe((void**)&content);
            if (!sub_argv) {
                for (int j = 0; j < out_idx; ++j) memory_free_safe((void**)&new_argv[j]);
                memory_free_safe((void**)&new_argv);
                return 0;
            }
            for (int j = 0; j < sub_argc; ++j) {
                new_argv[out_idx++] = sub_argv[j];
            }
            /* Free the array of pointers only; the token strings are now owned by new_argv. */
            memory_free_safe((void**)&sub_argv);
        } else {
            new_argv[out_idx++] = u__strdup_safe((*argv)[i]);
        }
    }
    new_argv[new_argc] = NULL;

    *argc = new_argc;
    *argv = new_argv;
    return 1;
}

/*
 * parse_arguments
 * Processes the command line (after @file expansion) and fills the `args`
 * structure. The first non‑option argument is treated as the output file name
 * (unless -o is given); subsequent non‑option arguments are source files.
 * Returns 1 on success, 0 on fatal error, -1 if the program should exit
 * immediately (e.g., after -help or -version).
 */
static int parse_arguments(int argc, char* argv[], Arguments* args)
{
    memset(args, 0, sizeof(*args));

    /* Prepare the dynamic array for source filenames. */
    args->file_capacity = (argc / 2) + FILENAMES_BLOCK;
    args->filenames = (char**)memory_allocate_zero(args->file_capacity * sizeof(char*));
    if (!args->filenames) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                 "Failed to allocate filename array");
        return 0;
    }

    /* By default, target parameters are set to "nativ" (auto-detect). */
    args->target_arch = "nativ";
    args->target_core = "nativ";
    args->target_bits = "nativ";

    const char* rest = NULL;
    int output_set = 0;   /* whether the output file has already been specified */

    for (int i = 1; i < argc; ++i) {
        char* arg = argv[i];

        /* Non‑option arguments. */
        if (arg[0] != '-') {
            if (!output_set) {
                /* First positional argument is the output file. */
                args->output_file = u__strdup_safe(arg);
                output_set = 1;
                /* Implicitly enable compilation mode. */
                args->flags |= F_MODE_COMPILE;
            } else {
                /* All following positional arguments are source files. */
                if (!dynamic_string_push(&args->filenames, &args->file_count,
                                         &args->file_capacity, arg, "filename")) {
                    return 0;
                }
            }
            continue;
        }

        /* Immediate‑exit flags. */
        if (u__streq(arg, "-help")) {
            print_usage();
            args->exit_code = 0;
            return -1;
        }
        if (u__streq(arg, "-version")) {
            print_version();
            args->exit_code = 0;
            return -1;
        }

        /* -o <file> : explicitly sets the output file name. */
        if (u__streq(arg, "-o")) {
            if (i + 1 < argc) {
                /* Free a previously set output name in case of multiple -o flags. */
                if (args->output_file) {
                    memory_free_safe((void**)&args->output_file);
                }
                args->output_file = u__strdup_safe(argv[++i]);
                output_set = 1;
                args->flags |= F_MODE_COMPILE;
            }
            continue;
        }

        /* -shared : indicates compilation of a shared object. */
        if (u__streq(arg, "-shared")) {
            args->flags |= F_MODE_COMPILE;
            continue;
        }

        /* --c=<format> : compilation mode (format is currently ignored). */
        if (arg_matches(arg, "--c", &rest)) {
            args->flags |= F_MODE_COMPILE;
            continue;
        }

        /* -time : request measurement of compilation time (TODO). */
        if (u__streq(arg, "-time")) {
            args->flags |= F_TIME;
            continue;
        }

        /*
         * -g : analogous to GCC's -g, requests generation of debug information.
         * Full implementation is pending; here we just set the flag.
         */
        if (u__streq(arg, "-g")) {
            args->flags |= F_DEBUG_SYMBOLS;
            continue;
        }

        /* Warning control. */
        if (u__streq(arg, "-Wall")) {
            args->flags |= F_WALL;
            continue;
        }
        if (u__streq(arg, "-Wextra")) {
            args->flags |= F_WEXTRA;
            continue;
        }
        if (u__streq(arg, "-Werror")) {
            args->flags |= F_WERROR;
            continue;
        }
        if (u__streq(arg, "-Wignor")) {
            args->flags |= F_WIGNOR;
            continue;
        }

        /* Target architecture. */
        if (arg_matches(arg, "--tarch", &rest)) {
            if (!validate_target_arch(rest)) {
                errhandler__report_error(ERROR_CODE_INPUT_INVALID_FLAG, 0, 0, "input",
                                         "Invalid value for --tarch: %s", rest ? rest : "(null)");
                continue;
            }
            args->target_arch = rest;
            continue;
        }

        /* Target core (operating system). */
        if (arg_matches(arg, "--tcore", &rest)) {
            if (!validate_target_core(rest)) {
                errhandler__report_error(ERROR_CODE_INPUT_INVALID_FLAG, 0, 0, "input",
                                         "Invalid value for --tcore: %s", rest ? rest : "(null)");
                continue;
            }
            args->target_core = rest;
            continue;
        }

        /* Target bits. */
        if (arg_matches(arg, "--tbits", &rest)) {
            if (!validate_target_bits(rest)) {
                errhandler__report_error(ERROR_CODE_INPUT_INVALID_FLAG, 0, 0, "input",
                                         "Invalid value for --tbits: %s", rest ? rest : "(null)");
                continue;
            }
            args->target_bits = rest;
            continue;
        }

        /* Debug info (the only way to enable debug output). */
        if (arg_matches(arg, "--debug-info", &rest)) {
            parse_debug_info(rest, &args->flags);
            continue;
        }

        /* Anything else is an unrecognised flag. */
        errhandler__report_error(ERROR_CODE_INPUT_INVALID_FLAG, 0, 0, "input",
                                 "unknown flag: %s", arg);
    }

    return 1;
}

/*
 * read_file_contents
 * Reads the entire content of `filename` into a null‑terminated buffer
 * allocated with memory_allocate_zero. On error reports a diagnostic and
 * returns NULL.
 */
static char* read_file_contents(const char* filename, size_t* out_size)
{
    FILE* f = fopen(filename, "r");
    if (!f) {
        errhandler__report_error(ERROR_CODE_IO_READ, 0, 0, "file",
                                 "Cannot open file: %s", filename);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        errhandler__report_error(ERROR_CODE_IO_READ, 0, 0, "file",
                                 "Cannot seek in file: %s", filename);
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        errhandler__report_error(ERROR_CODE_IO_READ, 0, 0, "file",
                                 "Cannot determine file size: %s", filename);
        fclose(f);
        return NULL;
    }
    rewind(f);
    *out_size = (size_t)size;
    if (*out_size == 0) {
        fclose(f);
        char* empty = (char*)memory_allocate_zero(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    char* buf = (char*)memory_allocate_zero(*out_size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read_bytes = fread(buf, 1, *out_size, f);
    fclose(f);
    if (read_bytes != *out_size) {
        memory_free_safe((void**)&buf);
        errhandler__report_error(ERROR_CODE_IO_READ, 0, 0, "file",
                                 "Short read from file: %s", filename);
        return NULL;
    }
    buf[*out_size] = '\0';
    return buf;
}

/*
 * write_debug_output
 * If `flags` has the bit `required_flag` set, the callback `writer` is invoked
 * with `stdout` and `userdata`. This centralises the gating of debug output.
 */
static void write_debug_output(FlagSet flags,
                               FlagSet required_flag,
                               OutputWriterEx writer,
                               void* userdata)
{
    if (flags & required_flag) {
        writer(stdout, userdata);
    }
}

/*
 * split_into_lines
 * Breaks a null‑terminated string into an array of line strings (without the
 * line terminator). Each line is a separate allocated copy. The array and its
 * elements must later be freed with free_lines().
 */
static const char** split_into_lines(const char* text, size_t* out_count)
{
    *out_count = 0;
    if (!text || !*text) return NULL;

    size_t cap = 32;
    const char** lines = (const char**)memory_allocate_zero(cap * sizeof(char*));
    if (!lines) return NULL;

    size_t idx = 0;
    const char* start = text;
    const char* p = text;

    while (*p) {
        if (u__char_is_line_break(*p)) {
            size_t len = p - start;
            char* copy = (char*)memory_allocate_zero(len + 1);
            if (!copy) goto fail;
            memcpy(copy, start, len);
            copy[len] = '\0';

            if (idx >= cap) {
                cap *= 2;
                const char** newl = (const char**)memory_reallocate_zero(
                    lines, idx * sizeof(char*), cap * sizeof(char*));
                if (!newl) {
                    memory_free_safe((void**)&copy);
                    goto fail;
                }
                lines = newl;
            }
            lines[idx++] = copy;
            start = p + 1;
        }
        ++p;
    }

    /* Handle the last line if it is not empty. */
    if (start < p) {
        size_t len = p - start;
        char* copy = (char*)memory_allocate_zero(len + 1);
        if (!copy) goto fail;
        memcpy(copy, start, len);
        copy[len] = '\0';

        if (idx >= cap) {
            cap += 1;
            const char** newl = (const char**)memory_reallocate_zero(
                lines, idx * sizeof(char*), cap * sizeof(char*));
            if (!newl) {
                memory_free_safe((void**)&copy);
                goto fail;
            }
            lines = newl;
        }
        lines[idx++] = copy;
    }

    *out_count = idx;

    /* Optionally shrink the array to the exact used size. */
    if (idx < cap) {
        const char** shrunk = (const char**)memory_reallocate_zero(
            lines, cap * sizeof(char*), idx * sizeof(char*));
        if (shrunk) lines = shrunk;
    }
    return lines;

fail:
    for (size_t i = 0; i < idx; ++i)
        memory_free_safe((void**)&lines[i]);
    memory_free_safe((void**)&lines);
    return NULL;
}

/*
 * free_lines
 * Releases the memory used by an array of lines previously returned by
 * split_into_lines.
 */
static void free_lines(const char** lines, size_t count)
{
    if (!lines) return;
    for (size_t i = 0; i < count; ++i)
        memory_free_safe((void**)&lines[i]);
    memory_free_safe((void**)&lines);
}

/* Debug callbacks – each knows how to display an internal data structure. */
static void lexer_output_writer(FILE* f, void* data)
{
    Lexer* lexer = (Lexer*)data;
    print_tokens_in_lines(lexer, f);
}

static void parser_output_writer(FILE* f, void* data)
{
    AST* ast = (AST*)data;
    print_ast_detailed(ast, f);
}

static void semantic_output_writer(FILE* f, void* data)
{
    SemanticContext* ctx = (SemanticContext*)data;
    print_semantic_analysis(ctx, f);
}

static void optimizer_output_writer(FILE* f, void* data)
{
    AST* ast = (AST*)data;
    print_optimized_ast(ast, f);
}

/*
 * Auto‑detection functions for the host platform.
 * They return the string that would be used as a target value when
 * "nativ" is specified.
 */
static const char* detect_target_os(void)
{
#if defined(_WIN32) || defined(_WIN64)
    return "NT";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__) && defined(__MACH__)
    return "darwin";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#elif defined(__NetBSD__)
    return "netbsd";
#elif defined(__sun)
    return "solaris";
#elif defined(__MSDOS__) || defined(__DOS__)
    return "msdos";
#else
    return "unknown";
#endif
}

static const char* detect_target_arch(void)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86) || defined(__i486__) || defined(__i586__) || defined(__i686__)
    return "i386";
#elif defined(__arm__)
    #if defined(__ARM_ARCH_7__) || defined(__ARM_ARCH_7A__)
        return "armv7";
    #elif defined(__ARM_ARCH_6__)
        return "armv6";
    #elif defined(__ARM_ARCH_5__)
        return "armv5";
    #elif defined(__ARM_ARCH_4T__)
        return "armv4t";
    #else
        return "arm";
    #endif
#elif defined(__aarch64__)
    return "aarch64";
#else
    return "unknown";
#endif
}

static const char* detect_target_bits(void)
{
    if (sizeof(void*) == 8) return "64";
    if (sizeof(void*) == 4) return "32";
    if (sizeof(void*) == 2) return "16";
    if (sizeof(void*) == 1) return "8";
    return "unknown";
}

/*
 * process_one_file
 * Runs the full compilation pipeline for a single source file:
 *   1. Read the file.
 *   2. Preprocess.
 *   3. Lex.
 *   4. Parse.
 *   5. Semantic analysis (if a context is provided).
 *   6. Optimization.
 * Debug output is emitted if the corresponding flags are set.
 * Returns non‑zero if an error occurred.
 */
static int process_one_file(const char* filename,
                            FlagSet flags,
                            const Arguments* args,
                            SemanticContext** semantic_ctx)
{
    int err = 0;
    size_t file_size = 0;
    char* raw = NULL;
    char* processed = NULL;
    Lexer* lexer = NULL;
    AST* ast = NULL;
    const char** lines = NULL;
    size_t line_count = 0;

    /* Tell the error handler which file is currently being processed. */
    errhandler__set_current_filename(filename);

    /* 1. Read raw source. */
    raw = read_file_contents(filename, &file_size);
    if (!raw) { err = 1; goto cleanup; }

    /* 2. Preprocessing. */
    processed = preprocess(raw, filename, NULL);
    if (!processed) {
        errhandler__report_error(ERROR_CODE_COM_FAILCREATE, 0, 0, "preproc",
                                 "Preprocessing failed for file: %s", filename);
        err = 1; goto cleanup;
    }
    /* TODO: if F_DEBUG_PREPROCESS is set, output the preprocessed code. */

    /* Feed the processed source lines to the error handler for better messages. */
    lines = split_into_lines(processed, &line_count);
    if (lines) {
        errhandler__set_source_code(lines, line_count);
    }

    /* 3. Lexical analysis. */
    lexer = lexer__init_lexer(processed);
    if (!lexer) { err = 1; goto cleanup; }
    lexer__tokenize(lexer);
    write_debug_output(flags, F_DEBUG_LEXICAL, lexer_output_writer, lexer);

    /* 4. Parsing (only if no errors so far). */
    if (!errhandler__has_errors()) {
        ast = parse(lexer->tokens, lexer->token_count);
        write_debug_output(flags, F_DEBUG_SYNTAX, parser_output_writer, ast);
    }

    /* 5. Semantic analysis (runs if a context exists, the AST is valid, and no errors). */
    if (*semantic_ctx && ast && !errhandler__has_errors()) {
        semantic__analyze(*semantic_ctx, ast);
        write_debug_output(flags, F_DEBUG_SEMANTIC, semantic_output_writer, *semantic_ctx);

        /* 6. Optimization (only if still error‑free). */
        if (!errhandler__has_errors()) {
            optimizer__optimize(ast, (*semantic_ctx)->global_scope);
            write_debug_output(flags, F_DEBUG_OPTIM, optimizer_output_writer, ast);
        }
    }

    /*
     * TODO: If F_DEBUG_SYMBOLS is set, enable the generation of debug
     * information (e.g., DWARF). This will eventually attach source
     * line mappings and symbol tables to the output.
     */

cleanup:
    errhandler__clear_source_code();
    if (lines) free_lines(lines, line_count);
    if (ast) parser__free_ast(ast);
    if (lexer) lexer__free_lexer(lexer);
    memory_free_safe((void**)&processed);
    memory_free_safe((void**)&raw);
    errhandler__set_current_filename(NULL);

    return err || errhandler__has_errors();
}

/* ================================================================== */
/*  main – program entry point                                         */
/* ================================================================== */
int main(int argc, char* argv[])
{
    /*
     * Step 1: Expand any @file arguments. We work on a copy of argv because
     * expand_at_files allocates a completely new array.
     */
    int expanded_argc = argc;
    char** expanded_argv = NULL;
    expanded_argv = (char**)memory_allocate_zero(argc * sizeof(char*));
    if (!expanded_argv) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                 "Failed to allocate argv copy");
        return 1;
    }
    for (int i = 0; i < argc; ++i) {
        expanded_argv[i] = u__strdup_safe(argv[i]);
    }

    if (!expand_at_files(&expanded_argc, &expanded_argv)) {
        /* Expansion failed – report error and exit. */
        for (int i = 0; i < expanded_argc; ++i) memory_free_safe((void**)&expanded_argv[i]);
        memory_free_safe((void**)&expanded_argv);
        return 1;
    }

    /* Step 2: Parse the expanded arguments. */
    Arguments args = {0};
    int parse_result = parse_arguments(expanded_argc, expanded_argv, &args);

    /* The expanded argv is no longer needed after parsing. */
    for (int i = 0; i < expanded_argc; ++i) memory_free_safe((void**)&expanded_argv[i]);
    memory_free_safe((void**)&expanded_argv);

    if (parse_result == -1) {
        /* -help or -version requested immediate exit. */
        if (args.output_file) memory_free_safe((void**)&args.output_file);
        for (size_t i = 0; i < args.file_count; ++i)
            memory_free_safe((void**)&args.filenames[i]);
        memory_free_safe((void**)&args.filenames);
        return args.exit_code;
    }
    if (parse_result == 0) {
        /* Fatal error during argument parsing. */
        if (args.output_file) memory_free_safe((void**)&args.output_file);
        for (size_t i = 0; i < args.file_count; ++i)
            memory_free_safe((void**)&args.filenames[i]);
        memory_free_safe((void**)&args.filenames);
        return 1;
    }

    /* Configure the error handler based on warning flags. */
    errhandler__set_warnings_as_errors((args.flags & F_WERROR) != 0);
    errhandler__set_suppress_warnings((args.flags & F_WIGNOR) != 0);

    /* Resolve target platform parameters (auto‑detect if "nativ"). */
    if (u__streq(args.target_arch, "nativ")) {
        args.target_arch = detect_target_arch();
    }
    if (u__streq(args.target_core, "nativ")) {
        args.target_core = detect_target_os();
    }
    if (u__streq(args.target_bits, "nativ")) {
        args.target_bits = detect_target_bits();
    }

    /* Pass the final target values to the preprocessor global variables. */
    builtin_target_os = args.target_core;
    builtin_target_arch = args.target_arch;
    builtin_target_bits = args.target_bits;

    /* If compilation was requested, an output file name is mandatory. */
    if ((args.flags & F_MODE_COMPILE) && !args.output_file) {
        errhandler__report_error(ERROR_CODE_INPUT_NO_SOURCE, 0, 0, "input",
                                 "compilation requested but no output file specified");
    }

    /* If no source files were provided and some flags are active, complain. */
    if (args.file_count == 0 && args.flags) {
        errhandler__report_error(ERROR_CODE_INPUT_NO_SOURCE, 0, 0, "input",
                                 "no input source files specified");
    }

    if (errhandler__has_errors()) {
        errhandler__print_errors();
        errhandler__print_warnings();
        goto cleanup_args;
    }

    /* Create a semantic analysis context if needed (compilation or debug). */
    SemanticContext* semantic_ctx = NULL;
    if ((args.flags & F_MODE_COMPILE) || (args.flags & F_DEBUG_SEMANTIC)) {
        semantic_ctx = semantic__create_context();
        if (!semantic_ctx) {
            errhandler__report_error(ERROR_CODE_COM_FAILCREATE, 0, 0, "syntax",
                                     "Failed to create semantic analysis context");
        } else {
            /* Direct field assignment instead of a setter (as per original code). */
            semantic_ctx->exit_on_error = (args.flags & F_MODE_COMPILE) != 0;
        }
    }

    /*
     * Process every source file. For multiple files, a fresh semantic context
     * is created for each file, so that scopes do not leak between compilations.
     */
    int exit_code = 0;
    for (size_t i = 0; i < args.file_count; ++i) {
        if (process_one_file(args.filenames[i], args.flags, &args, &semantic_ctx))
            exit_code = 1;

        /* Recreate the semantic context for the next file. */
        if (semantic_ctx && i + 1 < args.file_count) {
            semantic__destroy_context(semantic_ctx);
            semantic_ctx = semantic__create_context();
            if (semantic_ctx) {
                semantic_ctx->exit_on_error = (args.flags & F_MODE_COMPILE) != 0;
            } else {
                errhandler__report_error(ERROR_CODE_COM_FAILCREATE, 0, 0, "syntax",
                                         "Failed to recreate semantic context");
                exit_code = 1;
            }
        }
    }

    /* Print all accumulated errors and warnings. */
    errhandler__print_errors();
    errhandler__print_warnings();

    if (semantic_ctx) semantic__destroy_context(semantic_ctx);

cleanup_args:
    if (args.output_file) memory_free_safe((void**)&args.output_file);
    for (size_t i = 0; i < args.file_count; ++i)
        memory_free_safe((void**)&args.filenames[i]);
    memory_free_safe((void**)&args.filenames);
    errhandler__free_error_manager();

    /* TODO: If F_TIME was set, print compilation time here. */

    return exit_code;
}
