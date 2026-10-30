#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "pipeline.h"
#include "toml.h"

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

/*
 * cmd_build.c - the core build commands:
 *   build / check  run the frontend over the project's sources;
 *   run            execute the compiled program when one exists.
 * A successful build is silent unless --verbose is given: diagnostics
 * speak for themselves and the exit status is the success signal.
 * Source discovery honours the recursive flag, which the command line
 * may force on top of the configuration file.
 */

#define MAX_SRCS 64
typedef struct { char paths[MAX_SRCS][PAXSY_PATH_MAX]; int count; } SrcList;

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Appends all matching .px files of one directory level. */
static void collect_dir(const char *dir, int recursive, int tests_only,
                        SrcList *list) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && list->count < MAX_SRCS) {
        if (e->d_name[0] == '.') continue;
        char path[PAXSY_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (recursive) collect_dir(path, recursive, tests_only, list);
            continue;
        }
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

static void collect_sources(const char *root, int recursive, int tests_only,
                            SrcList *list) {
    char src[PAXSY_PATH_MAX];
    snprintf(src, sizeof(src), "%s/src", root);
    memset(list, 0, sizeof(*list));
    collect_dir(src, recursive, tests_only, list);
}

/* Runs the analysis over every source, accumulating statistics.
 * Returns the number of failed files. */
static int analyze_sources(const Cli *cli, const SrcList *srcs,
                           long *tokens, long *nodes, int *warnings) {
    int failed = 0;
    *tokens = *nodes = 0;
    *warnings = 0;
    for (int i = 0; i < srcs->count; i++) {
        Diag d;
        diag_init(&d, cli->max_errors);
        d.suppress_warnings = cli->ignore_warnings;
        if (cli->verbose) printf("checking %s\n", srcs->paths[i]);
        Analysis a;
        int rc = pipeline_analyze(srcs->paths[i], cli, &d, &a);
        if (cli->werror && diag_warning_count(&d) > 0) rc = 1;
        *tokens += a.tokens.count;
        *nodes += a.nodes;
        *warnings += diag_warning_count(&d);
        if (rc) failed++;
        analysis_release(&a);
        diag_free(&d);
    }
    return failed;
}

static int write_report(const char *root, const SrcList *srcs,
                        long tokens, long nodes, int failed) {
    char path[PAXSY_PATH_MAX];
    snprintf(path, sizeof(path), "%s/build/report.txt", root);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "paxsy frontend build report\n");
    fprintf(f, "sources: %d  tokens: %ld  ast nodes: %ld  "
               "failed files: %d\n", srcs->count, tokens, nodes, failed);
    fclose(f);
    return 0;
}

static int build_common(const Cli *cli, int write_artifacts) {
    const char *root = cli->path ? cli->path : ".";
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: no configuration file in '%s'\n", root);
        return 2;
    }
    int recursive = cli->recursive;
    if (!recursive) {
        char v[16];
        if (toml_get(cfg, "build", "recursive", v, sizeof(v)) &&
            strcmp(v, "true") == 0)
            recursive = 1;
    }
    SrcList srcs;
    collect_sources(root, recursive, 0, &srcs);
    if (srcs.count == 0) {
        fprintf(stderr, "paxsy: no .px sources under '%s/src'\n", root);
        return 1;
    }
    if (write_artifacts) {
        char bd[PAXSY_PATH_MAX];
        snprintf(bd, sizeof(bd), "%s/build", root);
        if (!is_dir(bd)) mkdir(bd, 0755);
    }
    long tokens, nodes;
    int warnings;
    int failed = analyze_sources(cli, &srcs, &tokens, &nodes, &warnings);
    (void)tokens; (void)nodes; (void)warnings;
    if (write_artifacts && !failed)
        write_report(root, &srcs, tokens, nodes, failed);
    return failed ? 1 : 0;
}

int cmd_build(const Cli *cli) { return build_common(cli, 1); }
int cmd_check(const Cli *cli) { return build_common(cli, 0); }

int cmd_run(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "paxsy: no configuration file in '%s'\n", root);
        return 2;
    }
    char name[128];
    if (!toml_get(cfg, "project", "name", name, sizeof(name))) {
        const char *base = strrchr(cfg, '/');
        base = base ? base + 1 : cfg;
        snprintf(name, sizeof(name), "%.*s", (int)(strlen(base) - 5), base);
    }
    char bin[PAXSY_PATH_MAX];
    snprintf(bin, sizeof(bin), "%s/target/%s", root, name);
    if (access(bin, X_OK) != 0) {
        fprintf(stderr, "paxsy: '%s' does not exist or is not executable; "
                        "build the project with the code generation "
                        "backend first\n", bin);
        return 1;
    }
    const char *av[4];
    int ac = 0;
    av[ac++] = bin;
    if (cli->arg) av[ac++] = cli->arg;
    if (cli->value) av[ac++] = cli->value;
    av[ac] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        execv(bin, (char *const *)av);
        fprintf(stderr, "paxsy: cannot execute '%s': %s\n", bin, strerror(errno));
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}
