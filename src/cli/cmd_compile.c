#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "pipeline.h"
#include "node.h"
#include "srcdir.h"
#include "stdpath.h"
#include "toml.h"
#include "fsutil.h"
#include "hir.h"
#include "lower.h"
#include "tyinfer.h"
#include "opt.h"
#include "nemit/nemit.h"
#include "nemit/nar.h"
#include "nemit/npe.h"
#include "offload.h"
#include "llic.h"
#include "mlir.h"
#include "cuda.h"
#include "output_path.h"
#include "cmd_emit.h"
#include "cmd_check.h"
#include "source.h"
#include "recovery_metadata.h"

#include <time.h>


/*
 * cli/cmd_compile.c - the code generation driver behind `paxsy build`.
 *
 * Every source is lowered into typed HIR and optimised by the HIR
 * pipeline (constant/copy propagation, constant folding, strength
 * reduction, dead-code elimination). Before each file is lowered its
 * package context is announced, so the standard library sources
 * (which ride along in the source list) lower under the std::
 * namespace and resolve against qualified references from the
 * program. The build is fully self-hosted: the native emission pass
 * turns the HIR into a static x86-64 ELF executable in-process. No C
 * or LLVM translation unit is produced and no external tool
 * (compiler, assembler, linker) is invoked at any step. The final
 * executable and incremental stamp live directly in build/; build/bin/
 * is reserved for relocatable object files.
 *
 * Additional outputs are selected by [artifacts] and [build.target]
 * in the project TOML. The native executable is still emitted by the
 * normal build pipeline unless PE is selected as the primary target.
 */

/* Fills `name` with the project name: the [project] table wins, and
 * the configuration file base name (minus ".toml") is the fallback;
 * the strip is length-guarded so short names cannot wrap around. */
static void project_name(const char *cfg, char *name, size_t cap) {
    if (toml_get(cfg, "project", "name", name, cap)) return;
    const char *base = strrchr(cfg, '/');
    base = base ? base + 1 : cfg;
    size_t blen = strlen(base);
    snprintf(name, cap, "%.*s",
             blen > 5 ? (int)(blen - 5) : (int)blen, base);
}




static double compile_elapsed(struct timespec begin, struct timespec end) {
    return (double)(end.tv_sec - begin.tv_sec) +
           (double)(end.tv_nsec - begin.tv_nsec) / 1000000000.0;
}

static void profile_add(double *total, struct timespec begin) {
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    *total += compile_elapsed(begin, end);
}

int compile_executable(Cli *cli, const char *root,
                       const char *cfg, SrcList *srcs) {
    char name[128];
    project_name(cfg, name, sizeof(name));

    Diag d;
    diag_init(&d, cli->max_errors);
    HModule **mods = calloc((size_t)srcs->count, sizeof(HModule *));
    Analysis *as = calloc((size_t)srcs->count, sizeof(Analysis));
    int nmods = 0;
    int rc = 1;
    int pe_only = 0;
    int emit_started = 0;
    double source_time = 0.0, lex_time = 0.0, parse_time = 0.0;
    double sema_time = 0.0, lower_time = 0.0, type_time = 0.0;
    double opt_time = 0.0, emit_time = 0.0;
    OptProfile opt_profile_total;
    memset(&opt_profile_total, 0, sizeof(opt_profile_total));
    struct timespec phase_begin;
    if (!mods || !as) {
        fprintf(stderr, "build: out of memory while allocating module state\n");
        free(mods);
        free(as);
        diag_free(&d);
        return 1;
    }
    int parsed = 0;
    for (int i = 0; i < srcs->count; i++) {
        Diag front;
        diag_init(&front, cli->max_errors);
        front.suppress_warnings = cli->ignore_warnings;
        if (cli->verbose) printf("checking %s\n", srcs->paths[i]);
        int parse_rc = pipeline_parse(srcs->paths[i], cli, &front, &as[i]);
        if (cli->werror && diag_warning_count(&front)) parse_rc = 1;
        diag_free(&front);
        if (cli->profile) {
            source_time += as[i].source_seconds;
            lex_time += as[i].lex_seconds;
            parse_time += as[i].parse_seconds;
        }
        if (parse_rc) { rc = 1; break; }
        parsed++;
    }
    if (parsed == srcs->count) {
        if (cli->profile) clock_gettime(CLOCK_MONOTONIC, &phase_begin);
        if (paxsy_sema_project(as, parsed, &d) != 0) rc = 1;
        if (cli->profile) profile_add(&sema_time, phase_begin);
    }
    if (cli->werror && diag_warning_count(&d)) rc = 1;
    const Node **generic_sources = NULL;
    if (rc == 1 && diag_error_count(&d) == 0 &&
        !(cli->werror && diag_warning_count(&d))) {
        size_t source_count = parsed > 0 ? (size_t)parsed : 0;
        generic_sources = calloc(source_count, sizeof(*generic_sources));
        if (!generic_sources) {
            diag_report(&d, DK_ERROR, "<lower>", 0, 0,
                        "out of memory preparing project-wide generic declarations");
        } else {
            for (int i = 0; i < parsed; i++) generic_sources[i] = as[i].ast;
            lower_set_generic_sources(generic_sources, parsed);
            for (int i = 0; i < parsed; i++) {
                stdpath_set_pkg_prefix(srcs->paths[i]);
                if (cli->profile) clock_gettime(CLOCK_MONOTONIC, &phase_begin);
                HModule *m = lower_run(as[i].ast, &d);
                if (cli->profile) profile_add(&lower_time, phase_begin);
                if (!m) { rc = 1; break; }
                mods[nmods++] = m;
            }
        }
    }
    lower_set_generic_sources(NULL, 0);
    free(generic_sources);
    /* Type every module against the whole program's bindings so
     * cross-module calls resolve. */
    if (rc == 1 && diag_error_count(&d) == 0) {
        if (cli->profile) clock_gettime(CLOCK_MONOTONIC, &phase_begin);
        if (hir_tyinfer(mods, nmods, &d)) rc = 1;
        if (cli->profile) profile_add(&type_time, phase_begin);
    }
    if (rc == 1 && diag_error_count(&d) == 0 && cli->dump_hir)
        for (int i = 0; i < nmods; i++) hir_dump(mods[i], stdout);
    if (rc == 1 && diag_error_count(&d) == 0)
    for (int i = 0; i < nmods; i++) {
        /* Optimise the typed HIR before the backend sees it. */
        char optimization[32] = "3";
        int opt_level = 3;
        if (toml_get(cfg, "build", "optimization", optimization,
                     sizeof(optimization))) {
            char *end = NULL;
            long parsed_level = strtol(optimization, &end, 10);
            if (end == optimization || !end || *end ||
                parsed_level < 0 || parsed_level > 3) {
                diag_report(&d, DK_ERROR, "<config>", 0, 0,
                            "build.optimization must be an integer from 0 to 3");
                parsed_level = 0;
            }
            opt_level = (int)parsed_level;
        }
        OptConfig ocfg = opt_config_for_level(opt_level);
        /* PAXSY_NO_OPT=1 disables the pipeline (A/B benchmarking). */
        if (getenv("PAXSY_NO_OPT")) ocfg.max_iterations = 0;
        OptProfile module_profile;
        if (cli->profile) clock_gettime(CLOCK_MONOTONIC, &phase_begin);
        long nopt = opt_run_profile(mods[i], &ocfg,
                                    cli->profile ? &module_profile : NULL);
        if (cli->profile) {
            profile_add(&opt_time, phase_begin);
#define ADD_OPT_PROFILE(field) opt_profile_total.field += module_profile.field
            ADD_OPT_PROFILE(cfg_seconds); ADD_OPT_PROFILE(prop_seconds);
            ADD_OPT_PROFILE(gvn_seconds); ADD_OPT_PROFILE(fold_seconds);
            ADD_OPT_PROFILE(strength_seconds); ADD_OPT_PROFILE(store_seconds);
            ADD_OPT_PROFILE(dce_seconds); ADD_OPT_PROFILE(simplify_seconds);
            ADD_OPT_PROFILE(sroa_seconds);
            ADD_OPT_PROFILE(cfg_calls); ADD_OPT_PROFILE(prop_calls);
            ADD_OPT_PROFILE(gvn_calls); ADD_OPT_PROFILE(fold_calls);
            ADD_OPT_PROFILE(strength_calls); ADD_OPT_PROFILE(store_calls);
            ADD_OPT_PROFILE(dce_calls); ADD_OPT_PROFILE(simplify_calls);
            ADD_OPT_PROFILE(sroa_calls);
            ADD_OPT_PROFILE(cfg_changes); ADD_OPT_PROFILE(prop_changes);
            ADD_OPT_PROFILE(gvn_changes); ADD_OPT_PROFILE(fold_changes);
            ADD_OPT_PROFILE(strength_changes); ADD_OPT_PROFILE(store_changes);
            ADD_OPT_PROFILE(dce_changes); ADD_OPT_PROFILE(simplify_changes);
            ADD_OPT_PROFILE(sroa_changes);
            ADD_OPT_PROFILE(rounds);
#undef ADD_OPT_PROFILE
        }
        (void)nopt;
        if (cli->offload_sim) {
            printf("offload simulation for %s:\n", srcs->paths[i]);
            offload_simulate(&mods[i], 1, stdout);
        } else if (cli->dump_offload || cli->verbose) {
            printf("offload analysis for %s:\n", srcs->paths[i]);
            offload_report(&mods[i], 1, stdout);
        }
    }

    /* An internal override is used by the debugger to force a native
     * executable. Ordinary builds always use the project target. */
    char configured_target[32] = "elf";
    const char *target = cli->target[0] ? cli->target : NULL;
    if (!target && toml_get(cfg, "build", "target", configured_target,
                            sizeof(configured_target)))
        target = configured_target;
    if (target) {
        if (strcmp(target, "elf") == 0 || strcmp(target, "native") == 0 ||
            strcmp(target, "exe") == 0 ||
            strcmp(target, "linux-x86_64") == 0 ||
            strcmp(target, "x86_64-linux-gnu") == 0) {
            /* Native x86-64 ELF executable. */
        } else if (strcmp(target, "pe") == 0 ||
                   strcmp(target, "windows-x86_64") == 0 ||
                   strcmp(target, "x86_64-windows-gnu") == 0) {
            cli->emit_pe = 1;
            pe_only = 1;
        } else if (strcmp(target, "obj") == 0 ||
                   strcmp(target, "o") == 0) {
            cli->emit_obj = 1;
        } else if (strcmp(target, "a") == 0 ||
                   strcmp(target, "static") == 0 ||
                   strcmp(target, "lib") == 0) {
            cli->emit_obj = 1;
            cli->emit_a = 1;
        } else if (strcmp(target, "llvm") == 0) {
            cli->emit_llvm = 1;
        } else if (strcmp(target, "mlir") == 0) {
            cli->emit_mlir = 1;
        } else if (strcmp(target, "cuda") == 0) {
            cli->emit_cuda = 1;
        } else if (strcmp(target, "linux-aarch64") == 0 ||
                   strcmp(target, "windows-aarch64") == 0 ||
                   strcmp(target, "macos-aarch64") == 0 ||
                   strcmp(target, "macos-x86_64") == 0) {
            diag_report(&d, DK_ERROR, "<cli>", 0, 0,
                        "target '%s' is not implemented; current machine-code backends support x86-64 ELF and PE32+ only",
                        target);
            rc = 1;
            goto compile_cleanup;
        } else {
            diag_report(&d, DK_ERROR, "<cli>", 0, 0,
                        "unknown target '%s' (exe linux-x86_64 windows-x86_64 pe obj a llvm mlir cuda)",
                        target);
            rc = 1;
            goto compile_cleanup;
        }
    }

    /* Executables live directly in build/. Object files alone live in
     * build/bin/; textual and archive artifacts live in build/artifacts/. */
    char bin[PAXSY_PATH_MAX];
    if (paxsy_output_path(root, cfg, name, pe_only,
                          bin, sizeof(bin)) != 0) {
        diag_report(&d, DK_ERROR, cfg, 0, 0,
                    "configured build.output path is too long");
        rc = 1;
        goto compile_cleanup;
    }
    if (rc == 1 && diag_error_count(&d) == 0) {
        if (cli->profile) {
            clock_gettime(CLOCK_MONOTONIC, &phase_begin);
            emit_started = 1;
        }
        char bdir[PAXSY_PATH_MAX], tdir[PAXSY_PATH_MAX];
        char adir[PAXSY_PATH_MAX];
        int listing_failed = 0;
        snprintf(bdir, sizeof(bdir), "%s/build", root);
        snprintf(tdir, sizeof(tdir), "%s/build/bin", root);
        snprintf(adir, sizeof(adir), "%s/build/artifacts", root);
        int need_artifacts = cli->emit_cuda || cli->emit_mlir ||
                             cli->emit_llvm || cli->emit_a;
        if (fs_mkdir_p(bdir) != 0 || fs_mkdir_p(tdir) != 0 ||
            (need_artifacts && fs_mkdir_p(adir) != 0)) {
            fprintf(stderr, "build: cannot create artifact directories\n");
            rc = 1;
            goto compile_cleanup;
        }
        char *out_slash = strrchr(bin, '/');
        if (out_slash) {
            char parent[PAXSY_PATH_MAX];
            size_t parent_len = (size_t)(out_slash - bin);
            if (parent_len >= sizeof(parent)) parent_len = sizeof(parent) - 1;
            memcpy(parent, bin, parent_len);
            parent[parent_len] = '\0';
            if (fs_mkdir_p(parent) != 0) {
                fprintf(stderr, "cannot create configured output directory\n");
                rc = 1;
                goto compile_cleanup;
            }
        }

        /* Emit every additional artifact enabled in the project TOML. */
        if (cli->emit_cuda) {
            char gpath[PAXSY_PATH_MAX];
            snprintf(gpath, sizeof(gpath), "%s/build/artifacts/%s.cu", root, name);
            FILE *gf = fopen(gpath, "w");
            if (gf) {
                if (cuda_emit_modules(mods, nmods, &d, gf) != 0) {
                    fprintf(stderr, "CUDA emission failed\n");
                    fclose(gf);
                    remove(gpath);
                    rc = 1;   /* a partial listing is not an artifact */
                    listing_failed = 1;
                } else fclose(gf);
            } else {
                fprintf(stderr, "cannot write '%s'\n", gpath);
                listing_failed = 1;
            }
        }
        if (cli->emit_mlir) {
            char mpath[PAXSY_PATH_MAX];
            snprintf(mpath, sizeof(mpath), "%s/build/artifacts/%s.mlir", root, name);
            FILE *mf = fopen(mpath, "w");
            if (mf) {
                if (mlir_modules(mods, nmods, &d, mf) != 0) {
                    fprintf(stderr, "MLIR emission failed\n");
                    if (diag_error_count(&d) > 0)
                        diag_print_all(&d, NULL);
                    fclose(mf);
                    remove(mpath);
                    rc = 1;
                    listing_failed = 1;
                } else if (fclose(mf) != 0) {
                    listing_failed = 1;
                }
            } else {
                fprintf(stderr, "cannot write '%s'\n", mpath);
                listing_failed = 1;
            }
        }
        if (cli->emit_llvm) {
            char lpath[PAXSY_PATH_MAX];
            /* listing failure must survive the native build below */
            snprintf(lpath, sizeof(lpath), "%s/build/artifacts/%s.ll", root, name);
            FILE *lf = fopen(lpath, "w");
            if (lf) {
                if (llic_modules(mods, nmods, &d, lf) != 0) {
                    fprintf(stderr, "LLVM emission failed\n");
                    if (diag_error_count(&d) > 0)
                        diag_print_all(&d, NULL);
                    fclose(lf);
                    remove(lpath);
                    rc = 1;
                    listing_failed = 1;
                } else if (fclose(lf) != 0) {
                    listing_failed = 1;
                }
            } else {
                fprintf(stderr, "cannot write '%s'\n", lpath);
                listing_failed = 1;
            }
        }
        if (cli->emit_obj || cli->emit_a || cli->emit_pe) {
            char opath[PAXSY_PATH_MAX];
            snprintf(opath, sizeof(opath), "%s/build/bin/%s.o", root, name);
            if (nemit_obj(mods, nmods, &d, opath) != 0) {
                fprintf(stderr, "object emission failed\n");
                listing_failed = 1;
            }
            if (!listing_failed && cli->emit_a &&
                paxsy_emit_archive(root, name, opath) != 0)
                listing_failed = 1;
            if (!listing_failed && cli->emit_pe &&
                paxsy_emit_pe_image(root, cfg, name, bin, pe_only,
                              mods, nmods, &d) != 0)
                listing_failed = 1;
            if (pe_only && !listing_failed && cli->emit_pe) rc = 0;
        }

        /* A PE target emits only the PE executable; other builds emit ELF. */
        if (!pe_only) rc = nemit_build(mods, nmods, &d, bin);
        if (rc == 0 && !pe_only) {
            char embed_source[16];
            if (toml_get(cfg, "recovery", "embed_source", embed_source,
                         sizeof(embed_source)) &&
                strcmp(embed_source, "true") == 0 &&
                paxsy_embed_sources(bin, root, srcs) != 0) {
                fprintf(stderr,
                        "build: could not embed recovery source metadata in '%s'\n",
                        bin);
                rc = 1;
            }
        }
        if (listing_failed) rc = 1;
    }
    /* Every native build leaves its relocatable object under
     * build/bin/; the content stamp lives in build/.paxsy-hash. */
    if (rc == 0 && !cli->emit_obj && !cli->emit_a && !cli->emit_pe &&
        !(cli->emit_cuda || cli->emit_mlir || cli->emit_llvm)) {
        char opath[PAXSY_PATH_MAX];
        snprintf(opath, sizeof(opath),
                 "%s/build/bin/%s.o", root, name);
        if (nemit_obj(mods, nmods, &d, opath) != 0) rc = 1;
    }
compile_cleanup:
    if (cli->profile && emit_started) profile_add(&emit_time, phase_begin);
    for (int i = 0; i < nmods; i++) hir_module_free(mods[i]);
    for (int i = 0; i < srcs->count; i++) analysis_release(&as[i]);
    free(as);
    free(mods);
    if (d.count > 0) diag_print_all(&d, NULL);
    if (cli->profile) {
        double total = source_time + lex_time + parse_time + sema_time +
                       lower_time + type_time + opt_time + emit_time;
        fprintf(stderr,
            "\nPaxsy compilation profile (summed across %d source file%s)\n"
            "  source loading : %8.3f ms\n"
            "  lexing         : %8.3f ms\n"
            "  parsing        : %8.3f ms\n"
            "  semantics      : %8.3f ms\n"
            "  lowering       : %8.3f ms\n"
            "  HIR typing     : %8.3f ms\n"
            "  optimization   : %8.3f ms\n"
            "  code generation: %8.3f ms\n"
            "  measured total : %8.3f ms\n",
            parsed, parsed == 1 ? "" : "s",
            source_time * 1000.0, lex_time * 1000.0, parse_time * 1000.0,
            sema_time * 1000.0, lower_time * 1000.0, type_time * 1000.0,
            opt_time * 1000.0, emit_time * 1000.0, total * 1000.0);
        fprintf(stderr,
            "\nHIR optimizer profile (all modules)\n"
            "  pass          CPU ms   calls  changes\n"
            "  CFG         %8.3f %7lu %8ld\n"
            "  propagation %8.3f %7lu %8ld\n"
            "  GVN         %8.3f %7lu %8ld\n"
            "  fold        %8.3f %7lu %8ld\n"
            "  strength    %8.3f %7lu %8ld\n"
            "  dead-store  %8.3f %7lu %8ld\n"
            "  DCE         %8.3f %7lu %8ld\n"
            "  simplify    %8.3f %7lu %8ld\n"
            "  SROA        %8.3f %7lu %8ld\n"
            "  fixpoint rounds: %lu\n",
            opt_profile_total.cfg_seconds * 1000.0, opt_profile_total.cfg_calls, opt_profile_total.cfg_changes,
            opt_profile_total.prop_seconds * 1000.0, opt_profile_total.prop_calls, opt_profile_total.prop_changes,
            opt_profile_total.gvn_seconds * 1000.0, opt_profile_total.gvn_calls, opt_profile_total.gvn_changes,
            opt_profile_total.fold_seconds * 1000.0, opt_profile_total.fold_calls, opt_profile_total.fold_changes,
            opt_profile_total.strength_seconds * 1000.0, opt_profile_total.strength_calls, opt_profile_total.strength_changes,
            opt_profile_total.store_seconds * 1000.0, opt_profile_total.store_calls, opt_profile_total.store_changes,
            opt_profile_total.dce_seconds * 1000.0, opt_profile_total.dce_calls, opt_profile_total.dce_changes,
            opt_profile_total.simplify_seconds * 1000.0, opt_profile_total.simplify_calls, opt_profile_total.simplify_changes,
            opt_profile_total.sroa_seconds * 1000.0, opt_profile_total.sroa_calls, opt_profile_total.sroa_changes,
            opt_profile_total.rounds);
    }
    diag_free(&d);
    return rc;
}
