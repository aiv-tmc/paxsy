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
#include <stdlib.h>
#include <string.h>


static int load_quality_options(const char *root, Cli *options) {
    char cfg[PAXSY_PATH_MAX], value[32];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) return 0;
    if (toml_get(cfg, "quality", "max_errors", value, sizeof(value))) {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || !end || *end || n < 1 || n > 10000) {
            fprintf(stderr, "invalid [quality].max_errors (expected 1..10000)\n");
            return -1;
        }
        options->max_errors = (int)n;
    }
    if (toml_get(cfg, "quality", "ignore_warnings", value, sizeof(value)))
        options->ignore_warnings = strcmp(value, "true") == 0;
    if (toml_get(cfg, "quality", "warnings_as_errors", value, sizeof(value)))
        options->werror = strcmp(value, "true") == 0;
    return 0;
}

/*
 * cli/cmd_artifacts.c - the archive, benchmark and test commands:
 *   state    bundle the analysed frontend results into
 *            build/<name>.a;
 *   shared   the same for a shared build as build/<name>.sfa;
 *   bench    measure lexer throughput per file;
 *   test     compile *_test.px files and list the discovered tests.
 * An archive carries one text dump per input file plus a build
 * report; see docs/ToolchainUserManual.md. Source discovery goes through srcdir, so
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
        fprintf(stderr, "cannot create '%s'\n", path);
}

/* Packs every collected file plus the build report into one archive. */
static int emit_archive(const char *root, const char *ext, const Cli *cli) {
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "no configuration file in '%s'\n", root);
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
    if (!rf) { fprintf(stderr, "cannot write '%s'\n", report); return 1; }
    fprintf(rf, "paxsy frontend archive %s\nsources:\n", name);
    fclose(rf);

    /* Zero-initialised: srclist_push grows the array from count/cap,
     * so uninitialised storage would send realloc() to a wild
     * pointer (the release-build crash fixed together with the
     * normalisation guard inside srcdir_collect). */
    SrcList srcs = {0};
    if (srcdir_collect(root, 1, 0, &srcs) < 0) {
        fprintf(stderr, "source discovery failed under '%s'\n", root);
        return 1;
    }
    if (srcs.count == 0) {
        srclist_free(&srcs);
        fprintf(stderr, "no .px sources under '%s/src'\n", root);
        return 1;
    }

    /* 1200 bytes per member: build dir (<=511) + stem (<=127) + suffix.
     * Sized by the collected source count, so there is no fixed cap. */
    char members[srcs.count + 1][1200];
    const char *ptrs[srcs.count + 1];
    int nmembers = 0, failed = 0;

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
        int analysis_failed = pipeline_analyze(srcs.paths[i], cli, &d, &a);
        if (cli->werror && diag_warning_count(&d)) analysis_failed = 1;
        if (analysis_failed) {
            fclose(f);
            unlink(out);
            analysis_release(&a);
            diag_free(&d);
            failed = 1;
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
    if (failed) return 1;

    /* The archive lands at build/<name><ext>, next to the report and
     * the binary, inside the tree that .gitignore excludes. */
    char tdir[PAXSY_PATH_MAX];
    snprintf(tdir, sizeof(tdir), "%s/build", root);
    ensure_dir(tdir);
    char archive[PAXSY_PATH_MAX];
    snprintf(archive, sizeof(archive), "%s/%s%s", tdir, name, ext);
    if (tar_create(archive, ptrs, nmembers) != 0) {
        fprintf(stderr, "cannot write '%s'\n", archive);
        return 1;
    }
    printf("wrote %s (%d members)\n", archive, nmembers);
    return 0;
}

int cmd_state(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    Cli options = *cli;
    if (load_quality_options(root, &options) != 0) return 2;
    return emit_archive(root, ".a", &options);
}

int cmd_shared(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    Cli options = *cli;
    if (load_quality_options(root, &options) != 0) return 2;
    return emit_archive(root, ".sfa", &options);
}

/* Repeated, warmed lexer measurements. Median timing is less sensitive
 * to scheduler noise than a single sample and is stable for small files. */
static int bench_double_cmp(const void *a, const void *b) {
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

static double bench_lexer_once(const Source *src, const Cli *options,
                               int *token_count) {
    struct timespec begin, end;
    Diag d;
    TokenVec tv;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    diag_init(&d, options->max_errors);
    d.suppress_warnings = options->ignore_warnings;
    lexer_run((Source *)src, &tv, &d);
    diag_free(&d);
    clock_gettime(CLOCK_MONOTONIC, &end);
    *token_count = tv.count;
    token_vec_free(&tv);
    return (double)(end.tv_sec - begin.tv_sec) * 1000.0 +
           (double)(end.tv_nsec - begin.tv_nsec) / 1e6;
}

int cmd_bench(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    Cli options = *cli;
    if (load_quality_options(root, &options) != 0) return 2;
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "no configuration file in '%s'\n", root);
        return 2;
    }
    int iterations = 7;
    const char *iteration_env = getenv("PAXSY_BENCH_ITERATIONS");
    if (iteration_env && *iteration_env) {
        char *end = NULL;
        long parsed = strtol(iteration_env, &end, 10);
        if (end == iteration_env || *end || parsed < 1 || parsed > 101) {
            fprintf(stderr, "PAXSY_BENCH_ITERATIONS must be in 1..101\n");
            return 2;
        }
        iterations = (int)parsed;
    }
    char bench_dir[PAXSY_PATH_MAX];
    char configured[256];
    if (!toml_get(cfg, "bench", "path", configured, sizeof(configured)))
        snprintf(configured, sizeof(configured), "benches");
    snprintf(bench_dir, sizeof(bench_dir), "%s/%s", root, configured);
    SrcList srcs = {0};
    if (srcdir_collect_path(bench_dir, 1, 0, &srcs) < 0) {
        fprintf(stderr, "source discovery failed under '%s'\n", root);
        return 1;
    }
    if (srcs.count == 0) {
        srclist_free(&srcs);
        printf("no benchmark files found in %s\n", configured);
        return 0;
    }
    printf("running %d lexer benchmark%s\n\n", srcs.count,
           srcs.count == 1 ? "" : "s");
    double total_ms = 0.0;
    long total_tokens = 0;
    for (int i = 0; i < srcs.count; i++) {
        Source src;
        if (source_load(&src, srcs.paths[i]) != 0) continue;
        int tokens = 0;
        double samples[101];
        (void)bench_lexer_once(&src, &options, &tokens); /* warm-up */
        for (int n = 0; n < iterations; n++)
            samples[n] = bench_lexer_once(&src, &options, &tokens);
        qsort(samples, (size_t)iterations, sizeof(samples[0]), bench_double_cmp);
        double median = samples[iterations / 2];
        if (iterations % 2 == 0)
            median = (samples[iterations / 2 - 1] +
                      samples[iterations / 2]) / 2.0;
        const char *base = strrchr(srcs.paths[i], '/');
        base = base ? base + 1 : srcs.paths[i];
        printf("bench %-24s ... %8.3f ms  (%d tokens, %.0f tokens/sec)\n",
               base, median, tokens,
               median > 0 ? tokens / (median / 1000.0) : 0);
        total_ms += median;
        total_tokens += tokens;
        source_free(&src);
    }
    printf("\nbench result: %d files, %ld tokens, %.3f ms total\n",
           srcs.count, total_tokens, total_ms);
    printf("measurement: median of %d runs after one warm-up\n", iterations);
    srclist_free(&srcs);
    return 0;
}

/* Compile test sources and report discoveries in the same restrained,
 * line-oriented style used by Cargo and Python's unittest runner. Test
 * bodies are not executed yet: the current language test command is a
 * compile/discovery gate, so each row says "compiled", never "passed". */
int cmd_test(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    Cli options = *cli;
    char cfg[PAXSY_PATH_MAX], test_dir[PAXSY_PATH_MAX], configured[256];
    SrcList srcs = {0};
    int total = 0, failed = 0, files_ok = 0;
    if (load_quality_options(root, &options) != 0) return 2;
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "no configuration file in '%s'\n", root);
        return 2;
    }
    if (!toml_get(cfg, "test", "path", configured, sizeof(configured)))
        snprintf(configured, sizeof(configured), "tests");
    snprintf(test_dir, sizeof(test_dir), "%s/%s", root, configured);
    if (srcdir_collect_path(test_dir, 1, 1, &srcs) < 0) {
        fprintf(stderr, "test: cannot read '%s'\n", test_dir);
        return 1;
    }
    if (srcs.count == 0) {
        printf("no test files found in %s\n", configured);
        srclist_free(&srcs);
        return 0;
    }
    printf("running test discovery (%d source file%s)\n\n",
           srcs.count, srcs.count == 1 ? "" : "s");
    for (int i = 0; i < srcs.count; i++) {
        Diag d;
        Analysis a;
        int n = 0;
        const char *base = strrchr(srcs.paths[i], '/');
        base = base ? base + 1 : srcs.paths[i];
        diag_init(&d, options.max_errors);
        d.suppress_warnings = options.ignore_warnings;
        int analysis_failed = pipeline_analyze(srcs.paths[i], &options, &d, &a);
        int warnings_failed = options.werror && diag_warning_count(&d) > 0;
        if (analysis_failed || warnings_failed) {
            printf("test %-28s ... FAILED (compilation)\n", base);
            if (d.count) diag_print_all(&d, NULL);
            analysis_release(&a);
            failed++;
            diag_free(&d);
            continue;
        }
        for (int f = 0; a.ast && f < a.ast->item_count; f++) {
            Node *fn = a.ast->items[f];
            if (fn->kind == NK_FUNC && fn->name &&
                strncmp(fn->name, "test_", 5) == 0) {
                printf("test %s::%s ... compiled\n", base, fn->name);
                n++;
            }
        }
        if (n == 0) printf("test %-28s ... no test functions\n", base);
        total += n;
        files_ok++;
        analysis_release(&a);
        diag_free(&d);
    }
    srclist_free(&srcs);
    printf("\ntest result: %s; %d discovered, %d source files compiled, %d failed\n",
           failed ? "FAILED" : "ok", total, files_ok, failed);
    if (total == 0 && failed == 0)
        printf("note: define test_* functions to add tests\n");
    return failed ? 1 : 0;
}
