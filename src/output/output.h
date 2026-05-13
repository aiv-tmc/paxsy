#ifndef OUTPUT_H
#define OUTPUT_H

#include "../lexer/lexer.h"
#include "../parser/parser.h"
#include "../semantic/semantic.h"
#include "../ir/ir.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PRINT_TOKENS_ONLY,
    PRINT_AST_ONLY,
    PRINT_ALL,
    PRINT_VERBOSE,
    PRINT_PARSER_TRACE,
    PRINT_SEMANTIC_ONLY,
    PRINT_SEMANTIC_FULL,
    PRINT_COMPLETE_ANALYSIS,
    PRINT_SEMANTIC_LOG,
    PRINT_IR,
    PRINT_IR_STATISTICS
} PrintMode;

typedef struct {
    uint32_t total_tokens;
    uint32_t total_nodes;
    uint32_t node_types[64];
    uint32_t token_types[256];
    uint32_t semantic_errors;
    uint32_t semantic_warnings;
    uint32_t symbols_count;
} ParseStatistics;

extern const char* token_names[];
extern const char* ast_node_names[];

#define TOKEN_TYPE_COUNT (TOKEN_ERRORCODE + 1)
#define AST_NODE_TYPE_COUNT (sizeof(ast_node_names) / sizeof(ast_node_names[0]))
#define IS_VALID_TOKEN_TYPE(t)    (((t) & (TOKEN_TYPE_COUNT - 1)) == (t))
#define IS_VALID_AST_NODE_TYPE(t) ((unsigned int)(t) < AST_NODE_TYPE_COUNT)

void print_section_header(const char* title, FILE* out);
void print_all_tokens(Lexer* lexer, FILE* out);
void print_tokens_in_lines(Lexer* lexer, FILE* out);
void print_token_statistics(Lexer* lexer, FILE* out);
void print_parser_trace(ParserState* parser, FILE* out);
void print_ast_detailed(AST* ast, FILE* out);
void print_ast_statistics(AST* ast, FILE* out);
void print_semantic_symbol_table(SemanticContext* context, FILE* out);
void print_semantic_type_info(SemanticContext* context, FILE* out);
void print_semantic_summary(SemanticContext* context, FILE* out);
void print_semantic_analysis(SemanticContext* context, FILE* out);
void print_semantic_log(SemanticContext* context, FILE* out);
const char* get_ast_node_type_name(ASTNodeType type);
const char* get_token_type_name(TokenType type);
void print_type_info(Type* type, FILE* out);
void print_complete_analysis(Lexer* lexer, AST* ast, SemanticContext* semantic, PrintMode mode, FILE* out);
ParseStatistics* collect_parse_statistics(Lexer* lexer, AST* ast, SemanticContext* semantic);
void print_statistics_report(ParseStatistics* stats, FILE* out);
void enable_parser_trace(bool enabled);
void log_parser_step(ParserState* parser, const char* action, ASTNode* node);
void print_optimized_ast(AST* ast, FILE* out);
void output__print_ir_module(IrModule* mod, FILE* out);
void output__print_ir_statistics(IrModule* mod, FILE* out);

#endif
