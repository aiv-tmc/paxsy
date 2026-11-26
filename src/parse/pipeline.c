#define _POSIX_C_SOURCE 200809L
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

#include <pthread.h>
#include <time.h>

/*
 * pipeline.c - orchestrates one file through every frontend phase and
 * normalizes the failure protocol for all commands that analyse code.
 * Internal inspection modes can additionally lower the typed tree into
 * the high-level IR or print the GPU offload classification. Lowering
 * and typing errors join the same diagnostics stream and fail the file
 * like any other phase. Before lowering runs, the file's package
 * qualifier is announced so modules declared in a library source are
 * named under their package (std::inout, not inout).
 *
 * Literal text ownership: the lexer records literal bodies as slices
 * into the source buffer and the parser interns identifier names.
 * Token slices stay valid into lowering and emission, so instead of
 * copying selected literal kinds the whole source buffer is handed to
 * the Analysis and released by analysis_release() once the backend
 * is done with the tree.
 */

static double pipeline_elapsed(struct timespec begin, struct timespec end) {
    return (double)(end.tv_sec - begin.tv_sec) +
           (double)(end.tv_nsec - begin.tv_nsec) / 1000000000.0;
}

static int pipeline_run(const char *path, const Cli *cli, Diag *diag,
                        Analysis *out, int run_sema) {
    Source src;
    struct timespec phase_begin, phase_end;
    memset(out, 0, sizeof(*out));
    if (cli->profile) clock_gettime(CLOCK_MONOTONIC, &phase_begin);
    if (source_load(&src, path) != 0) {
        diag_report(diag, DK_ERROR, path, 1, 1, "cannot open source file");
        return 1;
    }
    if (cli->profile) {
        clock_gettime(CLOCK_MONOTONIC, &phase_end);
        out->source_seconds = pipeline_elapsed(phase_begin, phase_end);
        clock_gettime(CLOCK_MONOTONIC, &phase_begin);
    }
    lexer_run(&src, &out->tokens, diag);
    if (cli->profile) {
        clock_gettime(CLOCK_MONOTONIC, &phase_end);
        out->lex_seconds = pipeline_elapsed(phase_begin, phase_end);
    }
    if (cli->dump_tokens) dump_tokens(&out->tokens, stdout);
    if (diag_error_count(diag) == 0) {
        if (cli->profile) clock_gettime(CLOCK_MONOTONIC, &phase_begin);
        out->ast = parser_run(&out->tokens, diag, path);
        if (cli->profile) {
            clock_gettime(CLOCK_MONOTONIC, &phase_end);
            out->parse_seconds = pipeline_elapsed(phase_begin, phase_end);
        }
        if (run_sema && out->ast && diag_error_count(diag) == 0) {
            if (cli->profile) clock_gettime(CLOCK_MONOTONIC, &phase_begin);
            sema_run(out->ast, diag, path);
            if (cli->profile) {
                clock_gettime(CLOCK_MONOTONIC, &phase_end);
                out->sema_seconds = pipeline_elapsed(phase_begin, phase_end);
            }
        }
    }
    if (out->ast) out->nodes = pipeline_count_nodes(out->ast);
    if (out->ast && cli->dump_ast) dump_ast(out->ast, stdout, 0);
    if (run_sema && out->ast && cli->dump_hir && diag_error_count(diag) == 0) {
        stdpath_set_pkg_prefix(path);
        HModule *hir = lower_run(out->ast, diag);
        if (hir) {
            HModule *only[] = { hir };
            if (diag_error_count(diag) == 0)
                hir_tyinfer(only, 1, diag);
            hir_dump(hir, stdout);
            hir_module_free(hir);
        }
    }
    /* Concurrent builds run one pipeline per file; the excerpt
     * printer writes several interleaved fprintf calls per diagnostic,
     * so the whole block is serialized to keep multi-file output
     * readable. Uncontended in the sequential path. */
    static pthread_mutex_t print_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&print_mu);
    diag_print_all(diag, &src);
    pthread_mutex_unlock(&print_mu);
    /* The backend reads token slices long after this function
     * returns: detach the buffer from the Source and let the
     * Analysis own it until analysis_release(); source_free then
     * releases only the line index. */
    out->text = src.text;
    src.text = NULL;
    source_free(&src);
    return diag_error_count(diag) ? 1 : 0;
}

int pipeline_analyze(const char *path, const Cli *cli, Diag *diag,
                     Analysis *out) {
    return pipeline_run(path, cli, diag, out, 1);
}

int pipeline_parse(const char *path, const Cli *cli, Diag *diag,
                   Analysis *out) {
    return pipeline_run(path, cli, diag, out, 0);
}

void analysis_release(Analysis *a) {
    token_vec_free(&a->tokens);
    free(a->text);
    memset(a, 0, sizeof(*a));
}

long pipeline_count_nodes(const Node *n) {
    if (!n) return 0;
    long c = 1;
    for (int i = 0; i < 4; i++) c += pipeline_count_nodes(n->child[i]);
    for (int i = 0; i < n->item_count; i++) c += pipeline_count_nodes(n->items[i]);
    return c;
}
