#define _POSIX_C_SOURCE 200809L

/* Path buffers are bounded by PAXSY_PATH_MAX; snprintf truncation is
 * always safe (NUL-terminated output), so the compiler warning about
 * theoretical truncation is disabled for this file. */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "dump.h"
#include "pipeline.h"
#include "srcdir.h"
#include "tar.h"
#include "toml.h"
#include "color.h"

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>

/*
 * cli/cmd_artifacts.c - the archive, benchmark and test commands:
 *   state    bundle the analysed frontend results into
 *            build/<name>.a;
 *   shared   the same for a shared build as build/<name>.sfa;
 *   bench    measure lexer throughput per file;
 *   test     compile *_test.px files and list the discovered tests.
 * An archive carries one text dump per input file plus a build
 * report; see docs/cli.md. Source discovery goes through srcdir, so
 * the member order of an archive is identical between runs.
 */

/* Derives the project name: the [project] name from the configuration
 * file, or the file base name with the '.toml' suffix stripped. */
static void project_name(const char *cfg, char *name, size_t n) {
    if (toml_get(cfg, "project", "name", name, n)) return;
    const char *base = strrchr(cfg, '/');
    base = base ? base + 1 : cfg;
    size_t blen = strlen(base);
    snprintf(name, n, "%.*s", blen > 5 ? (int)(blen - 5) : (int)blen, base);
}

static void ensure_dir(const char *path) {
    if (mkdir(path, 0755) != 0 && access(path, F_OK) != 0)
        fprintf(stderr, "paxsy: cannot create '%s'\n", path);
}

/* Packs every collected file plus the build report into one archive. */
static int emit_archive(const char *root, const char *ext, const Cli *cli) {
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: no configuration file in '%s'\n", root);
        return 2;
    }
    char name[128];
    project_name(cfg, name, sizeof(name));

    char dir[PAXSY_PATH_MAX], report[PAXSY_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/build/archive", root);
    ensure_dir("build");
    ensure_dir(dir);
    snprintf(report, sizeof(report), "%s/build/report.txt", root);
    FILE *rf = fopen(report, "w");
    if (!rf) { fprintf(stderr, "paxsy: cannot write '%s'\n", report); return 1; }
    fprintf(rf, "paxsy frontend archive %s\nsources:\n", name);
    fclose(rf);

    /* Zero-initialised: srclist_push grows the array from count/cap,
     * so uninitialised storage would send realloc() to a wild
     * pointer (the release-build crash fixed together with the
     * normalisation guard inside srcdir_collect). */
    SrcList srcs = {0};
    if (srcdir_collect(root, 1, 0, &srcs) < 0) {
        fprintf(stderr, "paxsy: source discovery failed under '%s'\n", root);
        return 1;
    }
    if (srcs.count == 0) {
        srclist_free(&srcs);
        fprintf(stderr, "paxsy: no .px sources under '%s/src'\n", root);
        return 1;
    }

    /* 1200 bytes per member: build dir (<=511) + stem (<=127) + suffix.
     * Sized by the collected source count, so there is no fixed cap. */
    char members[srcs.count + 1][1200];
    const char *ptrs[srcs.count + 1];
    int nmembers = 0;

    for (int i = 0; i < srcs.count; i++) {
        /* One .ast dump per source file. */
        const char *base = strrchr(srcs.paths[i], '/');
        base = base ? base + 1 : srcs.paths[i];
        char out[PAXSY_PATH_MAX];
        snprintf(out, sizeof(out), "%s/%.*s.ast", dir,
                 (int)(strlen(base) > 3 ? strlen(base) - 3 : strlen(base)), base);
        FILE *f = fopen(out, "w");
        if (!f) continue;
        Analysis a;
        Diag d;
        diag_init(&d, cli->max_errors);
        d.suppress_warnings = cli->ignore_warnings;
        if (pipeline_analyze(srcs.paths[i], cli, &d, &a)) {
            fclose(f);
            unlink(out);
            analysis_release(&a);
            diag_free(&d);
            continue;
        }
        if (cli->dump_tokens) dump_tokens(&a.tokens, f);
        else dump_ast(a.ast, f, 0);
        analysis_release(&a);
        diag_free(&d);
        fclose(f);
        snprintf(members[nmembers], 1200, "%s", out);
        ptrs[nmembers] = members[nmembers];
        nmembers++;

        rf = fopen(report, "a");
        if (rf) { fprintf(rf, "  %s\n", srcs.paths[i]); fclose(rf); }
    }
    srclist_free(&srcs);

    /* The archive lands at build/<name><ext>, next to the report and
     * the binary, inside the tree that .gitignore excludes. */
    char tdir[PAXSY_PATH_MAX];
    snprintf(tdir, sizeof(tdir), "%s/build", root);
    ensure_dir(tdir);
    char archive[PAXSY_PATH_MAX];
    snprintf(archive, sizeof(archive), "%s/%s%s", tdir, name, ext);
    if (tar_create(archive, ptrs, nmembers) != 0) {
        fprintf(stderr, "paxsy: cannot write '%s'\n", archive);
        return 1;
    }
    printf("wrote %s (%d members)\n", archive, nmembers);
    return 0;
}

int cmd_state(const Cli *cli) {
    return emit_archive(cli->path ? cli->path : ".", ".a", cli);
}

int cmd_shared(const Cli *cli) {
    return emit_archive(cli->path ? cli->path : ".", ".sfa", cli);
}

/* Benchmarks the lexer per file; see docs/cli.md. */
int cmd_bench(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: no configuration file in '%s'\n", root);
        return 2;
    }
    /* Zero-initialised: see emit_archive for the reason. */
    SrcList srcs = {0};
    if (srcdir_collect(root, 1, 0, &srcs) < 0) {
        fprintf(stderr, "paxsy: source discovery failed under '%s'\n", root);
        return 1;
    }
    if (srcs.count == 0) {
        srclist_free(&srcs);
        fprintf(stderr, "paxsy: no .px sources under '%s/src'\n", root);
        return 1;
    }
    printf("%-28s %10s %10s %14s\n", "file", "ms", "tokens", "tokens/sec");
    double total_ms = 0.0;
    long total_tokens = 0;
    for (int i = 0; i < srcs.count; i++) {
        Source src;
        if (source_load(&src, srcs.paths[i]) != 0) continue;
        struct timespec begin, end;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        Diag d;
        diag_init(&d, cli->max_errors);
        TokenVec tv;
        lexer_run(&src, &tv, &d);
        diag_free(&d);
        clock_gettime(CLOCK_MONOTONIC, &end);
        double ms = (double)(end.tv_sec - begin.tv_sec) * 1000.0 +
                    (double)(end.tv_nsec - begin.tv_nsec) / 1e6;
        const char *base = strrchr(srcs.paths[i], '/');
        base = base ? base + 1 : srcs.paths[i];
        printf("%-28s %10.2f %10d %14.0f\n", base, ms, tv.count,
               ms > 0 ? tv.count / (ms / 1000.0) : 0);
        total_ms += ms;
        total_tokens += tv.count;
        token_vec_free(&tv);
        source_free(&src);
    }
    printf("%-28s %10.2f %10ld %14.0f\n", "total", total_ms, total_tokens,
           total_ms > 0 ? total_tokens / (total_ms / 1000.0) : 0);
    srclist_free(&srcs);
    return 0;
}

/* Compiles *_test.px files and lists the discovered test functions.
 * No test runner exists yet, so a human-readable list is the output. */
int cmd_test(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: no configuration file in '%s'\n", root);
        return 2;
    }
    /* Zero-initialised: see emit_archive for the reason. */
    SrcList srcs = {0};
    if (srcdir_collect(root, 1, 1, &srcs) < 0) {
        fprintf(stderr, "paxsy: source discovery failed under '%s'\n", root);
        return 1;
    }
    if (srcs.count == 0) {
        srclist_free(&srcs);
        printf("no test files found\n");
        return 0;
    }
    int total = 0, failed = 0;
    for (int i = 0; i < srcs.count; i++) {
        Diag d;
        diag_init(&d, cli->max_errors);
        d.suppress_warnings = cli->ignore_warnings;
        Analysis a;
        if (pipeline_analyze(srcs.paths[i], cli, &d, &a)) {
            const char *base = strrchr(srcs.paths[i], '/');
            base = base ? base + 1 : srcs.paths[i];
            printf("%sFAIL%s %s\n", c_red, c_reset, base);
            failed++;
            diag_free(&d);
            continue;
        }
        int n = 0;
        const char *base = strrchr(srcs.paths[i], '/');
        base = base ? base + 1 : srcs.paths[i];
        printf("%s%s%s\n", c_cyan, base, c_reset);
        for (int f = 0; f < a.ast->item_count; f++) {
            Node *fn = a.ast->items[f];
            if (fn->kind == NK_FUNC && fn->name &&
                strncmp(fn->name, "test_", 5) == 0) {
                printf("    %s\n", fn->name);
                n++;
            }
        }
        total += n;
        printf("%d test(s) in %s\n", n, base);
        analysis_release(&a);
        diag_free(&d);
    }
    srclist_free(&srcs);
    printf("discovered %d test(s)%s\n", total,
           failed ? "; some files failed to compile" : "");
    return failed ? 1 : 0;
}
