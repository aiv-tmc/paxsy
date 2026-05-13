/* Copyright (c) 2026 aiv-tmc
 * This software is released under the MIT License.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "preprocessor/preprocessor.h"
#include "preprocessor/defmacros/defmacros.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "output/output.h"
#include "semantic/semantic.h"
#include "optimizer/optimizer.h"
#include "ir/ir.h"
#include "errhandler/errhandler.h"
#include "utils/str_utils.h"
#include "utils/char_utils.h"
#include "utils/memory_utils.h"
#include "utils/common.h"

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

#ifndef PAXSY_LIBRARY_DIR
#define PAXSY_LIBRARY_DIR "/usr/local/lib/paxsy"
#endif
#ifndef PAXSY_INCLUDE_DIR
#define PAXSY_INCLUDE_DIR "/usr/local/include/paxsy"
#endif

typedef uint32_t FlagSet;

enum {
    F_MODE_COMPILE       = 1U << 0,
    F_DEBUG_PREPROCESS   = 1U << 1,
    F_DEBUG_LEXICAL      = 1U << 2,
    F_DEBUG_SYNTAX       = 1U << 3,
    F_DEBUG_SEMANTIC     = 1U << 4,
    F_DEBUG_IR           = 1U << 5,
    F_DEBUG_OPTIM        = 1U << 6,
    F_DEBUG_COMPILE      = 1U << 7,
    F_DEBUG_BUILD        = 1U << 8,
    F_DEBUG_LINKER       = 1U << 9,
    F_DEBUG_ALL          = 1U << 10,
    F_TIME               = 1U << 11,
    F_WALL               = 1U << 13,
    F_WEXTRA             = 1U << 14,
    F_WERROR             = 1U << 15,
    F_WIGNOR             = 1U << 16,
    F_DEBUG_SYMBOLS      = 1U << 17,
    F_OUTPUT_ASSEMBLY    = 1U << 18,
    F_MODE_STATIC_LIB    = 1U << 19
};

#define FILENAMES_BLOCK 8
#define LIBRARIES_BLOCK 4

typedef void (*OutputWriterEx)(FILE*, void*);

typedef struct {
    FlagSet flags;
    char*   output_file;
    char**  filenames;
    size_t  file_count;
    size_t  file_capacity;
    char**  libraries;
    size_t  lib_count;
    size_t  lib_capacity;
    int     exit_code;
    const char* target_arch;
    const char* target_core;
    const char* target_bits;
} Arguments;

static int dynamic_string_push(char*** array, size_t* count, size_t* capacity,
                               const char* str, const char* err_msg);
static int expand_at_files(int* argc, char*** argv);
static char* read_file_contents(const char* filename, size_t* out_size);
static void write_debug_output(FlagSet flags, FlagSet required_flag,
                               OutputWriterEx writer, void* userdata);
static const char** split_into_lines(const char* text, size_t* out_count);
static void free_lines(const char** lines, size_t count);
static char* derive_assembly_filename(const char* source);
static char* derive_optimized_ast_filename(const char* source);
static void lexer_output_writer(FILE* f, void* data);
static void parser_output_writer(FILE* f, void* data);
static void semantic_output_writer(FILE* f, void* data);
static void optimizer_output_writer(FILE* f, void* data);
static void ir_output_writer(FILE* f, void* data);
static const char* detect_target_os(void);
static const char* detect_target_arch(void);
static const char* detect_target_bits(void);
static int process_one_file(const char* filename, const char* output_file,
                            FlagSet flags, const Arguments* args,
                            SemanticContext** semantic_ctx);
static int arg_matches(const char* arg, const char* prefix, const char** out_rest);
static void parse_debug_info(const char* value, FlagSet* flags);
static int validate_target_arch(const char* value);
static int validate_target_core(const char* value);
static int validate_target_bits(const char* value);
static void print_usage(void);
static void print_version(void);
static int parse_arguments(int argc, char* argv[], Arguments* args);
static char* read_entire_file(const char* path, size_t* size);
static char** tokenize_args(const char* input, int* argc);

static int dynamic_string_push(char*** array, size_t* count, size_t* capacity,
                               const char* str, const char* err_msg) {
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
    char* copy = u__strdup_safe(str);
    if (!copy) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                 "Failed to duplicate %s", err_msg);
        return 0;
    }
    (*array)[(*count)++] = copy;
    return 1;
}

static int arg_matches(const char* arg, const char* prefix, const char** out_rest) {
    if (out_rest) *out_rest = NULL;
    size_t plen = strlen(prefix);
    if (strncmp(arg, prefix, plen) != 0) return 0;
    if (arg[plen] == '\0') return 1;
    if (arg[plen] == '=') {
        if (out_rest) *out_rest = arg + plen + 1;
        return 1;
    }
    return 0;
}

static void parse_debug_info(const char* value, FlagSet* flags) {
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

static int validate_target_arch(const char* value) {
    if (!value) return 0;
    return (u__streq(value, "x86") || u__streq(value, "x86_64") ||
            u__streq(value, "amd64") || u__streq(value, "arm") ||
            u__streq(value, "nativ"));
}

static int validate_target_core(const char* value) {
    if (!value) return 0;
    return (u__streq(value, "UNIX") || u__streq(value, "BSD") ||
            u__streq(value, "GNUHurd") || u__streq(value, "Linux") ||
            u__streq(value, "Darwin") || u__streq(value, "NT") ||
            u__streq(value, "nativ"));
}

static int validate_target_bits(const char* value) {
    if (!value) return 0;
    return (u__streq(value, "64") || u__streq(value, "32") ||
            u__streq(value, "16") || u__streq(value, "8") ||
            u__streq(value, "nativ"));
}

static void print_usage(void) {
    printf("usage: paxsy \033[1m[operations] <output> <source>\033[0m ...\n"
           "operations:\n"
           "  \033[1m-help\033[0m                   Display this information.\n"
           "  \033[1m-version\033[0m                Display compiler version.\n"
           "  \033[1m--c=<format>\033[0m            Compile files into an executable file with the\n"
           "                          specified format.\n"
           "                           --c={{elf|exe|app}|nativ}\n"
           "  \033[1m-o\033[0m                      Compile a binary file (overrides output file).\n"
           "  \033[1m-S\033[0m                      Compile to assembly only (generates .s files).\n"
           "  \033[1m-shared\033[0m                 Compile shared object file.\n"
           "  \033[1m-state\033[0m                  Create a static library archive (.a file).\n"
           "  \033[1m--l=<lib>\033[0m               Link with the specified static library.\n"
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
           "<https://github.com/aiv-tmc/paxsy/wiki/Flags>\n");
}

static void print_version(void) {
    printf("paxsy %s %s\n"
           "\033[1m%s\033[0m - \033[1m%s\033[0m\n"
           "\n"
           "Developed by AIV\n"
           "This free software is distributed under the MIT General Public License\n",
           GENERATION, NAME, VERSION, DATE);
}

static char* read_entire_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "r");
    if (!f) {
        errhandler__report_error(ERROR_CODE_IO_READ, 0, 0, "file",
                                 "Cannot open flags file: %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)memory_allocate_zero((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
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

static char** tokenize_args(const char* input, int* argc) {
    if (!input) return NULL;
    int count = 0;
    const char* p = input;
    int in_quote = 0;
    char quote_char = 0;
    while (*p) {
        if (!in_quote && u__char_is_whitespace(*p)) { ++p; continue; }
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
            if (!in_quote && u__char_is_whitespace(*p)) break;
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

static int expand_at_files(int* argc, char*** argv) {
    int new_argc = *argc;
    char** new_argv = NULL;
    int out_idx = 0;
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
            new_argc += sub_argc - 1;
            for (int j = 0; j < sub_argc; ++j) memory_free_safe((void**)&sub_argv[j]);
            memory_free_safe((void**)&sub_argv);
        }
    }
    new_argv = (char**)memory_allocate_zero((new_argc + 1) * sizeof(char*));
    if (!new_argv) return 0;
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
            for (int j = 0; j < sub_argc; ++j) new_argv[out_idx++] = sub_argv[j];
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

static int parse_arguments(int argc, char* argv[], Arguments* args) {
    memset(args, 0, sizeof(*args));
    args->file_capacity = (argc / 2) + FILENAMES_BLOCK;
    args->filenames = (char**)memory_allocate_zero(args->file_capacity * sizeof(char*));
    if (!args->filenames) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                 "Failed to allocate filename array");
        return 0;
    }
    args->lib_capacity = LIBRARIES_BLOCK;
    args->libraries = (char**)memory_allocate_zero(args->lib_capacity * sizeof(char*));
    if (!args->libraries) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                 "Failed to allocate libraries array");
        return 0;
    }
    args->target_arch = "nativ";
    args->target_core = "nativ";
    args->target_bits = "nativ";
    const char* rest = NULL;
    int output_set = 0;
    for (int i = 1; i < argc; ++i) {
        char* arg = argv[i];
        if (arg[0] != '-') {
            if (!output_set) {
                args->output_file = u__strdup_safe(arg);
                output_set = 1;
                args->flags |= F_MODE_COMPILE;
            } else {
                if (!dynamic_string_push(&args->filenames, &args->file_count,
                                         &args->file_capacity, arg, "filename"))
                    return 0;
            }
            continue;
        }
        if (u__streq(arg, "-help")) { print_usage(); args->exit_code = 0; return -1; }
        if (u__streq(arg, "-version")) { print_version(); args->exit_code = 0; return -1; }
        if (u__streq(arg, "-S")) { args->flags |= F_OUTPUT_ASSEMBLY; continue; }
        if (u__streq(arg, "-state")) { args->flags |= F_MODE_STATIC_LIB; continue; }
        if (arg_matches(arg, "--l", &rest)) {
            if (!rest || !*rest) {
                errhandler__report_error(ERROR_CODE_INPUT_INVALID_FLAG, 0, 0, "input",
                                         "Missing library name after --l=");
                continue;
            }
            if (!dynamic_string_push(&args->libraries, &args->lib_count,
                                     &args->lib_capacity, rest, "library"))
                return 0;
            continue;
        }
        if (u__streq(arg, "-o")) {
            if (i + 1 < argc) {
                if (args->output_file) memory_free_safe((void**)&args->output_file);
                args->output_file = u__strdup_safe(argv[++i]);
                output_set = 1;
                args->flags |= F_MODE_COMPILE;
            }
            continue;
        }
        if (u__streq(arg, "-shared")) { args->flags |= F_MODE_COMPILE; continue; }
        if (arg_matches(arg, "--c", &rest)) { args->flags |= F_MODE_COMPILE; continue; }
        if (u__streq(arg, "-time")) { args->flags |= F_TIME; continue; }
        if (u__streq(arg, "-g")) { args->flags |= F_DEBUG_SYMBOLS; continue; }
        if (u__streq(arg, "-Wall")) { args->flags |= F_WALL; continue; }
        if (u__streq(arg, "-Wextra")) { args->flags |= F_WEXTRA; continue; }
        if (u__streq(arg, "-Werror")) { args->flags |= F_WERROR; continue; }
        if (u__streq(arg, "-Wignor")) { args->flags |= F_WIGNOR; continue; }
        if (arg_matches(arg, "--tarch", &rest)) {
            if (!validate_target_arch(rest)) {
                errhandler__report_error(ERROR_CODE_INPUT_INVALID_FLAG, 0, 0, "input",
                                         "Invalid value for --tarch: %s", rest ? rest : "(null)");
                continue;
            }
            args->target_arch = rest;
            continue;
        }
        if (arg_matches(arg, "--tcore", &rest)) {
            if (!validate_target_core(rest)) {
                errhandler__report_error(ERROR_CODE_INPUT_INVALID_FLAG, 0, 0, "input",
                                         "Invalid value for --tcore: %s", rest ? rest : "(null)");
                continue;
            }
            args->target_core = rest;
            continue;
        }
        if (arg_matches(arg, "--tbits", &rest)) {
            if (!validate_target_bits(rest)) {
                errhandler__report_error(ERROR_CODE_INPUT_INVALID_FLAG, 0, 0, "input",
                                         "Invalid value for --tbits: %s", rest ? rest : "(null)");
                continue;
            }
            args->target_bits = rest;
            continue;
        }
        if (arg_matches(arg, "--debug-info", &rest)) {
            parse_debug_info(rest, &args->flags);
            continue;
        }
        errhandler__report_error(ERROR_CODE_INPUT_INVALID_FLAG, 0, 0, "input",
                                 "unknown flag: %s", arg);
    }
    return 1;
}

static char* read_file_contents(const char* filename, size_t* out_size) {
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
    if (!buf) { fclose(f); return NULL; }
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

static void write_debug_output(FlagSet flags, FlagSet required_flag,
                               OutputWriterEx writer, void* userdata) {
    if (flags & required_flag) writer(stdout, userdata);
}

static const char** split_into_lines(const char* text, size_t* out_count) {
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
                if (!newl) { memory_free_safe((void**)&copy); goto fail; }
                lines = newl;
            }
            lines[idx++] = copy;
            start = p + 1;
        }
        ++p;
    }
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
            if (!newl) { memory_free_safe((void**)&copy); goto fail; }
            lines = newl;
        }
        lines[idx++] = copy;
    }
    *out_count = idx;
    if (idx < cap) {
        const char** shrunk = (const char**)memory_reallocate_zero(
            lines, cap * sizeof(char*), idx * sizeof(char*));
        if (shrunk) lines = shrunk;
    }
    return lines;
fail:
    for (size_t i = 0; i < idx; ++i) memory_free_safe((void**)&lines[i]);
    memory_free_safe((void**)&lines);
    return NULL;
}

static void free_lines(const char** lines, size_t count) {
    if (!lines) return;
    for (size_t i = 0; i < count; ++i) memory_free_safe((void**)&lines[i]);
    memory_free_safe((void**)&lines);
}

static char* derive_assembly_filename(const char* source) {
    if (!source) return NULL;
    const char* last_slash = strrchr(source, '/');
    const char* last_backslash = strrchr(source, '\\');
    const char* file_start = source;
    if (last_slash) file_start = last_slash + 1;
    if (last_backslash && last_backslash > last_slash) file_start = last_backslash + 1;
    const char* last_dot = strrchr(file_start, '.');
    size_t base_len = last_dot ? (size_t)(last_dot - source) : strlen(source);
    size_t new_len = base_len + 3;
    char* asm_name = (char*)memory_allocate_zero(new_len);
    if (!asm_name) return NULL;
    memcpy(asm_name, source, base_len);
    asm_name[base_len] = '.';
    asm_name[base_len+1] = 's';
    asm_name[base_len+2] = '\0';
    return asm_name;
}

static char* derive_optimized_ast_filename(const char* source) {
    if (!source) return NULL;
    const char* last_slash = strrchr(source, '/');
    const char* last_backslash = strrchr(source, '\\');
    const char* file_start = source;
    if (last_slash) file_start = last_slash + 1;
    if (last_backslash && last_backslash > last_slash) file_start = last_backslash + 1;
    const char* last_dot = strrchr(file_start, '.');
    size_t base_len = last_dot ? (size_t)(last_dot - source) : strlen(source);
    size_t new_len = base_len + 11;
    char* opt_name = (char*)memory_allocate_zero(new_len);
    if (!opt_name) return NULL;
    memcpy(opt_name, source, base_len);
    memcpy(opt_name + base_len, ".optim.ast", 10);
    opt_name[base_len + 10] = '\0';
    return opt_name;
}

static void lexer_output_writer(FILE* f, void* data) {
    Lexer* lexer = (Lexer*)data;
    print_tokens_in_lines(lexer, f);
}

static void parser_output_writer(FILE* f, void* data) {
    AST* ast = (AST*)data;
    print_ast_detailed(ast, f);
}

static void semantic_output_writer(FILE* f, void* data) {
    SemanticContext* ctx = (SemanticContext*)data;
    print_semantic_analysis(ctx, f);
}

static void optimizer_output_writer(FILE* f, void* data) {
    AST* ast = (AST*)data;
    print_optimized_ast(ast, f);
}

static void ir_output_writer(FILE* f, void* data) {
    IrModule* mod = (IrModule*)data;
    output__print_ir_module(mod, f);
}

static const char* detect_target_os(void) {
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

static const char* detect_target_arch(void) {
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

static const char* detect_target_bits(void) {
    if (sizeof(void*) == 8) return "64";
    if (sizeof(void*) == 4) return "32";
    if (sizeof(void*) == 2) return "16";
    if (sizeof(void*) == 1) return "8";
    return "unknown";
}

static int process_one_file(const char* filename, const char* output_file,
                            FlagSet flags, const Arguments* args,
                            SemanticContext** semantic_ctx) {
    int err = 0;
    size_t file_size = 0;
    char* raw = NULL;
    char* processed = NULL;
    Lexer* lexer = NULL;
    AST* ast = NULL;
    IrModule* ir_mod = NULL;
    const char** lines = NULL;
    size_t line_count = 0;
    errhandler__set_current_filename(filename);
    raw = read_file_contents(filename, &file_size);
    if (!raw) { err = 1; goto cleanup; }
    processed = preprocess(raw, filename, NULL);
    if (!processed) {
        errhandler__report_error(ERROR_CODE_COM_FAILCREATE, 0, 0, "preproc",
                                 "Preprocessing failed for file: %s", filename);
        err = 1; goto cleanup;
    }
    lines = split_into_lines(processed, &line_count);
    if (lines) errhandler__set_source_code(lines, line_count);
    lexer = lexer__init_lexer(processed);
    if (!lexer) { err = 1; goto cleanup; }
    lexer__tokenize(lexer);
    write_debug_output(flags, F_DEBUG_LEXICAL, lexer_output_writer, lexer);
    if (!errhandler__has_errors()) {
        ast = parse(lexer->tokens, lexer->token_count);
        write_debug_output(flags, F_DEBUG_SYNTAX, parser_output_writer, ast);
    }
    if (*semantic_ctx && ast && !errhandler__has_errors()) {
        if (flags & F_WEXTRA) semantic__set_extra_warnings(*semantic_ctx, true);
        semantic__analyze(*semantic_ctx, ast);
        write_debug_output(flags, F_DEBUG_SEMANTIC, semantic_output_writer, *semantic_ctx);
        if (!errhandler__has_errors()) {
            ir_mod = ir__generate_module(*semantic_ctx, ast);
            if (ir_mod) {
                write_debug_output(flags, F_DEBUG_IR, ir_output_writer, ir_mod);
            } else {
                errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "ir",
                                         "IR module generation failed");
                err = 1;
            }
        }
        if (!errhandler__has_errors()) {
            if (flags & F_DEBUG_OPTIM) {
                optimizer__enable_debug(true);
                optimizer__set_debug_file(stdout);
            } else {
                optimizer__enable_debug(false);
            }
            if (flags & F_DEBUG_OPTIM) {
                char* opt_filename = derive_optimized_ast_filename(filename);
                if (opt_filename) {
                    FILE* opt_fp = fopen(opt_filename, "w");
                    if (opt_fp) {
                        print_optimized_ast(ast, opt_fp);
                        fclose(opt_fp);
                    } else {
                        errhandler__report_error(ERROR_CODE_IO_WRITE, 0, 0, "file",
                                                 "Cannot open optimized AST debug file: %s",
                                                 opt_filename);
                    }
                    memory_free_safe((void**)&opt_filename);
                }
            }
            if (!optimizer__optimize(ast, (*semantic_ctx)->global_scope)) err = 1;
            write_debug_output(flags, F_DEBUG_OPTIM, optimizer_output_writer, ast);
        }
    }
    if ((flags & F_OUTPUT_ASSEMBLY) && ir_mod && !errhandler__has_errors() && output_file) {
        FILE *asm_out = fopen(output_file, "w");
        if (asm_out) {
            fprintf(asm_out, "; ARM AArch64 assembly placeholder for %s\n", filename);
            fclose(asm_out);
        } else {
            errhandler__report_error(ERROR_CODE_IO_WRITE, 0, 0, "file",
                                     "Cannot open assembly output: %s", output_file);
        }
    }
cleanup:
    errhandler__clear_source_code();
    if (lines) free_lines(lines, line_count);
    if (ir_mod) ir__module_destroy(ir_mod);
    if (ast) parser__free_ast(ast);
    if (lexer) lexer__free_lexer(lexer);
    memory_free_safe((void**)&processed);
    memory_free_safe((void**)&raw);
    errhandler__set_current_filename(NULL);
    return err || errhandler__has_errors();
}

int main(int argc, char* argv[]) {
    int expanded_argc = argc;
    char** expanded_argv = NULL;
    expanded_argv = (char**)memory_allocate_zero(argc * sizeof(char*));
    if (!expanded_argv) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                 "Failed to allocate argv copy");
        return 1;
    }
    for (int i = 0; i < argc; ++i) expanded_argv[i] = u__strdup_safe(argv[i]);
    if (!expand_at_files(&expanded_argc, &expanded_argv)) {
        for (int i = 0; i < expanded_argc; ++i) memory_free_safe((void**)&expanded_argv[i]);
        memory_free_safe((void**)&expanded_argv);
        return 1;
    }
    Arguments args = {0};
    int parse_result = parse_arguments(expanded_argc, expanded_argv, &args);
    for (int i = 0; i < expanded_argc; ++i) memory_free_safe((void**)&expanded_argv[i]);
    memory_free_safe((void**)&expanded_argv);
    if (parse_result == -1) {
        if (args.output_file) memory_free_safe((void**)&args.output_file);
        for (size_t i = 0; i < args.file_count; ++i) memory_free_safe((void**)&args.filenames[i]);
        for (size_t i = 0; i < args.lib_count; ++i) memory_free_safe((void**)&args.libraries[i]);
        memory_free_safe((void**)&args.filenames);
        memory_free_safe((void**)&args.libraries);
        return args.exit_code;
    }
    if (parse_result == 0) {
        if (args.output_file) memory_free_safe((void**)&args.output_file);
        for (size_t i = 0; i < args.file_count; ++i) memory_free_safe((void**)&args.filenames[i]);
        for (size_t i = 0; i < args.lib_count; ++i) memory_free_safe((void**)&args.libraries[i]);
        memory_free_safe((void**)&args.filenames);
        memory_free_safe((void**)&args.libraries);
        return 1;
    }
    errhandler__set_warnings_as_errors((args.flags & F_WERROR) != 0);
    errhandler__set_suppress_warnings((args.flags & F_WIGNOR) != 0);
    if (u__streq(args.target_arch, "nativ")) args.target_arch = detect_target_arch();
    if (u__streq(args.target_core, "nativ")) args.target_core = detect_target_os();
    if (u__streq(args.target_bits, "nativ")) args.target_bits = detect_target_bits();
    builtin_target_os = args.target_core;
    builtin_target_arch = args.target_arch;
    builtin_target_bits = args.target_bits;
    if ((args.flags & F_OUTPUT_ASSEMBLY) && args.output_file && args.file_count > 1) {
        errhandler__report_error(ERROR_CODE_INPUT_INVALID_FLAG, 0, 0, "input",
                                 "cannot specify -o with -S and multiple source files");
    }
    if ((args.flags & (F_MODE_COMPILE | F_MODE_STATIC_LIB)) && !args.output_file) {
        if (!(args.flags & F_OUTPUT_ASSEMBLY)) {
            errhandler__report_error(ERROR_CODE_INPUT_NO_SOURCE, 0, 0, "input",
                                     "compilation or static library requested but no output file specified");
        }
    }
    if (args.file_count == 0 && args.flags) {
        errhandler__report_error(ERROR_CODE_INPUT_NO_SOURCE, 0, 0, "input",
                                 "no input source files specified");
    }
    if (errhandler__has_errors()) {
        errhandler__print_errors();
        errhandler__print_warnings();
        goto cleanup_args;
    }
    SemanticContext* semantic_ctx = NULL;
    if ((args.flags & (F_MODE_COMPILE | F_OUTPUT_ASSEMBLY | F_MODE_STATIC_LIB)) ||
        (args.flags & F_DEBUG_SEMANTIC)) {
        semantic_ctx = semantic__create_context();
        if (!semantic_ctx) {
            errhandler__report_error(ERROR_CODE_COM_FAILCREATE, 0, 0, "syntax",
                                     "Failed to create semantic analysis context");
        } else {
            semantic_ctx->exit_on_error = ((args.flags & F_MODE_COMPILE) ||
                                           (args.flags & F_MODE_STATIC_LIB) ||
                                           (args.flags & F_OUTPUT_ASSEMBLY)) != 0;
            if (args.flags & F_WEXTRA) semantic__set_extra_warnings(semantic_ctx, true);
        }
    }
    int exit_code = 0;
    for (size_t i = 0; i < args.file_count; ++i) {
        const char* out_name = NULL;
        if (args.flags & F_OUTPUT_ASSEMBLY) {
            out_name = derive_assembly_filename(args.filenames[i]);
            if (!out_name) {
                errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                         "could not derive assembly filename for %s", args.filenames[i]);
                exit_code = 1;
                continue;
            }
        } else {
            out_name = args.output_file;
        }
        if (process_one_file(args.filenames[i], out_name, args.flags, &args, &semantic_ctx))
            exit_code = 1;
        if ((args.flags & F_OUTPUT_ASSEMBLY) && out_name) memory_free_safe((void**)&out_name);
        if (semantic_ctx && i + 1 < args.file_count) {
            semantic__destroy_context(semantic_ctx);
            semantic_ctx = semantic__create_context();
            if (semantic_ctx) {
                semantic_ctx->exit_on_error = ((args.flags & F_MODE_COMPILE) ||
                                               (args.flags & F_MODE_STATIC_LIB) ||
                                               (args.flags & F_OUTPUT_ASSEMBLY)) != 0;
                if (args.flags & F_WEXTRA) semantic__set_extra_warnings(semantic_ctx, true);
            } else {
                errhandler__report_error(ERROR_CODE_COM_FAILCREATE, 0, 0, "syntax",
                                         "Failed to recreate semantic context");
                exit_code = 1;
            }
        }
    }
    if ((args.flags & F_MODE_STATIC_LIB) && !exit_code) {
        /* output_create_static_library(args.output_file, ...); */
    }
    errhandler__print_errors();
    errhandler__print_warnings();
    if (semantic_ctx) semantic__destroy_context(semantic_ctx);
cleanup_args:
    if (args.output_file) memory_free_safe((void**)&args.output_file);
    for (size_t i = 0; i < args.file_count; ++i) memory_free_safe((void**)&args.filenames[i]);
    for (size_t i = 0; i < args.lib_count; ++i) memory_free_safe((void**)&args.libraries[i]);
    memory_free_safe((void**)&args.filenames);
    memory_free_safe((void**)&args.libraries);
    errhandler__free_error_manager();
    return exit_code;
}
