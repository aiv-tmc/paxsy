#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "pipeline.h"
#include "node.h"
#include "sema.h"
#include "lower.h"
#include "hir.h"
#include "tyinfer.h"
#include "stdpath.h"

#include <stdlib.h>

/* Module declarations are project-wide, ordinary declarations are not.
 * Each source gets its own semantic translation unit, preloaded with the
 * project's module declarations. This preserves cross-file `use module`
 * while preventing an unrelated top-level function/constant/type in one
 * file from silently becoming visible in every other file. The standard
 * library remains isolated per file because its sibling imports have
 * package-local meaning. */
int paxsy_sema_project(Analysis *analyses, int count, Diag *diag) {
    if (count <= 0) return 0;
    Node **modules = NULL;
    int nmodules = 0, cap = 0, errors = 0;

    for (int i = 0; i < count; i++) {
        Node *ast = analyses[i].ast;
        const char *path = ast && ast->tok.path ? ast->tok.path : "<source>";
        char package[80];
        if (!ast || std_pkg_prefix(path, package, sizeof(package)) == 0)
            continue;
        for (int j = 0; j < ast->item_count; j++) {
            Node *decl = ast->items[j];
            if (decl->kind != NK_MODULE) continue;
            if (nmodules == cap) {
                int next = cap ? cap * 2 : 16;
                Node **grown = realloc(modules, (size_t)next * sizeof(*grown));
                if (!grown) { free(modules); return errors + 1; }
                modules = grown;
                cap = next;
            }
            modules[nmodules++] = decl;
        }
    }

    for (int i = 0; i < count; i++) {
        Node *ast = analyses[i].ast;
        const char *path = ast && ast->tok.path ? ast->tok.path : "<source>";
        char package[80];
        if (!ast) continue;
        if (std_pkg_prefix(path, package, sizeof(package)) == 0) {
            errors += sema_run(ast, diag, path);
            continue;
        }

        Node *unit = node_new(NK_PROGRAM, ast->tok);
        if (!unit) { errors++; continue; }
        for (int j = 0; j < nmodules; j++) node_add(unit, modules[j]);
        for (int j = 0; j < ast->item_count; j++)
            if (ast->items[j]->kind != NK_MODULE)
                node_add(unit, ast->items[j]);
        errors += sema_run(unit, diag, path);
        free(unit->items);
        free(unit);
    }
    free(modules);
    return errors;
}

/* `check` performs the same lowering and whole-program type analysis
 * as `build`, but never invokes an emitter or writes artifacts. Keeping
 * each Analysis alive until type inference finishes preserves token text
 * and source-path metadata referenced by HIR diagnostics. */
int check_types(const Cli *cli, const SrcList *srcs) {
    Diag typed;
    diag_init(&typed, cli->max_errors);
    typed.suppress_warnings = cli->ignore_warnings;
    Analysis *analyses = calloc((size_t)srcs->count, sizeof(*analyses));
    HModule **mods = calloc((size_t)srcs->count, sizeof(*mods));
    int count = 0, failed = 0;
    if (!analyses || !mods) {
        free(analyses);
        free(mods);
        diag_free(&typed);
        return -1;
    }
    for (int i = 0; i < srcs->count; i++) {
        Diag front;
        diag_init(&front, cli->max_errors);
        front.suppress_warnings = cli->ignore_warnings;
        int rc = pipeline_parse(srcs->paths[i], cli, &front, &analyses[i]);
        if (cli->werror && diag_warning_count(&front)) rc = 1;
        diag_free(&front);
        if (rc) { failed = 1; break; }
    }
    if (!failed && paxsy_sema_project(analyses, srcs->count, &typed)) failed = 1;
    const Node **generic_sources = NULL;
    if (!failed) {
        size_t source_count = srcs->count > 0 ? (size_t)srcs->count : 0;
        generic_sources = calloc(source_count, sizeof(*generic_sources));
        if (!generic_sources) failed = 1;
        else {
            for (int i = 0; i < srcs->count; i++)
                generic_sources[i] = analyses[i].ast;
            lower_set_generic_sources(generic_sources, srcs->count);
        }
    }
    if (!failed) {
        for (int i = 0; i < srcs->count; i++) {
            stdpath_set_pkg_prefix(srcs->paths[i]);
            HModule *lowered = lower_run(analyses[i].ast, &typed);
            if (lowered) mods[count++] = lowered;
            if (!lowered || diag_error_count(&typed)) {
                failed = 1;
                break;
            }
        }
    }
    lower_set_generic_sources(NULL, 0);
    free(generic_sources);
    if (!failed && count && hir_tyinfer(mods, count, &typed)) failed = 1;
    if (!failed && cli->dump_hir)
        for (int i = 0; i < count; i++) hir_dump(mods[i], stdout);
    if (diag_error_count(&typed)) failed = 1;
    if (diag_warning_count(&typed) && cli->werror) failed = 1;
    if (typed.count) diag_print_all(&typed, NULL);
    for (int i = 0; i < srcs->count; i++) analysis_release(&analyses[i]);
    for (int i = 0; i < count; i++) hir_module_free(mods[i]);
    free(analyses);
    free(mods);
    diag_free(&typed);
    return failed;
}

