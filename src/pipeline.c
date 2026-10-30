#include "common.h"
#include "pipeline.h"
#include "source.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "dump.h"

/*
 * pipeline.c - orchestrates one file through every frontend phase and
 * normalizes the failure protocol for all commands that analyse code.
 */

int pipeline_analyze(const char *path, const Cli *cli, Diag *diag, Analysis *out) {
    Source src;
    memset(out, 0, sizeof(*out));
    if (source_load(&src, path) != 0) {
        diag_report(diag, DK_ERROR, path, 1, 1, "cannot open source file");
        return 1;
    }
    lexer_run(&src, &out->tokens, diag);
    if (cli->dump_tokens) dump_tokens(&out->tokens, stdout);
    if (diag_error_count(diag) == 0) {
        out->ast = parser_run(&out->tokens, diag, path);
        if (out->ast && diag_error_count(diag) == 0)
            sema_run(out->ast, diag, path);
    }
    if (out->ast) out->nodes = pipeline_count_nodes(out->ast);
    if (out->ast && cli->dump_ast) dump_ast(out->ast, stdout, 0);
    diag_print_all(diag, &src);
    source_free(&src);
    return diag_error_count(diag) ? 1 : 0;
}

void analysis_release(Analysis *a) {
    token_vec_free(&a->tokens);
    memset(a, 0, sizeof(*a));
}

long pipeline_count_nodes(const Node *n) {
    if (!n) return 0;
    long c = 1;
    for (int i = 0; i < 4; i++) c += pipeline_count_nodes(n->child[i]);
    for (int i = 0; i < n->item_count; i++) c += pipeline_count_nodes(n->items[i]);
    return c;
}
