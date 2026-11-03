#include "common.h"
#include "pipeline.h"
#include "source.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "dump.h"
#include "hir.h"
#include "lower.h"
#include "tyinfer.h"
#include "offload.h"
#include "stdpath.h"

/*
 * pipeline.c - orchestrates one file through every frontend phase and
 * normalizes the failure protocol for all commands that analyse code.
 * With --hir the pipeline additionally lowers the typed tree into the
 * high-level IR, types it, and prints it; --dump-offload runs the same
 * lowering and prints the GPU offload classification instead. Lowering
 * and typing errors join the same diagnostics stream and fail the file
 * like any other phase. Before lowering runs, the file's package
 * qualifier is announced so modules declared in a library source are
 * named under their package (std::inout, not inout).
 *
 * Literal text ownership: the lexer records string, character and
 * inline-asm bodies as slices into the source buffer. The source
 * buffer is released when this function returns, while the backend
 * runs much later, so every such slice is copied to owned memory
 * before the release and the token pointers are repointed at the
 * copies. Identifier names are interned by the parser and are not
 * affected.
 */

/* Copies one token's slice to owned memory and repoints the token at
 * the copy. */
static void own_token_text(Token *t) {
    char *buf = malloc((size_t)t->len + 1);
    memcpy(buf, t->start, (size_t)t->len);
    buf[t->len] = '\0';
    t->start = buf;
}

/* Walks the tree and takes ownership of every literal body that the
 * backend reads by slice: string and character literals and inline
 * asm statements. */
static void own_literal_text(Node *n) {
    if (!n) return;
    if (n->kind == NK_LITERAL &&
        (n->tok.kind == TK_STRING_LIT || n->tok.kind == TK_CHAR_LIT))
        own_token_text(&n->tok);
    if (n->kind == NK_ASM)
        own_token_text(&n->tok);
    for (int i = 0; i < 4; i++) own_literal_text(n->child[i]);
    for (int i = 0; i < n->item_count; i++) own_literal_text(n->items[i]);
}

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
    if (out->ast && (cli->dump_hir || cli->dump_offload) &&
        diag_error_count(diag) == 0) {
        stdpath_set_pkg_prefix(path);
        HModule *hir = lower_run(out->ast, diag);
        if (hir) {
            if (diag_error_count(diag) == 0)
                hir_tyinfer(hir, diag);
            if (cli->dump_offload && diag_error_count(diag) == 0)
                offload_report(&hir, 1, stdout);
            if (cli->dump_hir && diag_error_count(diag) == 0)
                hir_dump(hir, stdout);
            hir_module_free(hir);
        }
    }
    /* The backend reads literal bodies long after this function
     * returns, so the slices must survive the source buffer. */
    if (out->ast) own_literal_text(out->ast);
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
