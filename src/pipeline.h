#ifndef PAXSY_PIPELINE_H
#define PAXSY_PIPELINE_H

/*
 * pipeline.h - the shared per-file analysis driver used by build,
 * check, test and the artifact commands.
 * pipeline_analyze() loads a source file, lexes it, parses it and
 * runs semantic analysis, honouring the dump flags. All diagnostics
 * are stored in the caller-provided Diag and printed together with
 * source excerpts before returning. The return value is 1 when the
 * file failed (lex/parse/sema errors) and 0 when it is clean.
 */

#include "ast.h"
#include "diag.h"
#include "lexer.h"
#include "cli.h"

typedef struct {
    Node *ast;       /* parsed program, NULL when lexing failed */
    TokenVec tokens; /* token stream, owned by this analysis */
    long nodes;      /* total number of AST nodes */
} Analysis;

int pipeline_analyze(const char *path, const Cli *cli, Diag *diag, Analysis *out);
void analysis_release(Analysis *a);
long pipeline_count_nodes(const Node *n);

#endif /* PAXSY_PIPELINE_H */
