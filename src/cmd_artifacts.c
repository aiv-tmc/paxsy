#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "pipeline.h"
#include "dump.h"
#include "parser.h"
#include "source.h"
#include "toml.h"
#include "tar.h"

#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

/*
 * cmd_artifacts.c - analysis-derived commands:
 *   test   compile *_test.px files and list the test_* functions;
 *   bench  measure lexer/parser throughput of every source file;
 *   state / shared  pack per-file AST dumps into a ustar archive.
 */

#define MAX_SRCS 64
typedef struct { char paths[MAX_SRCS][PAXSY_PATH_MAX]; int count; } SrcList;

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void collect_dir(const char *dir, int tests_only, SrcList *list) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && list->count < MAX_SRCS) {
        if (e->d_name[0] == '.') continue;
        char path[PAXSY_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { collect_dir(path, tests_only, list); continue; }
        size_t len = strlen(e->d_name);
        if (len < 3 || strcmp(e->d_name + len - 3, ".px") != 0) continue;
        if (tests_only &&
            !(len > 8 && strcmp(e->d_name + len - 8, "_test.px") == 0))
            continue;
        snprintf(list->paths[list->count], PAXSY_PATH_MAX, "%s", path);
        list->count++;
    }
    closedir(d);
}

/* Recursively lists functions whose name starts with test_. */
static int count_tests(const Node *n, const char *file, int *total) {
    int found = 0;
    if (!n) return 0;                       /* empty child slots are normal */
    if (n->kind == NK_FUNC && n->name &&
        strncmp(n->name, "test_", 5) == 0) {
        printf("    %s :: %s\n", file, n->name);
        (*total)++;
        found = 1;
    }
    for (int i = 0; i < 4; i++) found |= count_tests(n->child[i], file, total);
    for (int i = 0; i < n->item_count; i++)
        found |= count_tests(n->items[i], file, total);
    return found;
}

int cmd_test(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    SrcList srcs;
    memset(&srcs, 0, sizeof(srcs));
    char srcdir[PAXSY_PATH_MAX];
    snprintf(srcdir, sizeof(srcdir), "%s/src", root);
    collect_dir(srcdir, 1, &srcs);
    if (srcs.count == 0) {
        printf("no test files found\n");
        return 0;
    }
    int failed = 0, total = 0;
    for (int i = 0; i < srcs.count; i++) {
        Diag d;
        diag_init(&d, cli->max_errors);
        d.suppress_warnings = cli->ignore_warnings;
        Analysis a;
        const char *base2 = strrchr(srcs.paths[i], '/');
        base2 = base2 ? base2 + 1 : srcs.paths[i];
        int rc = pipeline_analyze(srcs.paths[i], cli, &d, &a);
        if (cli->werror && diag_warning_count(&d) > 0) rc = 1;
        printf("%s: %s\n", base2, rc ? "FAILED" : "ok");
        if (!rc && a.ast) {
            int before = total;
            count_tests(a.ast, base2, &total);
            if (total == before) printf("    (no test_* functions)\n");
        }
        if (rc) failed++;
        analysis_release(&a);
        diag_free(&d);
    }
    printf("discovered %d test(s)%s\n", total,
           failed ? "; some files failed to compile" : "");
    return failed ? 1 : 0;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int cmd_bench(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    SrcList srcs;
    memset(&srcs, 0, sizeof(srcs));
    char srcdir[PAXSY_PATH_MAX];
    snprintf(srcdir, sizeof(srcdir), "%s/src", root);
    collect_dir(srcdir, 0, &srcs);
    if (srcs.count == 0) {
        fprintf(stderr, "paxsy: no .px sources under '%s/src'\n", root);
        return 1;
    }
    printf("%-28s %10s %10s %14s\n", "file", "time(ms)", "tokens", "tokens/sec");
    double total_ms = 0;
    long total_tokens = 0;
    for (int i = 0; i < srcs.count; i++) {
        Source src;
        if (source_load(&src, srcs.paths[i]) != 0) continue;
        Diag d;
        diag_init(&d, 1000000);
        TokenVec tv;
        double t0 = now_ms();
        lexer_run(&src, &tv, &d);
        if (diag_error_count(&d) == 0)
            parser_run(&tv, &d, srcs.paths[i]);
        double elapsed = now_ms() - t0;
        const char *base2 = strrchr(srcs.paths[i], '/');
        base2 = base2 ? base2 + 1 : srcs.paths[i];
        double rate = elapsed > 0 ? tv.count / (elapsed / 1000.0) : 0;
        printf("%-28s %10.2f %10d %14.0f\n", base2, elapsed, tv.count, rate);
        total_ms += elapsed;
        total_tokens += tv.count;
        token_vec_free(&tv);
        diag_free(&d);
        source_free(&src);
    }
    printf("%-28s %10.2f %10ld %14.0f\n", "total", total_ms, total_tokens,
           total_ms > 0 ? total_tokens / (total_ms / 1000.0) : 0);
    return 0;
}

/* Shared implementation of state/shared: full analysis, then a ustar
 * archive of every per-file AST dump plus the build report. */
static int emit_archive(const Cli *cli, const char *ext) {
    const char *root = cli->path ? cli->path : ".";
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: no configuration file in '%s'\n", root);
        return 2;
    }
    char name[128];
    if (!toml_get(cfg, "project", "name", name, sizeof(name))) {
        const char *base2 = strrchr(cfg, '/');
        base2 = base2 ? base2 + 1 : cfg;
        snprintf(name, sizeof(name), "%.*s", (int)(strlen(base2) - 5), base2);
    }
    SrcList srcs;
    memset(&srcs, 0, sizeof(srcs));
    char srcdir[PAXSY_PATH_MAX];
    snprintf(srcdir, sizeof(srcdir), "%s/src", root);
    collect_dir(srcdir, 0, &srcs);
    if (srcs.count == 0) {
        fprintf(stderr, "paxsy: no .px sources under '%s/src'\n", root);
        return 1;
    }
    char bdir[PAXSY_PATH_MAX], tdir[PAXSY_PATH_MAX];
    snprintf(bdir, sizeof(bdir), "%s/build", root);
    snprintf(tdir, sizeof(tdir), "%s/target", root);
    if (!is_dir(bdir)) mkdir(bdir, 0755);
    if (!is_dir(tdir)) mkdir(tdir, 0755);

    /* 1200 bytes per member: build dir (<=511) + stem (<=127) + suffix. */
    char members[MAX_SRCS + 1][1200];
    const char *ptrs[MAX_SRCS + 1];
    int nmembers = 0;
    int failed = 0;
    for (int i = 0; i < srcs.count; i++) {
        Diag d;
        diag_init(&d, cli->max_errors);
        Analysis a;
        if (pipeline_analyze(srcs.paths[i], cli, &d, &a) != 0) failed++;
        if (a.ast) {
            const char *base2 = strrchr(srcs.paths[i], '/');
            base2 = base2 ? base2 + 1 : srcs.paths[i];
            char stem[128];
            snprintf(stem, sizeof(stem), "%.*s", (int)(strlen(base2) - 3), base2);
            snprintf(members[nmembers], sizeof(members[0]),
                     "%s/%s.ast", bdir, stem);
            FILE *f = fopen(members[nmembers], "w");
            if (f) { dump_ast(a.ast, f, 0); fclose(f); ptrs[nmembers] = members[nmembers]; nmembers++; }
        }
        analysis_release(&a);
        diag_free(&d);
    }
    snprintf(members[nmembers], sizeof(members[0]), "%s/report.txt", bdir);
    FILE *r = fopen(members[nmembers], "w");
    if (r) {
        fprintf(r, "paxsy frontend archive %s\nsources: %d\n", name, srcs.count);
        fclose(r);
        ptrs[nmembers] = members[nmembers];
        nmembers++;
    }
    if (failed) {
        fprintf(stderr, "paxsy: archive skipped, %d file(s) failed\n", failed);
        return 1;
    }
    char archive[1200];
    snprintf(archive, sizeof(archive), "%s/%s%s", tdir, name, ext);
    if (tar_create(archive, ptrs, nmembers) != 0) {
        fprintf(stderr, "paxsy: cannot write '%s'\n", archive);
        return 1;
    }
    printf("wrote %s (%d members)\n", archive, nmembers);
    return 0;
}

int cmd_state(const Cli *cli)  { return emit_archive(cli, ".a"); }
int cmd_shared(const Cli *cli) { return emit_archive(cli, ".sfa"); }
