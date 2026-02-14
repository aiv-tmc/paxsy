#include <stdio.h>
#include <string.h>

#include "preprocessor/preprocessor.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "output/output.h"
#include "semantic/semantic.h"

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

typedef uint32_t FlagSet;

enum {
    F_WRITE_LEXER        = 1U << 0,
    F_WRITE_PARSER       = 1U << 1,
    F_WRITE_SEMANTIC     = 1U << 2,
    F_LOG_SEMANTIC_LOG   = 1U << 3,

    F_LOG_LEXER          = 1U << 4,
    F_LOG_PARSER         = 1U << 5,
    F_LOG_SEMANTIC       = 1U << 6,
    F_LOG_STATE          = 1U << 7,
    F_LOG_VERBOSE        = 1U << 8,

    F_MODE_COMPILE       = 1U << 9,
};

/** Initial capacity for filename array (grows as needed). */
#define FILENAMES_BLOCK 8

/** Suffixes used for generated output files. */
static const char* const SUFFIX_LEXER        = "_lexer.txt";
static const char* const SUFFIX_PARSER       = "_parser.txt";
static const char* const SUFFIX_SEMANTIC     = "_semantic.txt";
static const char* const SUFFIX_SEMANTIC_LOG = "_semantic_log.txt";

typedef void (*OutputWriterEx)(FILE*, void*);

static char*          read_file_contents(const char* filename, size_t* out_size);
static char*          build_output_filename(const char* input, const char* suffix);
static void           write_debug_output(const char* input_filename,
                                         FlagSet flags,
                                         FlagSet required_flag,
                                         const char* suffix,
                                         OutputWriterEx writer,
                                         void* userdata);
static const char**   split_into_lines(const char* text, size_t* out_count);
static void           free_lines(const char** lines, size_t count);

static void           lexer_output_writer(FILE* f, void* data);
static void           parser_output_writer(FILE* f, void* data);
static void           semantic_output_writer(FILE* f, void* data);
static void           semantic_log_writer(FILE* f, void* data);

typedef struct {
    FlagSet flags;
    char**  filenames;
    size_t  file_count;
    size_t  capacity;
    int     exit_code;
} Arguments;

/**
 * @brief Safely duplicates a string.
 * @return Newly allocated copy, or NULL on allocation failure.
 */
static char* strdup_safe(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* copy = (char*)memory_allocate_zero(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

/**
 * @brief Adds a filename to the argument structure (makes a private copy).
 * @return 1 on success, 0 on error.
 */
static int collect_filename(const char* fname, Arguments* args) {
    /* Validate extension */
    size_t len = strlen(fname);
    if (len < 3 || !str_endw(fname, ".px")) {
        errhandler__report_error(ERROR_CODE_IO_FILE_NOT_FOUND, 0, 0, "file",
                                 "File '%s' has invalid extension. Only .px files are supported.",
                                 fname);
        return 0;
    }

    /* Check for duplicates */
    for (size_t i = 0; i < args->file_count; ++i) {
        if (streq(args->filenames[i], fname)) {
            errhandler__report_error(ERROR_CODE_IO_DOUBLE_FILE, 0, 0, "file",
                                     "Duplicate file: %s", fname);
            return 0;
        }
    }

    /* Grow array if needed */
    if (args->file_count >= args->capacity) {
        size_t new_cap = (args->capacity == 0) ? FILENAMES_BLOCK : args->capacity * 2;
        char** new_f = (char**)memory_reallocate_zero(
            args->filenames,
            args->capacity * sizeof(char*),
            new_cap * sizeof(char*));
        if (!new_f) {
            errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                     "Failed to grow filename array");
            return 0;
        }
        args->filenames = new_f;
        args->capacity = new_cap;
    }

    /* Store a copy of the filename */
    char* copy = strdup_safe(fname);
    if (!copy) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                 "Failed to duplicate filename");
        return 0;
    }
    args->filenames[args->file_count++] = copy;
    return 1;
}

/**
 * @brief Prints the usage help screen.
 */
static void print_usage(void) {
    printf("\033[93mUSAGE:\033[0m paxsy \033[1m[operations] <source>\033[0m ...\n"
           "operations:\n"
           "  \033[1m -h  --help\033[0m                              Display this information\n"
           "  \033[1m -v  --version\033[0m                           Display compiler version\n"
           "  \033[1m -wl --write-lexer\033[0m <source>             Display lexer output only\n"
           "  \033[1m -wp --write-parser\033[0m <source>            Display parser output only\n"
           "  \033[1m -ws --write-semantic\033[0m <source>          Display semantic analysis output\n"
           "  \033[1m -wsl --write-semantic-log\033[0m <source>     Display semantic analysis log\n"
           "  \033[1m -w  --write\033[0m <source>                   Display all outputs (lexer, parser, semantic)\n"
           "  \033[1m -l  --log\033[0m <source>                     Write all outputs to files\n"
           "  \033[1m -ll --log-lexer\033[0m <source>               Write lexer output to file\n"
           "  \033[1m -lp --log-parser\033[0m <source>              Write parser output to file\n"
           "  \033[1m -ls --log-semantic\033[0m <source>            Write semantic analysis output to file\n"
           "  \033[1m -lsl --log-semantic-log\033[0m <source>       Write semantic analysis log to file\n"
           "  \033[1m -lst --log-state\033[0m <source>              Write state output to file\n"
           "  \033[1m -lv --log-verbose\033[0m <source>             Write verbose output to file\n"
           "  \033[1m -c  --compile\033[0m <source>                 Compile (no output unless errors)\n");
}

/**
 * @brief Prints the version banner.
 */
static void print_version(void) {
    printf("paxsy %s %s\n"
           "\033[1m%s\033[0m - \033[1m%s\033[0m\n"
           "\n"
           "Developed by AIV\n"
           "This free software is distributed under the MIT General Public License\n",
           GENERATION, NAME, VERSION, DATE);
}

/**
 * @brief Checks if a command line argument starts with a given prefix.
 * @param arg     The argument string.
 * @param prefix  The prefix to compare (without '=').
 * @param out_rest If not NULL and prefix matches and '=' follows,
 *                 points to the character after '=', otherwise NULL.
 * @return 1 if argument starts with prefix (and optionally '='), 0 otherwise.
 */
static int arg_matches(const char* arg, const char* prefix, const char** out_rest) {
    if (out_rest) *out_rest = NULL;
    size_t plen = strlen(prefix);
    if (strncmp(arg, prefix, plen) != 0) return 0;

    if (arg[plen] == '\0') {
        return 1;   /* exact match */
    }
    if (arg[plen] == '=') {
        if (out_rest) *out_rest = arg + plen + 1;
        return 1;
    }
    return 0;
}

/**
 * @brief Parses command line arguments.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param args Output structure (caller must free args.filenames and each element).
 * @return 1 on success, 0 on fatal error, -1 if program should exit immediately.
 */
static int parse_arguments(int argc, char* argv[], Arguments* args) {
    memset(args, 0, sizeof(*args));
    args->capacity = (argc / 2) + FILENAMES_BLOCK;
    args->filenames = (char**)memory_allocate_zero(args->capacity * sizeof(char*));
    if (!args->filenames) {
        errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                 "Failed to allocate filename array");
        return 0;
    }

    uint8_t mode_seen = 0;
    const char* rest = NULL;

    for (int i = 1; i < argc; ++i) {
        char* arg = argv[i];

        /* Non‑option arguments are collected as source files */
        if (arg[0] != '-') {
            collect_filename(arg, args);
            continue;
        }

        if (streq(arg, "--help") || streq(arg, "-h")) {
            print_usage();
            args->exit_code = 0;
            return -1;
        }
        if (streq(arg, "--version") || streq(arg, "-v")) {
            print_version();
            args->exit_code = 0;
            return -1;
        }

        if (arg_matches(arg, "--write-lexer", &rest) || arg_matches(arg, "-wl", &rest)) {
            args->flags |= F_WRITE_LEXER;
            if (rest) collect_filename(rest, args);
            continue;
        }
        if (arg_matches(arg, "--write-parser", &rest) || arg_matches(arg, "-wp", &rest)) {
            args->flags |= F_WRITE_PARSER;
            if (rest) collect_filename(rest, args);
            continue;
        }
        if (arg_matches(arg, "--write-semantic", &rest) || arg_matches(arg, "-ws", &rest)) {
            args->flags |= F_WRITE_SEMANTIC;
            if (rest) collect_filename(rest, args);
            continue;
        }
        if (arg_matches(arg, "--write-semantic-log", &rest) || arg_matches(arg, "-wsl", &rest)) {
            args->flags |= F_WRITE_SEMANTIC | F_LOG_SEMANTIC_LOG;
            if (rest) collect_filename(rest, args);
            continue;
        }
        if (arg_matches(arg, "--write", &rest) || arg_matches(arg, "-w", &rest)) {
            args->flags |= F_WRITE_LEXER | F_WRITE_PARSER | F_WRITE_SEMANTIC;
            if (rest) collect_filename(rest, args);
            continue;
        }

        if (arg_matches(arg, "--log", &rest) || arg_matches(arg, "-l", &rest)) {
            args->flags |= F_LOG_LEXER | F_LOG_PARSER | F_LOG_SEMANTIC |
                           F_LOG_STATE | F_LOG_VERBOSE;
            if (rest) collect_filename(rest, args);
            continue;
        }
        if (arg_matches(arg, "--log-lexer", &rest) || arg_matches(arg, "-ll", &rest)) {
            args->flags |= F_LOG_LEXER;
            if (rest) collect_filename(rest, args);
            continue;
        }
        if (arg_matches(arg, "--log-parser", &rest) || arg_matches(arg, "-lp", &rest)) {
            args->flags |= F_LOG_PARSER;
            if (rest) collect_filename(rest, args);
            continue;
        }
        if (arg_matches(arg, "--log-semantic", &rest) || arg_matches(arg, "-ls", &rest)) {
            args->flags |= F_LOG_SEMANTIC;
            if (rest) collect_filename(rest, args);
            continue;
        }
        if (arg_matches(arg, "--log-semantic-log", &rest) || arg_matches(arg, "-lsl", &rest)) {
            args->flags |= F_LOG_SEMANTIC_LOG;
            if (rest) collect_filename(rest, args);
            continue;
        }
        if (arg_matches(arg, "--log-state", &rest) || arg_matches(arg, "-lst", &rest)) {
            args->flags |= F_LOG_STATE;
            if (rest) collect_filename(rest, args);
            continue;
        }
        if (arg_matches(arg, "--log-verbose", &rest) || arg_matches(arg, "-lv", &rest)) {
            args->flags |= F_LOG_VERBOSE;
            if (rest) collect_filename(rest, args);
            continue;
        }

        if (arg_matches(arg, "--compile", &rest) || arg_matches(arg, "-c", &rest)) {
            if (mode_seen) {
                errhandler__report_error(ERROR_CODE_INPUT_MULTI_MOD_FLAGS, 0, 0, "input",
                                         "Multiple mode flags specified");
            } else {
                args->flags |= F_MODE_COMPILE;
                mode_seen = 1;
            }
            if (rest) collect_filename(rest, args);
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

static char* build_output_filename(const char* input, const char* suffix) {
    const char* base = input;
    const char* p = base;
    const char* last_slash = base;

    while (*p) {
        if (*p == '/' || *p == '\\')
            last_slash = p + 1;
        ++p;
    }
    base = last_slash;

    const char* dot = NULL;
    for (const char* d = p - 1; d >= base; --d) {
        if (*d == '.') {
            dot = d;
            break;
        }
    }

    size_t name_len = dot ? (size_t)(dot - base) : (size_t)(p - base);
    size_t total_len = name_len + strlen(suffix) + 1;
    char* out = (char*)memory_allocate_zero(total_len);
    if (!out) return NULL;

    memcpy(out, base, name_len);
    memcpy(out + name_len, suffix, strlen(suffix) + 1);
    return out;
}

static void write_debug_output(const char* input_filename,
                               FlagSet flags,
                               FlagSet required_flag,
                               const char* suffix,
                               OutputWriterEx writer,
                               void* userdata) {
    if (!(flags & required_flag))
        return;

    if (flags & F_MODE_COMPILE) {
        /* Write to file */
        char* fname = build_output_filename(input_filename, suffix);
        if (!fname) {
            errhandler__report_error(ERROR_CODE_MEMORY_ALLOCATION, 0, 0, "memory",
                                     "Cannot allocate output filename");
            return;
        }
        FILE* f = fopen(fname, "w");
        if (f) {
            writer(f, userdata);
            fclose(f);
            /* Only notify about file creation in compile mode */
            printf("%s written to: %s\n", suffix + 1, fname);
        } else {
            errhandler__report_error(ERROR_CODE_IO_WRITE, 0, 0, "file",
                                     "Cannot open %s for writing", fname);
        }
        memory_free_safe((void**)&fname);
    } else {
        /* Write to stdout */
        writer(stdout, userdata);
    }
}

static const char** split_into_lines(const char* text, size_t* out_count) {
    *out_count = 0;
    if (!text || !*text)
        return NULL;

    size_t cap = 32;
    const char** lines = (const char**)memory_allocate_zero(cap * sizeof(char*));
    if (!lines)
        return NULL;

    size_t idx = 0;
    const char* start = text;
    const char* p = text;

    while (*p) {
        if (char_is_line_break(*p)) {
            size_t len = p - start;
            char* copy = (char*)memory_allocate_zero(len + 1);
            if (!copy)
                goto fail;
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

    /* Last line (no trailing newline) */
    if (start < p) {
        size_t len = p - start;
        char* copy = (char*)memory_allocate_zero(len + 1);
        if (!copy)
            goto fail;
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

    /* Shrink to exact size (optional, safe even if realloc fails) */
    if (idx < cap) {
        const char** shrunk = (const char**)memory_reallocate_zero(
            lines, cap * sizeof(char*), idx * sizeof(char*));
        if (shrunk)
            lines = shrunk;
    }
    return lines;

fail:
    for (size_t i = 0; i < idx; ++i)
        memory_free_safe((void**)&lines[i]);
    memory_free_safe((void**)&lines);
    return NULL;
}

static void free_lines(const char** lines, size_t count) {
    if (!lines) return;
    for (size_t i = 0; i < count; ++i)
        memory_free_safe((void**)&lines[i]);
    memory_free_safe((void**)&lines);
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

static void semantic_log_writer(FILE* f, void* data) {
    SemanticContext* ctx = (SemanticContext*)data;
    print_semantic_log(ctx, f);
}

/**
 * @brief Processes a single source file through the entire compiler pipeline.
 * @param filename      Source file to process.
 * @param flags         Global operation flags.
 * @param semantic_ctx  Pointer to the semantic context (may be recreated).
 * @return 0 on success, non‑zero on error.
 */
static int process_one_file(const char* filename,
                            FlagSet flags,
                            SemanticContext** semantic_ctx) {
    int err = 0;
    size_t file_size = 0;
    char* raw = NULL;
    char* processed = NULL;
    Lexer* lexer = NULL;
    AST* ast = NULL;
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
    if (lines) {
        errhandler__set_source_code(lines, line_count);
    }

    lexer = lexer__init_lexer(processed);
    if (!lexer) { err = 1; goto cleanup; }
    lexer__tokenize(lexer);

    /* Lexer output (stdout or file) */
    if (!(flags & F_MODE_COMPILE) && (flags & F_WRITE_LEXER)) {
        write_debug_output(filename, flags, F_WRITE_LEXER, SUFFIX_LEXER,
                           lexer_output_writer, lexer);
    }
    if (flags & F_LOG_LEXER) {
        write_debug_output(filename, flags, F_LOG_LEXER, SUFFIX_LEXER,
                           lexer_output_writer, lexer);
    }

    if (!errhandler__has_errors()) {
        ast = parse(lexer->tokens, lexer->token_count);

        /* Parser output (stdout or file) */
        if (!(flags & F_MODE_COMPILE) && (flags & F_WRITE_PARSER)) {
            write_debug_output(filename, flags, F_WRITE_PARSER, SUFFIX_PARSER,
                               parser_output_writer, ast);
        }
        if (flags & F_LOG_PARSER) {
            write_debug_output(filename, flags, F_LOG_PARSER, SUFFIX_PARSER,
                               parser_output_writer, ast);
        }
    }

    if (*semantic_ctx && ast && !errhandler__has_errors()) {
        uint8_t semantic_ok = semantic__analyze(*semantic_ctx, ast);

        /* Semantic analysis output (stdout) – only if not in compile mode */
        if (!(flags & F_MODE_COMPILE) && (flags & F_WRITE_SEMANTIC)) {
            write_debug_output(filename, flags, F_WRITE_SEMANTIC, SUFFIX_SEMANTIC,
                               semantic_output_writer, *semantic_ctx);
        }
        /* Semantic log to stdout (only if explicitly requested and not in compile mode) */
        if (!(flags & F_MODE_COMPILE) && (flags & F_LOG_SEMANTIC_LOG)) {
            print_semantic_log(*semantic_ctx, stdout);
            fprintf(stdout, "\n");
        }

        /* Semantic analysis to file */
        if (flags & F_LOG_SEMANTIC) {
            write_debug_output(filename, flags, F_LOG_SEMANTIC, SUFFIX_SEMANTIC,
                               semantic_output_writer, *semantic_ctx);
        }
        /* Semantic log to file */
        if (flags & F_LOG_SEMANTIC_LOG) {
            write_debug_output(filename, flags, F_LOG_SEMANTIC_LOG, SUFFIX_SEMANTIC_LOG,
                               semantic_log_writer, *semantic_ctx);
        }

        /* In compile mode, if there are semantic errors, print them to stdout */
        if ((flags & F_MODE_COMPILE) && !semantic_ok) {
            print_section_header("SEMANTIC ANALYSIS - ERRORS", stdout);
            print_semantic_analysis(*semantic_ctx, stdout);
            fprintf(stdout, "\n");
        }
    }

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

int main(int argc, char* argv[]) {
    Arguments args = {0};
    int parse_result = parse_arguments(argc, argv, &args);

    if (parse_result == -1) {
        /* Free any allocated filenames (should be none, but safe) */
        for (size_t i = 0; i < args.file_count; ++i)
            memory_free_safe((void**)&args.filenames[i]);
        memory_free_safe((void**)&args.filenames);
        return args.exit_code;
    }
    if (parse_result == 0) {
        for (size_t i = 0; i < args.file_count; ++i)
            memory_free_safe((void**)&args.filenames[i]);
        memory_free_safe((void**)&args.filenames);
        return 1;
    }

    /* Validate combination of flags and files */
    FlagSet ops = args.flags & ~F_MODE_COMPILE;
    if (args.file_count == 0 && ops) {
        errhandler__report_error(ERROR_CODE_INPUT_NO_SOURCE, 0, 0, "input",
                                 "no input file specified");
    }
    if (args.file_count > 0 && ops == 0 && !(args.flags & F_MODE_COMPILE)) {
        errhandler__report_error(ERROR_CODE_INPUT_INVALID_FLAG, 0, 0, "input",
                                 "Files can only be processed with -c, -w*, -l* flags");
    }

    if (errhandler__has_errors()) {
        errhandler__print_errors();
        errhandler__print_warnings();
        goto cleanup_args;
    }

    /* Create semantic context if needed */
    SemanticContext* semantic_ctx = NULL;
    if (args.flags & (F_MODE_COMPILE | F_WRITE_SEMANTIC |
                      F_LOG_SEMANTIC | F_LOG_SEMANTIC_LOG)) {
        semantic_ctx = semantic__create_context();
        if (!semantic_ctx) {
            errhandler__report_error(ERROR_CODE_COM_FAILCREATE, 0, 0, "syntax",
                                     "Failed to create semantic analysis context");
        } else {
            semantic__set_exit_on_error(semantic_ctx, (args.flags & F_MODE_COMPILE) != 0);
        }
    }

    int exit_code = 0;
    for (size_t i = 0; i < args.file_count; ++i) {
        if (process_one_file(args.filenames[i], args.flags, &semantic_ctx))
            exit_code = 1;

        /* For multiple files, recreate semantic context for each */
        if (semantic_ctx && i + 1 < args.file_count) {
            semantic__destroy_context(semantic_ctx);
            semantic_ctx = semantic__create_context();
            if (semantic_ctx) {
                semantic__set_exit_on_error(semantic_ctx, (args.flags & F_MODE_COMPILE) != 0);
            } else {
                errhandler__report_error(ERROR_CODE_COM_FAILCREATE, 0, 0, "syntax",
                                         "Failed to recreate semantic context");
                exit_code = 1;
            }
        }
    }

    errhandler__print_errors();
    errhandler__print_warnings();

    if (semantic_ctx) semantic__destroy_context(semantic_ctx);

cleanup_args:
    for (size_t i = 0; i < args.file_count; ++i)
        memory_free_safe((void**)&args.filenames[i]);
    memory_free_safe((void**)&args.filenames);
    errhandler__free_error_manager();

    return exit_code;
}
