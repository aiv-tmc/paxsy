#ifndef OUTPUT_H
#define OUTPUT_H

#include "../lexer/lexer.h"
#include "../parser/parser.h"
#include "../semantic/semantic.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Print modes for controlling output verbosity and content
 */
typedef enum {
    PRINT_TOKENS_ONLY,       ///< Display only token information
    PRINT_AST_ONLY,          ///< Display only AST information
    PRINT_ALL,               ///< Display all information (tokens, AST, semantic)
    PRINT_VERBOSE,           ///< Display verbose detailed information
    PRINT_PARSER_TRACE,      ///< Display parser trace information
    PRINT_SEMANTIC_ONLY,     ///< Display only semantic analysis results
    PRINT_SEMANTIC_FULL,     ///< Display full semantic analysis details
    PRINT_COMPLETE_ANALYSIS  ///< Display complete analysis with statistics
} PrintMode;

/*
 * Parse statistics structure for collecting compilation metrics
 */
typedef struct {
    uint32_t total_tokens;       ///< Total number of tokens
    uint32_t total_nodes;        ///< Total number of AST nodes
    uint32_t node_types[64];     ///< Count of each AST node type
    uint32_t token_types[256];   ///< Count of each token type
    uint32_t semantic_errors;    ///< Number of semantic errors
    uint32_t semantic_warnings;  ///< Number of semantic warnings
    uint32_t symbols_count;      ///< Number of symbols in symbol table
} ParseStatistics;

extern const char* token_names[];
extern const char* ast_node_names[];

#define TOKEN_TYPE_COUNT (TOKEN_ERROR + 1)
#define AST_NODE_TYPE_COUNT (AST_TERNARY_OPERATION + 1)

/*
 * Print section header with title
 * @param title Section title
 * @param out Output file stream
 */
void print_section_header(const char* title, FILE* out);

/*
 * Print all tokens from lexer
 * @param lexer Lexer containing tokens
 * @param out Output file stream
 */
void print_all_tokens(Lexer* lexer, FILE* out);

/*
 * Print tokens grouped by line
 * @param lexer Lexer containing tokens
 * @param out Output file stream
 */
void print_tokens_by_line(Lexer* lexer, FILE* out);

/*
 * Print token statistics
 * @param lexer Lexer containing tokens
 * @param out Output file stream
 */
void print_token_statistics(Lexer* lexer, FILE* out);

/*
 * Print detailed token information
 * @param lexer Lexer containing tokens
 * @param out Output file stream
 */
void print_detailed_token_info(Lexer* lexer, FILE* out);

/*
 * Print tokens with line markers
 * @param lexer Lexer containing tokens
 * @param out Output file stream
 */
void print_tokens_in_lines(Lexer* lexer, FILE* out);

/*
 * Print parser trace information
 * @param parser Parser state to trace
 * @param out Output file stream
 */
void print_parser_trace(ParserState* parser, FILE* out);

/*
 * Print AST in detailed tree format
 * @param ast AST to print
 * @param out Output file stream
 */
void print_ast_detailed(AST* ast, FILE* out);

/*
 * Print AST nodes grouped by type
 * @param ast AST to analyze
 * @param out Output file stream
 */
void print_ast_by_type(AST* ast, FILE* out);

/*
 * Print AST statistics
 * @param ast AST to analyze
 * @param out Output file stream
 */
void print_ast_statistics(AST* ast, FILE* out);

/*
 * Print AST with type information
 * @param ast AST to print
 * @param out Output file stream
 */
void print_ast_with_types(AST* ast, FILE* out);

/*
 * Print AST in compact format
 * @param ast AST to print
 * @param out Output file stream
 */
void print_ast_compact(AST* ast, FILE* out);

/*
 * Print symbol table from semantic context
 * @param context Semantic context containing symbol table
 * @param out Output file stream
 */
void print_semantic_symbol_table(SemanticContext* context, FILE* out);

/*
 * Print type information from semantic context
 * @param context Semantic context containing type information
 * @param out Output file stream
 */
void print_semantic_type_info(SemanticContext* context, FILE* out);

/*
 * Print semantic analysis errors and warnings
 * @param out Output file stream
 */
void print_semantic_errors_warnings(FILE* out);

/*
 * Print semantic analysis summary
 * @param context Semantic context to summarize
 * @param out Output file stream
 */
void print_semantic_summary(SemanticContext* context, FILE* out);

/*
 * Print complete semantic analysis
 * @param context Semantic context to analyze
 * @param out Output file stream
 */
void print_semantic_analysis(SemanticContext* context, FILE* out);

/*
 * Get AST node type name as string
 * @param type AST node type
 * @return String representation of node type
 */
const char* get_ast_node_type_name(ASTNodeType type);

/*
 * Get token type name as string
 * @param type Token type
 * @return String representation of token type
 */
const char* get_token_type_name(TokenType type);

/*
 * Print AST node inline (without newline)
 * @param node AST node to print
 * @param out Output file stream
 */
void print_ast_node_inline(ASTNode* node, FILE* out);

/*
 * Print type information
 * @param type Type structure to print
 * @param out Output file stream
 */
void print_type_info(Type* type, FILE* out);

/*
 * Print complete analysis with specified mode
 * @param lexer Lexer data
 * @param ast AST data
 * @param semantic Semantic context (can be NULL)
 * @param mode Print mode
 * @param out Output file stream
 */
void print_complete_analysis(Lexer* lexer, AST* ast, 
                            SemanticContext* semantic, PrintMode mode, FILE* out);

/*
 * Print parse summary
 * @param lexer Lexer data
 * @param ast AST data
 * @param semantic Semantic context (can be NULL)
 * @param out Output file stream
 */
void print_parse_summary(Lexer* lexer, AST* ast, 
                        SemanticContext* semantic, FILE* out);

/*
 * Collect parse statistics
 * @param lexer Lexer data
 * @param ast AST data
 * @param semantic Semantic context (can be NULL)
 * @return ParseStatistics structure with collected data
 */
ParseStatistics* collect_parse_statistics(Lexer* lexer, AST* ast,
                                         SemanticContext* semantic);

/*
 * Print statistics report
 * @param stats Statistics to print
 * @param out Output file stream
 */
void print_statistics_report(ParseStatistics* stats, FILE* out);

/*
 * Enable or disable parser trace
 * @param enabled True to enable, false to disable
 */
void enable_parser_trace(bool enabled);

/*
 * Log parser step for debugging
 * @param parser Parser state
 * @param action Description of current action
 * @param node Current AST node being processed
 */
void log_parser_step(ParserState* parser, const char* action, ASTNode* node);

#endif
