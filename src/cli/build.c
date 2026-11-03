#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "pipeline.h"
#include "srcdir.h"
#include "toml.h"
#include "autocommit.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

/*
 * cli/build.c - the project-wide analysis commands and the runner.
 *
 *   build   runs the frontend over every source of the project and,
 *           when everything analyses cleanly, hands the typed HIR to
 *           the code generation driver in cmd_compile.c;
 *           before doing any work it checks whether the existing
 *           binary is still fresh (newer than every source and than
 *           the compiler itself) and skips the whole pipeline when it
 *           is, printing a one-line "up to date" notice;
 *   check   runs exactly the same analysis but stops before code
 *           generation, which makes it the fast semantic gate used by
 *           editors and CI; it never writes a binary, so the freshness
 *           check never applies to it;
 *   run     executes the binary produced by build, expected at
 *           build/<name> below the project root, forwarding the
 *           operands after the project path as program arguments.
 *
 * build and check are silent on success: diagnostics carry the
 * details and the exit status is the success signal. Source discovery
 * lives in srcdir so build, clippy, bench, test and the archive
 * emitters all see the same stable file order. bench lives in
 * cmd_artifacts.c with the other artifact commands.
 */

/* Defined in cmd_compile.c; declared here so build_common can hand the
 * collected sources to the code generation driver. */
int compile_executable(const Cli *cli, const char *root,
                       const char *cfg, SrcList *srcs);

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Derives the project name: the [project] table wins, and the
 * configuration file base name (minus ".toml") is the fallback; the
 * strip is length-guarded so short names cannot wrap around. */
static void project_name(const char *cfg, char *nm, size_t cap) {
    if (toml_get(cfg, "project", "name", nm, cap)) return;
    const char *base = strrchr(cfg, '/');
    base = base ? base + 1 : cfg;
    size_t blen = strlen(base);
    snprintf(nm, cap, "%.*s",
             blen > 5 ? (int)(blen - 5) : (int)blen, base);
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

/* Incremental rebuild -------------------------------------------------
 * A build is fresh when the binary at build/<name> exists and is
 * newer than every input (every source file and the compiler itself;
 * a toolchain upgrade rebuilds everything) and the optimisation stamp
 * matches the current PAXSY_NO_OPT setting (toggling the flag without
 * source edits still rebuilds). Explicit emission requests (--emit-*)
 * always regenerate their artifacts and never skip. */

/* Returns the newest modification time over the collected sources and
 * the running compiler binary, or (time_t)-1 on failure. */
static time_t inputs_mtime(const SrcList *srcs) {
    time_t newest = 0;
    for (int i = 0; i < srcs->count; i++) {
        struct stat st;
        if (stat(srcs->paths[i], &st) != 0) return (time_t)-1;
        if (st.st_mtime > newest) newest = st.st_mtime;
    }
    struct stat self;
    if (stat("/proc/self/exe", &self) == 0 && self.st_mtime > newest)
        newest = self.st_mtime;
    return newest;
}

/* Reads the optimisation stamp next to the artifacts and returns its
 * byte ('1' with PAXSY_NO_OPT, '0' with the optimiser enabled), or
 * -1 when the stamp is missing or unreadable. A missing stamp marks
 * the tree stale so the first build after an upgrade regenerates
 * everything. */
static int opt_stamp_read(const char *root, const char *name) {
    char stamp[PAXSY_PATH_MAX];
    snprintf(stamp, sizeof(stamp), "%s/build/bin/%s.opt", root, name);
    FILE *sf = fopen(stamp, "rb");
    if (!sf) return -1;
    int ch = fgetc(sf);
    fclose(sf);
    if (ch != '0' && ch != '1') return -1;
    return ch;
}

/* True when build/<name> exists and is at least as new as every
 * input and the optimisation stamp matches the current setting. */
static int build_is_fresh(const Cli *cli, const char *root,
                          const char *cfg, const SrcList *srcs) {
    if (cli->emit_cuda || cli->emit_mlir || cli->emit_llvm) return 0;
    char nm[128];
    project_name(cfg, nm, sizeof(nm));
    char bin[PAXSY_PATH_MAX];
    snprintf(bin, sizeof(bin), "%s/build/%s", root, nm);
    struct stat st;
    if (stat(bin, &st) != 0) return 0;
    time_t inputs = inputs_mtime(srcs);
    if (inputs == (time_t)-1) return 0;
    /* The optimisation stamp must match the current PAXSY_NO_OPT
     * setting: toggling the flag without editing any source still
     * rebuilds, otherwise the second pass of an A/B optimisation
     * comparison would reuse the first pass's artifacts. */
    int stamp = opt_stamp_read(root, nm);
    if (stamp != (getenv("PAXSY_NO_OPT") ? '1' : '0')) return 0;
    return st.st_mtime >= inputs;
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
    /* Zero-initialised: srclist_push grows the array from count/cap,
     * so uninitialised storage would send realloc() to a wild
     * pointer. */
    SrcList srcs = {0};
    if (srcdir_collect(root, recursive, 0, &srcs) < 0) {
        fprintf(stderr, "paxsy: source discovery failed under '%s'\n", root);
        return 1;
    }
    if (srcs.count == 0) {
        srclist_free(&srcs);
        fprintf(stderr, "paxsy: no .px sources under '%s/src'\n", root);
        return 1;
    }
    if (write_artifacts && build_is_fresh(cli, root, cfg, &srcs)) {
        if (!cli->quiet)
            printf("paxsy: build is up to date\n");
        srclist_free(&srcs);
        return 0;
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
    if (!failed && write_artifacts) {
        failed = compile_executable(cli, root, cfg, &srcs) ? 1 : 0;
        if (!failed)
            paxsy_autocommit(cli, root);
    }
    srclist_free(&srcs);
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
    char nm[128];
    project_name(cfg, nm, sizeof(nm));
    char bin[PAXSY_PATH_MAX];
    snprintf(bin, sizeof(bin), "%s/build/%s", root, nm);
    if (access(bin, X_OK) != 0) {
        fprintf(stderr, "paxsy: '%s' does not exist or is not executable; "
                        "build the project first\n", bin);
        return 1;
    }
    /* Forward every operand after the project path to the program;
     * the legacy single-value slots are kept as positional fallbacks
     * for `run <path> <arg>` invocations. */
    int extra = cli->rest_count;
    const char **args = cli->rest;
    if (extra == 0) {
        static const char *legacy[2];
        int n = 0;
        if (cli->arg) legacy[n++] = cli->arg;
        if (cli->value) legacy[n++] = cli->value;
        extra = n;
        args = legacy;
    }
    char **argv = malloc((size_t)(extra + 2) * sizeof(char *));
    if (!argv) return 2;
    argv[0] = bin;
    for (int i = 0; i < extra; i++) argv[1 + i] = (char *)args[i];
    argv[extra + 1] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        execv(bin, argv);
        fprintf(stderr, "paxsy: cannot execute '%s': %s\n",
                bin, strerror(errno));
        _exit(127);
    }
    free(argv);
    if (pid < 0) {
        fprintf(stderr, "paxsy: failed to run '%s'\n", bin);
        return 2;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "paxsy: failed to run '%s'\n", bin);
        return 2;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}
