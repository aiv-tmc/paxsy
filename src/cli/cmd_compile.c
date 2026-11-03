#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "pipeline.h"
#include "srcdir.h"
#include "stdpath.h"
#include "toml.h"
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

#include <sys/stat.h>

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
 * (compiler, assembler, linker) is invoked at any step; build/
 * carries only the binary and the incremental-rebuild stamp under
 * build/bin/.
 *
 * The explicit emission flags write their artifacts only: an MLIR
 * unit (--emit-mlir), an LLVM assembly unit (--emit-llvm), a CUDA
 * unit (--emit-cuda), an ELF relocatable object (--emit-obj), an ar
 * archive holding the object (--emit-a) and a PE32+ image
 * (--emit-exe). The binary always comes from the native pass. The
 * offload analysis report is informational and printed either way.
 */

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

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

int compile_executable(const Cli *cli, const char *root,
                       const char *cfg, SrcList *srcs) {
    char name[128];
    project_name(cfg, name, sizeof(name));

    Diag d;
    diag_init(&d, cli->max_errors);
    HModule **mods = calloc((size_t)srcs->count, sizeof(HModule *));
    Analysis *as = calloc((size_t)srcs->count, sizeof(Analysis));
    int nmods = 0;
    int rc = 1;
    for (int i = 0; i < srcs->count; i++) {
        if (pipeline_analyze(srcs->paths[i], cli, &d, &as[i])) { rc = 1; break; }
        /* Announce the package context of this file immediately
         * before lowering it: the static prefix is per-file, and a
         * stale announcement from another file would name this
         * file's modules wrongly. */
        stdpath_set_pkg_prefix(srcs->paths[i]);
        HModule *m = lower_run(as[i].ast, &d);
        if (!m || (diag_error_count(&d) == 0 && hir_tyinfer(m, &d))) {
            if (m) hir_module_free(m);
            rc = 1;
            break;
        }
        /* Optimise the typed HIR before the backend sees it. */
        OptConfig ocfg = opt_defaults();
        /* PAXSY_NO_OPT=1 disables the pipeline (A/B benchmarking). */
        if (getenv("PAXSY_NO_OPT")) ocfg.max_iterations = 0;
        long nopt = opt_run(m, &ocfg);
        if (nopt > 0)
            printf("optimized %s (%ld rewrites)\n", srcs->paths[i], nopt);
        if (cli->offload_sim) {
            printf("offload simulation for %s:\n", srcs->paths[i]);
            offload_simulate(&m, 1, stdout);
        } else if (cli->dump_offload || cli->verbose) {
            printf("offload analysis for %s:\n", srcs->paths[i]);
            offload_report(&m, 1, stdout);
        }
        mods[nmods++] = m;
    }

    /* The binary lives at build/<name>, the rebuild stamp and the
     * artifact listings under build/bin/. */
    char bin[PAXSY_PATH_MAX];
    snprintf(bin, sizeof(bin), "%s/build/%s", root, name);
    if (rc == 1 && diag_error_count(&d) == 0) {
        char bdir[PAXSY_PATH_MAX], tdir[PAXSY_PATH_MAX];
        snprintf(bdir, sizeof(bdir), "%s/build", root);
        snprintf(tdir, sizeof(tdir), "%s/build/bin", root);
        if (!is_dir(bdir)) mkdir(bdir, 0755);
        if (!is_dir(tdir)) mkdir(tdir, 0755);

        /* The explicit flags write their artifact listings only. */
        if (cli->emit_cuda) {
            char gpath[PAXSY_PATH_MAX];
            snprintf(gpath, sizeof(gpath), "%s/build/bin/%s.cu", root, name);
            FILE *gf = fopen(gpath, "w");
            if (gf) {
                if (cuda_emit_modules(mods, nmods, &d, gf) != 0)
                    fprintf(stderr, "paxsy: CUDA emission failed\n");
                fclose(gf);
            } else {
                fprintf(stderr, "paxsy: cannot write '%s'\n", gpath);
            }
        }
        if (cli->emit_mlir) {
            char mpath[PAXSY_PATH_MAX];
            snprintf(mpath, sizeof(mpath), "%s/build/bin/%s.mlir", root, name);
            FILE *mf = fopen(mpath, "w");
            if (mf) {
                if (mlir_modules(mods, nmods, &d, mf) != 0)
                    fprintf(stderr, "paxsy: MLIR emission failed\n");
                fclose(mf);
            } else {
                fprintf(stderr, "paxsy: cannot write '%s'\n", mpath);
            }
        }
        if (cli->emit_llvm) {
            char lpath[PAXSY_PATH_MAX];
            snprintf(lpath, sizeof(lpath), "%s/build/bin/%s.ll", root, name);
            FILE *lf = fopen(lpath, "w");
            if (lf) {
                if (llic_modules(mods, nmods, &d, lf) != 0)
                    fprintf(stderr, "paxsy: LLVM emission failed\n");
                fclose(lf);
            } else {
                fprintf(stderr, "paxsy: cannot write '%s'\n", lpath);
            }
        }
        if (cli->emit_obj || cli->emit_a || cli->emit_pe) {
            char opath[PAXSY_PATH_MAX];
            snprintf(opath, sizeof(opath), "%s/build/bin/%s.o", root, name);
            if (nemit_obj(mods, nmods, &d, opath) != 0) {
                fprintf(stderr, "paxsy: object emission failed\n");
            } else if (cli->emit_a) {
                NAMember m;
                char apath[PAXSY_PATH_MAX];
                char mname[160];
                FILE *of = fopen(opath, "rb");
                if (of) {
                    fseek(of, 0, SEEK_END);
                    long sz = ftell(of);
                    fseek(of, 0, SEEK_SET);
                    uint8_t *objbuf = malloc((size_t)sz);
                    if (objbuf && fread(objbuf, 1, (size_t)sz, of)
                                 == (size_t)sz) {
                        snprintf(apath, sizeof(apath),
                                 "%s/build/bin/%s.a", root, name);
                        snprintf(mname, sizeof(mname), "%s.o", name);
                        m.name = mname;
                        m.data = objbuf;
                        m.size = (size_t)sz;
                        if (nar_write(apath, &m, 1) != 0)
                            fprintf(stderr,
                                    "paxsy: archive emission failed\n");
                    }
                    free(objbuf);
                    fclose(of);
                }
            } else if (cli->emit_pe) {
                char epath[PAXSY_PATH_MAX];
                snprintf(epath, sizeof(epath),
                         "%s/build/bin/%s.exe", root, name);
                if (nemit_pe(mods, nmods, &d, epath) != 0)
                    fprintf(stderr, "paxsy: PE emission failed\n");
            }
        }

        /* The build itself: fully self-hosted native emission. */
        rc = nemit_build(mods, nmods, &d, bin);
    }
    /* The incremental rebuild check (cli/build.c) compares the
     * optimisation stamp so a PAXSY_NO_OPT change without source
     * edits still rebuilds. */
    if (rc == 0 && !(cli->emit_cuda || cli->emit_mlir || cli->emit_llvm)) {
        char stamp[PAXSY_PATH_MAX];
        snprintf(stamp, sizeof(stamp), "%s/build/bin/%s.opt", root, name);
        FILE *sf = fopen(stamp, "wb");
        if (sf) {
            fputc(getenv("PAXSY_NO_OPT") ? '1' : '0', sf);
            fclose(sf);
        }
    }
    for (int i = 0; i < nmods; i++) hir_module_free(mods[i]);
    for (int i = 0; i < srcs->count; i++) analysis_release(&as[i]);
    free(as);
    free(mods);
    if (diag_error_count(&d) > 0)
        diag_print_all(&d, NULL);  /* HIR errors carry no source line */
    diag_free(&d);
    return rc;
}
