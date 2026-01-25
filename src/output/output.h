#ifndef OUTPUT_H
#define OUTPUT_H

#include "../lexer/lexer.h"
#include "../parser/parser.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PRINT_TOKENS_ONLY,
    PRINT_AST_ONLY,
    PRINT_ALL,
    PRINT_VERBOSE,
    PRINT_PARSER_TRACE
} PrintMode;

typedef struct {
    uint32_t total_tokens;
    uint32_t total_nodes;
    uint32_t node_types[64];
    uint32_t token_types[256];
} ParseStatistics;

extern const char* token_names[];
extern const char* ast_node_names[];

#define TOKEN_TYPE_COUNT (TOKEN_ERROR + 1)
#define AST_NODE_TYPE_COUNT (AST_TERNARY_OPERATION + 1)

// Token output functions
void print_all_tokens(Lexer* lexer, FILE* out);
void print_tokens_by_line(Lexer* lexer, FILE* out);
void print_token_statistics(Lexer* lexer, FILE* out);
void print_detailed_token_info(Lexer* lexer, FILE* out);
void print_tokens_in_lines(Lexer* lexer, FILE* out);
void print_parser_trace(ParserState* parser, FILE* out);

// AST output functions
void print_ast_detailed(AST* ast, FILE* out);
void print_ast_by_type(AST* ast, FILE* out);
void print_ast_statistics(AST* ast, FILE* out);
void print_ast_with_types(AST* ast, FILE* out);
void print_ast_compact(AST* ast, FILE* out);

// Utility functions
const char* get_ast_node_type_name(ASTNodeType type);
const char* get_token_type_name(TokenType type);
void print_ast_node_inline(ASTNode* node, FILE* out);
void print_type_info(Type* type, FILE* out);

// Combined analysis
void print_complete_analysis(Lexer* lexer, AST* ast, PrintMode mode, FILE* out);
void print_parse_summary(Lexer* lexer, AST* ast, FILE* out);
ParseStatistics* collect_parse_statistics(Lexer* lexer, AST* ast);
void print_statistics_report(ParseStatistics* stats, FILE* out);

// Parser trace
void enable_parser_trace(bool enabled);
void log_parser_step(ParserState* parser, const char* action, ASTNode* node);

#endif
