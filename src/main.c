#include "preprocessor/preprocessor.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "output/output.h"

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

#define HANDLE_FLAG(flag_name, short_name, has_value, action) \
    if(!strcmp(flag, flag_name) || !strcmp(flag, short_name)) { \
        if((has_value) ^ (value != NULL)) { \
            errman__report_error(0, 0, has_value ? \
                "Flag '%s' requires a value" : "Flag '%s' doesn't take a value", flag); \
            continue; \
        } \
        action; \
        continue; \
    }

static char* read_file_contents(const char* filename, size_t* file_size) {
    struct stat st;
    if(stat(filename, &st)) {
        errman__report_error(0, 0, "File '%s' does not exist or cannot be opened.", filename);
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

static char* get_output_filename(const char* input_filename, const char* suffix) {
    const char* start = input_filename;
    const char* p = start;
    
    while(*p) {
        if(*p == '/' || *p == '\\') start = p + 1;
        p++;
    }
    
    const char* dot = strrchr(start, '.');
    size_t name_len = dot ? (size_t)(dot - start) : (size_t)(p - start);
    size_t total_len = name_len + strlen(suffix) + 1;
    
    char* result = malloc(total_len);
    if(!result) return NULL;
    
    memcpy(result, start, name_len);
    strcpy(result + name_len, suffix);
    
    return result;
}

static void write_output_to_file(const char* filename, const char* content) {
    FILE* file = fopen(filename, "w");
    if(!file) {
        errman__report_error(0, 0, "Cannot open file for writing: %s", filename);
        return;
    }
    fputs(content, file);
    fclose(file);
}

static void cleanup_resources(AST** ast, Lexer** lexer, char** processed, char** buffer) {
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
}

static uint8_t validate_filename(const char* filename, char*** filenames, size_t* file_count, size_t* capacity) {
    size_t len = strlen(filename);
    if(len < 3 || strcmp(filename + len - 3, ".px")) {
        errman__report_error(0, 0, "File '%s' has invalid extension. Only .px files are supported.", filename);
        return 0;
    }
    
    for(size_t j = 0; j < *file_count; j++) {
        if(!strcmp((*filenames)[j], filename)) {
            errman__report_error(0, 0, "Duplicate file: %s", filename);
            return 0;
        }
    }
    
    if(*file_count >= *capacity) {
        *capacity += FILENAMES_BLOCK;
        char** new_filenames = realloc(*filenames, *capacity * sizeof(char*));
        if(!new_filenames) {
            errman__report_error(0, 0, "Memory allocation failed for filenames");
            return 0;
        }
        *filenames = new_filenames;
    }
    
    (*filenames)[(*file_count)++] = (char*)filename;
    return 1;
}

int main(int argc, char* argv[]) {
    uint8_t flags = 0;
    #define F_WRITE_LEXER   0x01
    #define F_WRITE_PARSER  0x02
    #define F_LOG_LEXER     0x04
    #define F_LOG_PARSER    0x08
    #define F_LOG_STATE     0x10
    #define F_LOG_VERBOSE   0x20
    #define F_MODE_COMPILE  0x40
    
    char** filenames = NULL;
    size_t file_count = 0;
    size_t capacity = 0;
    int exit_code = 0;
    uint8_t has_mode = 0;

    for(int i = 1; i < argc; i++) {
        if(argv[i][0] != '-') {
            validate_filename(argv[i], &filenames, &file_count, &capacity);
            continue;
        }

        char* flag = argv[i];
        char* value = strchr(flag, '=');
        if(value) {
            *value = '\0';
            value++;
        }

        HANDLE_FLAG("--help", "-h", 0, {
            printf("\033[93mUSAGE:\033[0m paxsy \033[1m[operations] <source>\033[0m ...\n"
                   "operations:\n"
                   "  \033[1m -h  --help\033[0m\t\t\t\tDisplay this information\n"
                   "  \033[1m -v  --version\033[0m\t\t\tDisplay compiler version information\n"
                   "  \033[1m -wl --write-lexer\033[0m <source>\t\tDisplay lexer output only\n"
                   "  \033[1m -wp --write-parser\033[0m <source>\t\tDisplay parser output only\n"
                   "  \033[1m -l  --log\033[0m <source>\t\t\tWrite all outputs to files\n"
                   "  \033[1m -ll --log-lexer\033[0m <source>\t\tWrite lexer output to file\n"
                   "  \033[1m -lp --log-parser\033[0m <source>\t\tWrite parser output to file\n"
                   "  \033[1m -lst --log-state\033[0m <source>\t\tWrite state output to file\n"
                   "  \033[1m -lv --log-verbose\033[0m <source>\t\tWrite verbose output to file\n"
                   "  \033[1m -c  --compile\033[0m <source>\t\tCompile and assemble\n");
            exit_code = 0;
            goto cleanup;
        })
        
        HANDLE_FLAG("--version", "-v", 0, {
            printf("paxsy %s %s\n"
                   "\033[1m%s\033[0m - \033[1m%s\033[0m\n"
                   "\n"
                   "This is being developed by AIV\n"
                   "This free software is distributed under the MIT General Public License\n", 
                   GENERATION, NAME, VERSION, DATE);
            exit_code = 0;
            goto cleanup;
        })
        
        HANDLE_FLAG("--write-lexer", "-wl", 0, flags |= F_WRITE_LEXER)
        HANDLE_FLAG("--write-parser", "-wp", 0, flags |= F_WRITE_PARSER)
        HANDLE_FLAG("--write", "-w", 0, flags |= (F_WRITE_LEXER | F_WRITE_PARSER))
        HANDLE_FLAG("--log", "-l", 0, flags |= (F_LOG_LEXER | F_LOG_PARSER | F_LOG_STATE | F_LOG_VERBOSE))
        HANDLE_FLAG("--log-lexer", "-ll", 0, flags |= F_LOG_LEXER)
        HANDLE_FLAG("--log-parser", "-lp", 0, flags |= F_LOG_PARSER)
        HANDLE_FLAG("--log-state", "-lst", 0, flags |= F_LOG_STATE)
        HANDLE_FLAG("--log-verbose", "-lv", 0, flags |= F_LOG_VERBOSE)
        HANDLE_FLAG("--compile", "-c", 0, {
            if(has_mode) {
                errman__report_error(0, 0, "Multiple mode flags specified");
            } else {
                flags |= F_MODE_COMPILE;
                has_mode = 1;
            }
        })
        
        errman__report_error(0, 0, "unknown flag: %s", flag);
    }

    uint8_t has_write = flags & (F_WRITE_LEXER | F_WRITE_PARSER);
    uint8_t has_log = flags & (F_LOG_LEXER | F_LOG_PARSER | F_LOG_STATE | F_LOG_VERBOSE);
    uint8_t has_operation = flags & (F_MODE_COMPILE | has_write | has_log);

    if(!file_count && has_operation) {
        errman__report_error(0, 0, "no input file specified");
    }
    
    if(!has_operation && file_count) {
        errman__report_error(0, 0, "Files can only be processed with -c, -wl, -wp, -w, -l, -ll, -lp, or -lv flags");
    }

    if(errman__has_errors()) {
        printf("\033[93mATTENTION:\033[0m \033[4mcompilation messages:\033[0m\n");
        errman__print_errors();
        errman__print_warnings();
        exit_code = 1;
        goto cleanup;
    }

    for(size_t i = 0; i < file_count; i++) {
        char* filename = filenames[i];
        size_t file_size;
        char* buffer_file = NULL;
        Lexer* lexer = NULL;
        AST* ast = NULL;
        char* processed = NULL;

        buffer_file = read_file_contents(filename, &file_size);
        if(!buffer_file) {
            errman__report_error(0, 0, "Cannot open file: %s", filename);
            continue;
        }
        
        processed = preprocess(buffer_file, filename, NULL);
        if(!processed) {
            errman__report_error(0, 0, "Preprocessing failed for file: %s", filename);
            free(buffer_file);
            continue;
        }
        
        lexer = lexer__init_lexer(processed);
        if(!lexer) {
            free(processed);
            free(buffer_file);
            continue;
        }
        
        lexer__tokenize(lexer);

        if(flags & F_WRITE_LEXER) {
            print_tokens_in_lines(lexer, stdout);
        }
        
        if(flags & F_LOG_LEXER) {
            char* lexer_filename = get_output_filename(filename, "_lexer.txt");
            if(lexer_filename) {
                FILE* lexer_file = fopen(lexer_filename, "w");
                if(lexer_file) {
                    print_tokens_in_lines(lexer, lexer_file);
                    fclose(lexer_file);
                } else {
                    errman__report_error(0, 0, "Cannot open lexer output file: %s", lexer_filename);
                }
                free(lexer_filename);
            }
        }
        
        if(!errman__has_errors()) {
            ast = parse(lexer->tokens, lexer->token_count);
        }
        
        if((flags & F_WRITE_PARSER) && !errman__has_errors() && ast) {
            printf("\033[93mATTENTION:\033[0m \033[4mabstract syntax tree for %s:\033[0m\n", filename);
            print_ast_detailed(ast, stdout);
        }
        
        if((flags & F_LOG_PARSER) && !errman__has_errors() && ast) {
            char* parser_filename = get_output_filename(filename, "_parser.txt");
            if(parser_filename) {
                FILE* parser_file = fopen(parser_filename, "w");
                if(parser_file) {
                    fprintf(parser_file, "\033[93mATTENTION:\033[0m \033[4mabstract syntax tree:\033[0m\n");
                    print_ast_detailed(ast, parser_file);
                    fclose(parser_file);
                } else {
                    errman__report_error(0, 0, "Cannot open parser output file: %s", parser_filename);
                }
                free(parser_filename);
            }
        }
        
        cleanup_resources(&ast, &lexer, &processed, &buffer_file);
    }

    if(errman__has_errors() || errman__has_warnings()) {
        printf("\033[93mATTENTION:\033[0m \033[4mcompilation messages:\033[0m\n");
        if(errman__has_errors()) errman__print_errors();
        if(errman__has_warnings()) errman__print_warnings();
    }

    if(errman__has_errors()) exit_code = 1;

cleanup:
    free(filenames);
    errman__free_error_manager();
    return exit_code;
}
