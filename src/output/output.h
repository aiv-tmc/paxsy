#ifndef OUTPUT_H
#define OUTPUT_H

#include "../lexer/lexer.h"
#include "../parser/parser.h"
#include "../semantic/semantic.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Output verbosity and content selection modes.
 */
typedef enum {
    PRINT_TOKENS_ONLY,       ///< Print only lexer tokens.
    PRINT_AST_ONLY,          ///< Print only AST.
    PRINT_ALL,               ///< Print tokens, AST, and semantic analysis.
    PRINT_VERBOSE,           ///< Print detailed information for all phases.
    PRINT_PARSER_TRACE,      ///< Print parser trace (requires parser state).
    PRINT_SEMANTIC_ONLY,     ///< Print only semantic analysis results.
    PRINT_SEMANTIC_FULL,     ///< Print full semantic details (symbols, types, summary).
    PRINT_COMPLETE_ANALYSIS, ///< Print tokens, AST, semantic, and statistics.
    PRINT_SEMANTIC_LOG       ///< Print semantic analysis log.
} PrintMode;

/**
 * @brief Compilation statistics collected from lexer, parser, and semantic phases.
 */
typedef struct {
    uint32_t total_tokens;       ///< Total number of tokens.
    uint32_t total_nodes;        ///< Total number of AST nodes.
    uint32_t node_types[64];     ///< Count per AST node type.
    uint32_t token_types[256];   ///< Count per token type.
    uint32_t semantic_errors;    ///< Number of semantic errors detected.
    uint32_t semantic_warnings;  ///< Number of semantic warnings issued.
    uint32_t symbols_count;      ///< Number of symbols in the symbol table.
} ParseStatistics;

extern const char* token_names[];
extern const char* ast_node_names[];

#define TOKEN_TYPE_COUNT (TOKEN_ERROR + 1)
#define AST_NODE_TYPE_COUNT (AST_TERNARY_OPERATION + 1)

/**
 * @brief Print a formatted section header.
 * @param title Header title.
 * @param out   Output file stream.
 */
void print_section_header(const char* title, FILE* out);

/**
 * @brief Print all tokens with index, type, value, and location.
 * @param lexer Lexer containing the token list.
 * @param out   Output file stream.
 */
void print_all_tokens(Lexer* lexer, FILE* out);

/**
 * @brief Print tokens grouped by source line.
 * @param lexer Lexer containing the token list.
 * @param out   Output file stream.
 */
void print_tokens_by_line(Lexer* lexer, FILE* out);

/**
 * @brief Print token type statistics.
 * @param lexer Lexer containing the token list.
 * @param out   Output file stream.
 */
void print_token_statistics(Lexer* lexer, FILE* out);

/**
 * @brief Print detailed information for each token.
 * @param lexer Lexer containing the token list.
 * @param out   Output file stream.
 */
void print_detailed_token_info(Lexer* lexer, FILE* out);

/**
 * @brief Print tokens with line markers, showing tokens per line.
 * @param lexer Lexer containing the token list.
 * @param out   Output file stream.
 */
void print_tokens_in_lines(Lexer* lexer, FILE* out);

/**
 * @brief Print current parser state information.
 * @param parser Parser state.
 * @param out    Output file stream.
 */
void print_parser_trace(ParserState* parser, FILE* out);

/**
 * @brief Print AST in a detailed tree format with indentation.
 * @param ast AST to print.
 * @param out Output file stream.
 */
void print_ast_detailed(AST* ast, FILE* out);

/**
 * @brief Print AST node type distribution.
 * @param ast AST to analyze.
 * @param out Output file stream.
 */
void print_ast_by_type(AST* ast, FILE* out);

/**
 * @brief Print AST statistics (node counts per type, total nodes).
 * @param ast AST to analyze.
 * @param out Output file stream.
 */
void print_ast_statistics(AST* ast, FILE* out);

/**
 * @brief Print AST with type information attached to nodes.
 * @param ast AST to print.
 * @param out Output file stream.
 */
void print_ast_with_types(AST* ast, FILE* out);

/**
 * @brief Print AST in compact form (one line per statement).
 * @param ast AST to print.
 * @param out Output file stream.
 */
void print_ast_compact(AST* ast, FILE* out);

/**
 * @brief Print the symbol table from the semantic context.
 * @param context Semantic context.
 * @param out     Output file stream.
 */
void print_semantic_symbol_table(SemanticContext* context, FILE* out);

/**
 * @brief Print type distribution and initialization state from semantic analysis.
 * @param context Semantic context.
 * @param out     Output file stream.
 */
void print_semantic_type_info(SemanticContext* context, FILE* out);

/**
 * @brief Print a summary of semantic errors and warnings.
 * @param context Semantic context.
 * @param out     Output file stream.
 */
void print_semantic_errors_warnings(SemanticContext* context, FILE* out);

/**
 * @brief Print a concise summary of the semantic analysis.
 * @param context Semantic context.
 * @param out     Output file stream.
 */
void print_semantic_summary(SemanticContext* context, FILE* out);

/**
 * @brief Print complete semantic analysis: summary, symbol table, type info.
 * @param context Semantic context.
 * @param out     Output file stream.
 */
void print_semantic_analysis(SemanticContext* context, FILE* out);

/**
 * @brief Print a detailed semantic log (summary, symbols, types, scope info).
 * @param context Semantic context.
 * @param out     Output file stream.
 */
void print_semantic_log(SemanticContext* context, FILE* out);

/**
 * @brief Convert an AST node type to its string representation.
 * @param type AST node type.
 * @return String describing the node type.
 */
const char* get_ast_node_type_name(ASTNodeType type);

/**
 * @brief Convert a token type to its string representation.
 * @param type Token type.
 * @return String describing the token type.
 */
const char* get_token_type_name(TokenType type);

/**
 * @brief Print an AST node in a compact inline form (no newline).
 * @param node AST node.
 * @param out  Output file stream.
 */
void print_ast_node_inline(ASTNode* node, FILE* out);

/**
 * @brief Print type information in a human-readable format.
 * @param type Type structure.
 * @param out  Output file stream.
 */
void print_type_info(Type* type, FILE* out);

/**
 * @brief Print a complete analysis according to the selected mode.
 * @param lexer    Lexer data.
 * @param ast      AST data.
 * @param semantic Semantic context (may be NULL).
 * @param mode     Print mode selector.
 * @param out      Output file stream.
 */
void print_complete_analysis(Lexer* lexer, AST* ast,
                             SemanticContext* semantic, PrintMode mode, FILE* out);

/**
 * @brief Print a short summary of the parsing process.
 * @param lexer    Lexer data.
 * @param ast      AST data.
 * @param semantic Semantic context (may be NULL).
 * @param out      Output file stream.
 */
void print_parse_summary(Lexer* lexer, AST* ast,
                         SemanticContext* semantic, FILE* out);

/**
 * @brief Collect compilation statistics from all phases.
 * @param lexer    Lexer data.
 * @param ast      AST data.
 * @param semantic Semantic context (may be NULL).
 * @return Pointer to a newly allocated ParseStatistics structure (caller must free).
 */
ParseStatistics* collect_parse_statistics(Lexer* lexer, AST* ast,
                                          SemanticContext* semantic);

/**
 * @brief Print a formatted statistics report.
 * @param stats Statistics to print.
 * @param out   Output file stream.
 */
void print_statistics_report(ParseStatistics* stats, FILE* out);

/**
 * @brief Enable or disable parser trace logging.
 * @param enabled true to enable, false to disable.
 */
void enable_parser_trace(bool enabled);

/**
 * @brief Log a parser step when trace is enabled.
 * @param parser Parser state.
 * @param action Description of the current action.
 * @param node   Current AST node being processed (may be NULL).
 */
void log_parser_step(ParserState* parser, const char* action, ASTNode* node);

#endif
