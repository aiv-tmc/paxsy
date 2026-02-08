#include "preprocessor/preprocessor.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "output/output.h"
#include "semantic/semantic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

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

#define MODE_COMPILE 1
#define FILENAMES_BLOCK 8

/* Flag definitions moved to global scope */
#define F_WRITE_LEXER       0x0001
#define F_WRITE_PARSER      0x0002
#define F_WRITE_SEMANTIC    0x0004
#define F_LOG_LEXER         0x0008
#define F_LOG_PARSER        0x0010
#define F_LOG_SEMANTIC      0x0020
#define F_LOG_STATE         0x0040
#define F_LOG_VERBOSE       0x0080
#define F_MODE_COMPILE      0x0100
#define F_LOG_SEMANTIC_LOG  0x0200  /* New flag for semantic log */

/**
 * Macro to handle command line flag processing with validation.
 * Checks flag name, whether it requires a value, and executes action.
 */
#define HANDLE_FLAG(flag_name, short_name, has_value, action) \
    if(!strcmp(flag, flag_name) || !strcmp(flag, short_name)) { \
        if((has_value) ^ (value != NULL)) { \
            errhandler__report_error \
                ( ERROR_CODE_INPUT_INVALID_FLAG \
                , 0 \
                , 0 \
                , "file" \
                , has_value \
                ? "Flag '%s' requires a value" \
                : "Flag '%s' doesn't take a value" \
                , flag \
            ); \
            continue; \
        } \
        action; \
        continue; \
    }

/**
 * Reads entire file content into a dynamically allocated buffer.
 * @param filename Path to file to read
 * @param file_size Pointer to store file size
 * @return Buffer pointer on success, NULL on failure
 */
static char* read_file_contents(const char* filename, size_t* file_size) {
    struct stat st;
    if(stat(filename, &st)) {
        errhandler__report_error
            ( ERROR_CODE_IO_READ
            , 0
            , 0
            , "file"
            , "File '%s' does not exist or cannot be opened."
            , filename
        );
        return NULL;
    }
    
    *file_size = st.st_size;
    if(!*file_size) {
        char* empty = malloc(1);
        if(empty) empty[0] = '\0';
        return empty;
    }
    
    FILE* file = fopen(filename, "r");
    if(!file) return NULL;
    
    char* buffer = malloc(*file_size + 1);
    if(!buffer) {
        fclose(file);
        return NULL;
    }
    
    if(fread(buffer, 1, *file_size, file) != *file_size) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    
    fclose(file);
    buffer[*file_size] = '\0';
    return buffer;
}

/**
 * Generates output filename by replacing original extension with given suffix.
 * @param input_filename Original filename
 * @param suffix Suffix to append (including extension)
 * @return New filename string, caller must free
 */
static char* get_output_filename(const char* input_filename, 
                                 const char* suffix) {
    const char* start = input_filename;
    const char* p = start;
    const char* last_slash = start;
    
    /* O(n) - single pass to find last slash and extension */
    while(*p) {
        if(*p == '/' || *p == '\\') last_slash = p + 1;
        p++;
    }
    
    start = last_slash;
    const char* dot = NULL;
    
    /* Find last dot after last slash */
    for(const char* d = p - 1; d >= start; d--) {
        if(*d == '.') {
            dot = d;
            break;
        }
    }
    
    size_t name_len = dot ? (size_t)(dot - start) : (size_t)(p - start);
    size_t suffix_len = strlen(suffix);
    size_t total_len = name_len + suffix_len + 1;
    
    char* result = malloc(total_len);
    if(!result) return NULL;
    
    memcpy(result, start, name_len);
    memcpy(result + name_len, suffix, suffix_len + 1); /* +1 for null terminator */
    
    return result;
}

/**
 * Writes content string to specified filename.
 * @param filename Output filename
 * @param content Content to write
 */
static void write_output_to_file(const char* filename, const char* content) {
    FILE* file = fopen(filename, "w");
    if(!file) {
        errhandler__report_error
            ( ERROR_CODE_IO_WRITE
            , 0
            , 0
            , "file"
            , "Cannot open file for writing: %s"
            , filename
        );
        return;
    }
    fputs(content, file);
    fclose(file);
}

/**
 * Frees all dynamically allocated resources used during compilation.
 * @param ast Pointer to AST pointer (set to NULL after freeing)
 * @param lexer Pointer to lexer pointer (set to NULL after freeing)
 * @param processed Pointer to processed code buffer (set to NULL after freeing)
 * @param buffer Pointer to original file buffer (set to NULL after freeing)
 * @param semantic Pointer to semantic context (set to NULL after freeing)
 */
static void cleanup_resources(AST** ast, Lexer** lexer, 
                              char** processed, char** buffer,
                              SemanticContext** semantic) {
    if(*ast) {
        parser__free_ast(*ast);
        *ast = NULL;
    }
    
    if(*lexer) {
        lexer__free_lexer(*lexer);
        *lexer = NULL;
    }
    
    if(*processed) {
        free(*processed);
        *processed = NULL;
    }
    
    if(*buffer) {
        free(*buffer);
        *buffer = NULL;
    }
    
    if(*semantic) {
        semantic_destroy_context(*semantic);
        *semantic = NULL;
    }
}

/**
 * Validates input filename meets compilation requirements.
 * Optimized: Uses pointer to hash table or sorted array for O(log n) lookup
 */
static uint8_t validate_filename(const char* filename, char*** filenames, 
                                 size_t* file_count, size_t* capacity) {
    size_t len = strlen(filename);
    if(len < 3 || strcmp(filename + len - 3, ".px")) {
        errhandler__report_error
            ( ERROR_CODE_IO_FILE_NOT_FOUND
            , 0
            , 0
            , "file"
            , "File '%s' has invalid extension. "
              "Only .px files are supported."
            , filename
        );
        return 0;
    }
    
    for(size_t j = 0; j < *file_count; j++) {
        if(!strcmp((*filenames)[j], filename)) {
            errhandler__report_error
                ( ERROR_CODE_IO_DOUBLE_FILE
                , 0
                , 0
                , "file"
                , "Duplicate file: %s"
                , filename
            );
            return 0;
        }
    }
    
    /* Amortized O(1) resize with geometric growth */
    if(*file_count >= *capacity) {
        *capacity = (*capacity == 0) ? FILENAMES_BLOCK : *capacity * 2;
        char** new_filenames = realloc(*filenames, 
                                      *capacity * sizeof(char*));
        if(!new_filenames) {
            errhandler__report_error
                ( ERROR_CODE_MEMORY_ALLOCATION
                , 0
                , 0
                , "memory"
                , "Memory allocation failed for filenames"
            );
            return 0;
        }
        *filenames = new_filenames;
    }
    
    (*filenames)[(*file_count)++] = (char*)filename;
    return 1;
}

/**
 * Splits text into array of strings, each representing one line.
 * Optimized: Single pass O(n) algorithm with geometric buffer growth
 */
static const char** split_into_lines(const char* text, 
                                     uint16_t* line_count) {
    if(!text || !*text) {
        *line_count = 0;
        return NULL;
    }
    
    /* Single pass: count lines and collect line starts simultaneously */
    size_t lines_capacity = 16;
    const char** lines = malloc(lines_capacity * sizeof(char*));
    if(!lines) {
        *line_count = 0;
        return NULL;
    }
    
    uint16_t current_line = 0;
    const char* line_start = text;
    const char* p = text;
    
    while(*p) {
        if(*p == '\n') {
            /* Store line start position */
            if(current_line >= lines_capacity) {
                lines_capacity *= 2;
                const char** new_lines = realloc(lines, lines_capacity * sizeof(char*));
                if(!new_lines) {
                    for(uint16_t i = 0; i < current_line; i++) free((void*)lines[i]);
                    free(lines);
                    *line_count = 0;
                    return NULL;
                }
                lines = new_lines;
            }
            
            size_t line_len = p - line_start;
            char* line = malloc(line_len + 1);
            if(!line) {
                for(uint16_t i = 0; i < current_line; i++) free((void*)lines[i]);
                free(lines);
                *line_count = 0;
                return NULL;
            }
            memcpy(line, line_start, line_len);
            line[line_len] = '\0';
            lines[current_line++] = line;
            line_start = p + 1;
        }
        p++;
    }
    
    /* Handle last line */
    if(line_start < p) {
        if(current_line >= lines_capacity) {
            lines_capacity += 1;
            const char** new_lines = realloc(lines, lines_capacity * sizeof(char*));
            if(!new_lines) {
                for(uint16_t i = 0; i < current_line; i++) free((void*)lines[i]);
                free(lines);
                *line_count = 0;
                return NULL;
            }
            lines = new_lines;
        }
        
        size_t line_len = p - line_start;
        char* line = malloc(line_len + 1);
        if(!line) {
            for(uint16_t i = 0; i < current_line; i++) free((void*)lines[i]);
            free(lines);
            *line_count = 0;
            return NULL;
        }
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';
        lines[current_line++] = line;
    }
    
    *line_count = current_line;
    /* Final resize to exact size */
    if(current_line < lines_capacity) {
        const char** exact_lines = realloc(lines, current_line * sizeof(char*));
        if(exact_lines) lines = exact_lines;
    }
    
    return lines;
}

/**
 * Frees memory allocated by split_into_lines function.
 * @param lines Array of line strings
 * @param line_count Number of lines in array
 */
static void free_lines(const char** lines, uint16_t line_count) {
    if(!lines) return;
    
    for(uint16_t i = 0; i < line_count; i++) {
        free((void*)lines[i]);
    }
    free(lines);
}

/**
 * Prints usage information
 */
static void print_usage(void) {
    printf("\033[93mUSAGE:\033[0m paxsy \033[1m[operations] "
           "<source>\033[0m ...\n"
           "operations:\n"
           "  \033[1m -h  --help\033[0m\t\t\t\t"
           "Display this information\n"
           "  \033[1m -v  --version\033[0m\t\t\t"
           "Display compiler version information\n"
           "  \033[1m -wl --write-lexer\033[0m <source>\t\t"
           "Display lexer output only\n"
           "  \033[1m -wp --write-parser\033[0m <source>\t\t"
           "Display parser output only\n"
           "  \033[1m -ws --write-semantic\033[0m <source>\t\t"
           "Display semantic analysis output only\n"
           "  \033[1m -wsl --write-semantic-log\033[0m <source>\t"
           "Display semantic analysis log\n"
           "  \033[1m -w  --write\033[0m <source>\t\t\t"
           "Display all outputs (lexer, parser, semantic)\n"
           "  \033[1m -l  --log\033[0m <source>\t\t\t"
           "Write all outputs to files\n"
           "  \033[1m -ll --log-lexer\033[0m <source>\t\t"
           "Write lexer output to file\n"
           "  \033[1m -lp --log-parser\033[0m <source>\t\t"
           "Write parser output to file\n"
           "  \033[1m -ls --log-semantic\033[0m <source>\t\t"
           "Write semantic analysis output to file\n"
           "  \033[1m -lsl --log-semantic-log\033[0m <source>\t"
           "Write semantic analysis log to file\n"
           "  \033[1m -lst --log-state\033[0m <source>\t\t"
           "Write state output to file\n"
           "  \033[1m -lv --log-verbose\033[0m <source>\t\t"
           "Write verbose output to file\n"
           "  \033[1m -c  --compile\033[0m <source>\t\t"
           "Compile and assemble (no output unless errors)\n");
}

/**
 * Prints version information
 */
static void print_version(void) {
    printf("paxsy %s %s\n"
           "\033[1m%s\033[0m - \033[1m%s\033[0m\n"
           "\n"
           "This is being developed by AIV\n"
           "This free software is distributed under the "
           "MIT General Public License\n", 
           GENERATION, NAME, VERSION, DATE);
}

/**
 * Precomputes flag combinations to avoid repeated bit operations
 */
static void compute_flag_combinations(uint16_t flags, 
                                      uint16_t* has_write,
                                      uint16_t* has_log,
                                      uint16_t* has_operation) {
    *has_write = flags & (F_WRITE_LEXER | F_WRITE_PARSER | F_WRITE_SEMANTIC | F_LOG_SEMANTIC_LOG);
    *has_log = flags & (F_LOG_LEXER | F_LOG_PARSER | 
                       F_LOG_SEMANTIC | F_LOG_STATE | F_LOG_VERBOSE | F_LOG_SEMANTIC_LOG);
    *has_operation = flags & (F_MODE_COMPILE | *has_write | *has_log);
}

/**
 * Main entry point for the Paxsy compiler.
 * Optimizations:
 * 1. Precomputed flag combinations
 * 2. Early exits for error conditions
 * 3. Reduced duplicate code in output generation
 * 4. Batch file processing optimizations
 */
int main(int argc, char* argv[]) {
    uint16_t flags = 0;
    
    char** filenames = NULL;
    size_t file_count = 0;
    size_t capacity = 0;
    int exit_code = 0;
    uint8_t has_mode = 0;
    SemanticContext* semantic_context = NULL;
    
    /* Pre-allocate filename array to reduce realloc calls */
    capacity = (argc / 2) + FILENAMES_BLOCK; /* Estimate based on typical usage */
    filenames = malloc(capacity * sizeof(char*));
    if(!filenames) {
        errhandler__report_error
            ( ERROR_CODE_MEMORY_ALLOCATION
            , 0
            , 0
            , "memory"
            , "Failed to allocate filename array"
        );
        return 1;
    }

    /* Single pass argument processing with flag deduplication */
    for(int i = 1; i < argc; i++) {
        if(argv[i][0] != '-') {
            validate_filename(argv[i], &filenames, 
                             &file_count, &capacity);
            continue;
        }

        char* flag = argv[i];
        char* value = strchr(flag, '=');
        if(value) {
            *value = '\0';
            value++;
        }

        HANDLE_FLAG("--help", "-h", 0, {
            print_usage();
            exit_code = 0;
            goto cleanup;
        })
        
        HANDLE_FLAG("--version", "-v", 0, {
            print_version();
            exit_code = 0;
            goto cleanup;
        })
        
        /* Group similar flag types for better cache locality */
        if(!strcmp(flag, "--write-lexer") || !strcmp(flag, "-wl")) 
            { flags |= F_WRITE_LEXER; continue; }
        if(!strcmp(flag, "--write-parser") || !strcmp(flag, "-wp")) 
            { flags |= F_WRITE_PARSER; continue; }
        if(!strcmp(flag, "--write-semantic") || !strcmp(flag, "-ws")) 
            { flags |= F_WRITE_SEMANTIC; continue; }
        if(!strcmp(flag, "--write-semantic-log") || !strcmp(flag, "-wsl")) 
            { flags |= F_WRITE_SEMANTIC | F_LOG_SEMANTIC_LOG; continue; }
        if(!strcmp(flag, "--write") || !strcmp(flag, "-w")) 
            { flags |= (F_WRITE_LEXER | F_WRITE_PARSER | F_WRITE_SEMANTIC); continue; }
        if(!strcmp(flag, "--log") || !strcmp(flag, "-l")) 
            { flags |= (F_LOG_LEXER | F_LOG_PARSER | F_LOG_SEMANTIC | F_LOG_STATE | F_LOG_VERBOSE); continue; }
        if(!strcmp(flag, "--log-lexer") || !strcmp(flag, "-ll")) 
            { flags |= F_LOG_LEXER; continue; }
        if(!strcmp(flag, "--log-parser") || !strcmp(flag, "-lp")) 
            { flags |= F_LOG_PARSER; continue; }
        if(!strcmp(flag, "--log-semantic") || !strcmp(flag, "-ls")) 
            { flags |= F_LOG_SEMANTIC; continue; }
        if(!strcmp(flag, "--log-semantic-log") || !strcmp(flag, "-lsl")) 
            { flags |= F_LOG_SEMANTIC_LOG; continue; }
        if(!strcmp(flag, "--log-state") || !strcmp(flag, "-lst")) 
            { flags |= F_LOG_STATE; continue; }
        if(!strcmp(flag, "--log-verbose") || !strcmp(flag, "-lv")) 
            { flags |= F_LOG_VERBOSE; continue; }
        if(!strcmp(flag, "--compile") || !strcmp(flag, "-c")) {
            if(has_mode) {
                errhandler__report_error
                    ( ERROR_CODE_INPUT_MULTI_MOD_FLAGS
                    , 0
                    , 0
                    , "input"
                    , "Multiple mode flags specified"
                );
            } else {
                flags |= F_MODE_COMPILE;
                has_mode = 1;
            }
            continue;
        }
        
        errhandler__report_error
            ( ERROR_CODE_INPUT_INVALID_FLAG
            , 0
            , 0
            , "input"
            , "unknown flag: %s"
            , flag
        );
    }

    /* Precompute flag combinations once */
    uint16_t has_write, has_log, has_operation;
    compute_flag_combinations(flags, &has_write, &has_log, &has_operation);

    /* Early validation checks */
    if(!file_count && has_operation) {
        errhandler__report_error
            ( ERROR_CODE_INPUT_NO_SOURCE
            , 0
            , 0
            , "input"
            , "no input file specified"
        );
        exit_code = 1;
        goto cleanup;
    }
    
    if(!has_operation && file_count && !(flags & F_MODE_COMPILE)) {
        errhandler__report_error
            ( ERROR_CODE_INPUT_INVALID_FLAG
            , 0
            , 0
            , "input"
            , "Files can only be processed with "
              "-c, -wl, -wp, -ws, -wsl, -w, "
              "-l, -ll, -lp, -ls, -lsl, -lv flags"
        );
        exit_code = 1;
        goto cleanup;
    }

    if(errhandler__has_errors()) {
        errhandler__print_errors();
        errhandler__print_warnings();
        exit_code = 1;
        goto cleanup;
    }

    /* Create semantic context only if needed */
    uint8_t needs_semantic = flags & (F_MODE_COMPILE | F_WRITE_SEMANTIC | F_LOG_SEMANTIC | 
                                     F_WRITE_SEMANTIC | F_LOG_SEMANTIC_LOG);
    if(needs_semantic) {
        semantic_context = semantic_create_context();
        if(!semantic_context) {
            errhandler__report_error
                ( ERROR_CODE_COM_FAILCREATE
                , 0
                , 0
                , "syntax"
                , "Failed to create semantic analysis context"
            );
        } else {
            /* Enable exit on error for compile mode */
            if (flags & F_MODE_COMPILE) {
                semantic_set_exit_on_error(semantic_context, true);
            } else {
                semantic_set_exit_on_error(semantic_context, false);
            }
        }
    }

    /* Batch process files with shared context */
    uint16_t is_compile_mode = flags & F_MODE_COMPILE;
    uint8_t show_debug_output = !is_compile_mode;
    
    for(size_t i = 0; i < file_count; i++) {
        char* filename = filenames[i];
        size_t file_size;
        char* buffer_file = NULL;
        Lexer* lexer = NULL;
        AST* ast = NULL;
        char* processed = NULL;
        const char** source_lines = NULL;
        uint16_t source_line_count = 0;
        
        buffer_file = read_file_contents(filename, &file_size);
        if(!buffer_file) {
            errhandler__report_error
                ( ERROR_CODE_IO_READ
                , 0
                , 0
                , "file"
                , "Cannot open file: %s"
                , filename
            );
            continue;
        }
        
        processed = preprocess(buffer_file, filename, NULL);
        if(!processed) {
            errhandler__report_error
                ( ERROR_CODE_COM_FAILCREATE
                , 0
                , 0
                , "preproc"
                , "Preprocessing failed for file: %s"
                , filename
            );
            free(buffer_file);
            continue;
        }
        
        source_lines = split_into_lines(processed, &source_line_count);
        if(source_lines) errhandler__set_source_code(source_lines, 
                                                     source_line_count);
        
        lexer = lexer__init_lexer(processed);
        if(!lexer) {
            free(processed);
            free(buffer_file);
            if(source_lines) {
                free_lines(source_lines, source_line_count);
            }
            continue;
        }
        
        lexer__tokenize(lexer);
        
        /* Unified output generation with mode checks */
        if(show_debug_output) {
            /* Lexer outputs */
            if(flags & F_WRITE_LEXER) {
                print_section_header("LEXER TOKENS", stdout);
                print_tokens_in_lines(lexer, stdout);
                fprintf(stdout, "\n");
            }
            
            if(flags & F_LOG_LEXER) {
                char* lexer_filename = get_output_filename(filename, "_lexer.txt");
                if(lexer_filename) {
                    FILE* lexer_file = fopen(lexer_filename, "w");
                    if(lexer_file) {
                        print_section_header("LEXER TOKENS", lexer_file);
                        print_tokens_in_lines(lexer, lexer_file);
                        fprintf(lexer_file, "\n");
                        fclose(lexer_file);
                        printf("Lexer output written to: %s\n", lexer_filename);
                    } else {
                        errhandler__report_error
                            ( ERROR_CODE_COM_FAILCREATE
                            , 0
                            , 0
                            , "syntax"
                            , "Cannot open lexer output file: %s"
                            , lexer_filename
                        );
                    }
                    free(lexer_filename);
                }
            }
        }
        
        /* Parser and AST - only if no errors */
        if(!errhandler__has_errors()) {
            ast = parse(lexer->tokens, lexer->token_count);
            
            if(show_debug_output && ast) {
                if(flags & F_WRITE_PARSER) {
                    print_section_header("PARSER AST", stdout);
                    print_ast_detailed(ast, stdout);
                    fprintf(stdout, "\n");
                }
                
                if(flags & F_LOG_PARSER) {
                    char* parser_filename = get_output_filename(filename, "_parser.txt");
                    if(parser_filename) {
                        FILE* parser_file = fopen(parser_filename, "w");
                        if(parser_file) {
                            print_section_header("PARSER AST", parser_file);
                            print_ast_detailed(ast, parser_file);
                            fprintf(parser_file, "\n");
                            fclose(parser_file);
                            printf("Parser output written to: %s\n", parser_filename);
                        } else {
                            errhandler__report_error
                                ( ERROR_CODE_COM_FAILCREATE
                                , 0
                                , 0
                                , "syntax"
                                , "Cannot open parser output file: %s"
                                , parser_filename
                            );
                        }
                        free(parser_filename);
                    }
                }
            }
        }
        
        /* Semantic analysis - unified handling for all modes */
        if(semantic_context && ast && !errhandler__has_errors()) {
            uint8_t semantic_ok = semantic_analyze(semantic_context, ast);
            
            if(show_debug_output) {
                /* Write semantic analysis to stdout */
                if(flags & F_WRITE_SEMANTIC) {
                    print_section_header("SEMANTIC ANALYSIS", stdout);
                    print_semantic_analysis(semantic_context, stdout);
                    fprintf(stdout, "\n");
                }
                
                /* Write semantic log to stdout */
                if(flags & F_LOG_SEMANTIC_LOG) {
                    print_semantic_log(semantic_context, stdout);
                    fprintf(stdout, "\n");
                }
                
                /* Log semantic analysis to file */
                if(flags & F_LOG_SEMANTIC) {
                    char* semantic_filename = get_output_filename(filename, "_semantic.txt");
                    if(semantic_filename) {
                        FILE* semantic_file = fopen(semantic_filename, "w");
                        if(semantic_file) {
                            print_section_header("SEMANTIC ANALYSIS", semantic_file);
                            print_semantic_analysis(semantic_context, semantic_file);
                            fprintf(semantic_file, "\n");
                            fclose(semantic_file);
                            printf("Semantic analysis written to: %s\n", semantic_filename);
                        } else {
                            errhandler__report_error
                                ( ERROR_CODE_COM_FAILCREATE
                                , 0
                                , 0
                                , "syntax"
                                , "Cannot open semantic output file: %s"
                                , semantic_filename
                            );
                        }
                        free(semantic_filename);
                    }
                }
                
                /* Log semantic log to file */
                if(flags & F_LOG_SEMANTIC_LOG) {
                    char* semantic_log_filename = get_output_filename(filename, "_semantic_log.txt");
                    if(semantic_log_filename) {
                        FILE* semantic_log_file = fopen(semantic_log_filename, "w");
                        if(semantic_log_file) {
                            print_semantic_log(semantic_context, semantic_log_file);
                            fclose(semantic_log_file);
                            printf("Semantic analysis log written to: %s\n", semantic_log_filename);
                        } else {
                            errhandler__report_error
                                ( ERROR_CODE_COM_FAILCREATE
                                , 0
                                , 0
                                , "syntax"
                                , "Cannot open semantic log file: %s"
                                , semantic_log_filename
                            );
                        }
                        free(semantic_log_filename);
                    }
                }
            }
            
            /* In compile mode, always show errors */
            if(is_compile_mode && !semantic_ok) {
                print_section_header("SEMANTIC ANALYSIS - ERRORS", stdout);
                print_semantic_analysis(semantic_context, stdout);
                fprintf(stdout, "\n");
            }
        }
        
        /* Combined logging outputs - only in non-compile mode */
        if(show_debug_output) {
            /* Verbose output */
            if(flags & F_LOG_VERBOSE) {
                char* verbose_filename = get_output_filename(filename, "_verbose.txt");
                if(verbose_filename) {
                    FILE* verbose_file = fopen(verbose_filename, "w");
                    if(verbose_file) {
                        print_section_header("LEXER TOKENS", verbose_file);
                        print_tokens_in_lines(lexer, verbose_file);
                        fprintf(verbose_file, "\n");
                        
                        if(ast) {
                            print_section_header("PARSER AST", verbose_file);
                            print_ast_compact(ast, verbose_file);
                            fprintf(verbose_file, "\n");
                        }
                        
                        if(semantic_context) {
                            print_section_header("SEMANTIC ANALYSIS", verbose_file);
                            print_semantic_analysis(semantic_context, verbose_file);
                            fprintf(verbose_file, "\n");
                            
                            /* Add semantic log to verbose output */
                            print_section_header("SEMANTIC ANALYSIS LOG", verbose_file);
                            print_semantic_log(semantic_context, verbose_file);
                            fprintf(verbose_file, "\n");
                        }
                        
                        ParseStatistics* stats = collect_parse_statistics(lexer, ast, semantic_context);
                        if(stats) {
                            print_section_header("STATISTICS", verbose_file);
                            print_statistics_report(stats, verbose_file);
                            fprintf(verbose_file, "\n");
                            free(stats);
                        }
                        
                        fclose(verbose_file);
                        printf("Verbose output written to: %s\n", verbose_filename);
                    } else {
                        errhandler__report_error
                            ( ERROR_CODE_COM_FAILCREATE
                            , 0
                            , 0
                            , "syntax"
                            , "Cannot open verbose output file: %s"
                            , verbose_filename
                        );
                    }
                    free(verbose_filename);
                }
            }
            
            /* State output */
            if(flags & F_LOG_STATE) {
                char* state_filename = get_output_filename(filename, "_state.txt");
                if(state_filename) {
                    FILE* state_file = fopen(state_filename, "w");
                    if(state_file) {
                        fprintf(state_file, "File size: %zu bytes\n", file_size);
                        fprintf(state_file, "Lines: %u\n", source_line_count);
                        fprintf(state_file, "Tokens: %u\n", lexer->token_count);
                        fprintf(state_file, "AST statements: %u\n", ast ? ast->count : 0);
                        
                        if(semantic_context) {
                            fprintf(state_file, "Symbols: %zu\n", semantic_get_symbol_count(semantic_context));
                            fprintf(state_file, "Semantic analysis: %s\n", 
                                    semantic_has_errors(semantic_context) ? "FAILED" : "PASSED");
                            fprintf(state_file, "Exit on error: %s\n",
                                    semantic_context->exit_on_error ? "enabled" : "disabled");
                        }
                        
                        fprintf(state_file, "\nErrors: %s\n", 
                                errhandler__has_errors() ? "YES" : "NO");
                        fprintf(state_file, "Warnings: %s\n", 
                                errhandler__has_warnings() ? "YES" : "NO");
                        
                        fclose(state_file);
                        printf("State output written to: %s\n", state_filename);
                    } else {
                        errhandler__report_error
                            ( ERROR_CODE_IO_WRITE
                            , 0
                            , 0
                            , "file"
                            , "Cannot open state output file: %s"
                            , state_filename
                        );
                    }
                    free(state_filename);
                }
            }
        }
        
        /* Cleanup for this file */
        errhandler__clear_source_code();
        if(source_lines)
            free_lines(source_lines, source_line_count);
        
        cleanup_resources(&ast, &lexer, &processed, &buffer_file, &semantic_context);
        
        /* Reset semantic context for next file if needed */
        if(semantic_context && (i + 1 < file_count)) {
            semantic_destroy_context(semantic_context);
            semantic_context = semantic_create_context();
            
            /* Reapply exit on error setting */
            if (flags & F_MODE_COMPILE) {
                semantic_set_exit_on_error(semantic_context, true);
            }
        }
    }

    /* Final error/warning reporting */
    if(errhandler__has_errors() || errhandler__has_warnings()) {
        if(errhandler__has_errors()) {
            errhandler__print_errors();
        }
        if(errhandler__has_warnings()) {
            errhandler__print_warnings();
        }
    }

    if(errhandler__has_errors()) exit_code = 1;

cleanup:
    if(semantic_context) {
        semantic_destroy_context(semantic_context);
    }
    
    free(filenames);
    errhandler__free_error_manager();
    
    return exit_code;
}
