#define _POSIX_C_SOURCE 200809L
#include "common.h"

/* Some strict-C99 environments hide readlink even with the feature
 * macro above; the declaration is harmless where unistd provides it. */
extern ssize_t readlink(const char *path, char *buf, size_t bufsiz);
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "pipeline.h"
#include "srcdir.h"
#include "toml.h"
#include "autocommit.h"
#include "output_path.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

/*
 * cli/build.c - project build, type-check, and run commands.
 *
 * Build and check discover every source file in stable path order.
 * The frontend parses each file independently, then runs semantic
 * analysis across project sources as one namespace while preserving
 * package-local import scopes. Lowering and HIR type inference operate
 * over the resulting project module set. Build proceeds to emission;
 * check stops before writing artifacts.
 *
 * Both commands are silent on success. Build freshness uses a content
 * stamp over the compiler, configuration, source files, target, and
 * optimisation state. The primary output path comes from build.output.
 */

/* Defined in cmd_compile.c; declared here so build_common can hand the
 * collected sources to the code generation driver. */
int compile_executable(const Cli *cli, const char *root,
                       const char *cfg, SrcList *srcs);
int check_types(const Cli *cli, const SrcList *srcs);

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


static int config_true(const char *cfg, const char *section, const char *key) {
    char value[32];
    return toml_get(cfg, section, key, value, sizeof(value)) &&
           strcmp(value, "true") == 0;
}

static int apply_quality_config(Cli *options, const char *cfg) {
    char value[32];
    if (toml_get(cfg, "quality", "max_errors", value, sizeof(value))) {
        char *end = NULL;
        long n = strtol(value, &end, 10);
        if (end == value || !end || *end || n < 1 || n > 10000) {
            fprintf(stderr, "invalid [quality].max_errors (expected 1..10000)\n");
            return -1;
        }
        options->max_errors = (int)n;
    }
    options->wextra = config_true(cfg, "quality", "extra_warnings");
    options->werror = config_true(cfg, "quality", "warnings_as_errors");
    options->ignore_warnings = config_true(cfg, "quality", "ignore_warnings");
    return 0;
}

static void apply_artifact_config(Cli *options, const char *cfg) {
    char target[32] = "elf";
    options->emit_obj = config_true(cfg, "artifacts", "object");
    options->emit_a = config_true(cfg, "artifacts", "archive");
    options->emit_pe = config_true(cfg, "artifacts", "pe");
    options->emit_llvm = config_true(cfg, "artifacts", "llvm");
    options->emit_mlir = config_true(cfg, "artifacts", "mlir");
    options->emit_cuda = config_true(cfg, "artifacts", "cuda");
    if (toml_get(cfg, "build", "target", target, sizeof(target))) {
        if (strcmp(target, "obj") == 0 || strcmp(target, "o") == 0)
            options->emit_obj = 1;
        else if (strcmp(target, "a") == 0 || strcmp(target, "static") == 0 ||
                 strcmp(target, "lib") == 0) {
            options->emit_obj = 1;
            options->emit_a = 1;
        } else if (strcmp(target, "llvm") == 0) options->emit_llvm = 1;
        else if (strcmp(target, "mlir") == 0) options->emit_mlir = 1;
        else if (strcmp(target, "cuda") == 0) options->emit_cuda = 1;
        else if (strcmp(target, "pe") == 0 ||
                 strcmp(target, "windows-x86_64") == 0 ||
                 strcmp(target, "x86_64-windows-gnu") == 0)
            options->emit_pe = 1;
    }
}

/* Incremental rebuild -------------------------------------------------
 * A build is fresh when the configured primary output exists and its
 * content stamp matches the compiler, configuration, sources, target,
 * and optimisation state. Textual artifacts are regenerated when selected
 * in the project TOML. */

/* Returns the newest modification time over the collected sources and
 * the running compiler binary, or (time_t)-1 on failure. */

/* Reads the optimisation stamp next to the artifacts and returns its
 * byte ('1' with PAXSY_NO_OPT, '0' with the optimiser enabled), or
 * -1 when the stamp is missing or unreadable. A missing stamp marks
 * the tree stale so the first build after an upgrade regenerates
 * everything. */

/* True when the configured output exists and the input-content stamp
 * matches the current build configuration. */
/* FNV-1a over the bytes of one file; returns 0 when unreadable. */
#include <stdint.h>

static uint64_t fnv_file(const char *path, uint64_t h) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        size_t i;
        for (i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
    }
    fclose(f);
    return h ? h : 1;
}

/* Content hash of every build input: the configuration, every source
 * file, the optimisation A/B stamp and the compiler executable itself.
 * mtimes are not trustworthy (one-second granularity lets two edits in
 * the same second look fresh), contents are. */
static uint64_t inputs_hash(const Cli *cli, const char *cfg,
                            const SrcList *srcs) {
    uint64_t h = 1469598103934665603ULL;
    char exe[PAXSY_PATH_MAX];
    ssize_t el = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    int i;
    if (el > 0) {
        exe[el] = 0;
        h = fnv_file(exe, h);
    }
    h = fnv_file(cfg, h);
    for (i = 0; i < srcs->count; i++)
        h = fnv_file(srcs->paths[i], h);
    /* Command-line target selection is part of the build identity. */
    for (const char *p = cli->target; p && *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    h ^= (uint64_t)(cli->emit_obj != 0) << 1;
    h ^= (uint64_t)(cli->emit_a != 0) << 2;
    h ^= (uint64_t)(cli->emit_pe != 0) << 3;
    h ^= (uint64_t)(cli->emit_llvm != 0) << 4;
    h ^= (uint64_t)(cli->emit_mlir != 0) << 5;
    h ^= (uint64_t)(cli->emit_cuda != 0) << 6;
    if (getenv("PAXSY_NO_OPT"))
        h ^= 0x243F6A8885A308D3ULL;   /* optimisation A/B switch */
    return h ? h : 1;
}

/* Reads the stamp written after the last successful build. */
static uint64_t build_stamp_read(const char *root) {
    char p[PAXSY_PATH_MAX];
    snprintf(p, sizeof(p), "%s/build/.paxsy-hash", root);
    FILE *f = fopen(p, "r");
    unsigned long long v = 0;
    if (!f) return 0;
    if (fscanf(f, "%llx", &v) != 1) v = 0;
    fclose(f);
    return (uint64_t)v;
}

static void build_stamp_write(const char *root, uint64_t h) {
    char p[PAXSY_PATH_MAX];
    snprintf(p, sizeof(p), "%s/build/.paxsy-hash", root);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%llx\n", (unsigned long long)h);
    fclose(f);
}

static int build_is_fresh(const Cli *cli, const char *root,
                          const char *cfg, const SrcList *srcs) {
    /* Profiling must execute the pipeline even when the output stamp is
     * fresh; otherwise the command would report no stage measurements. */
    if (cli->profile) return 0;
    if (cli->emit_cuda || cli->emit_mlir || cli->emit_llvm) return 0;
    char nm[128];
    char target[64] = "";
    int pe_target;
    project_name(cfg, nm, sizeof(nm));
    if (cli->target[0])
        snprintf(target, sizeof(target), "%s", cli->target);
    else
        toml_get(cfg, "build", "target", target, sizeof(target));
    pe_target = strcmp(target, "pe") == 0 ||
                strcmp(target, "windows-x86_64") == 0 ||
                strcmp(target, "x86_64-windows-gnu") == 0;
    if (strcmp(target, "llvm") == 0 || strcmp(target, "mlir") == 0 ||
        strcmp(target, "cuda") == 0)
        return 0;
    char bin[PAXSY_PATH_MAX];
    if (paxsy_output_path(root, cfg, nm, pe_target,
                          bin, sizeof(bin)) != 0) return 0;
    struct stat st;
    if (stat(bin, &st) != 0) return 0;
    if (pe_target || cli->emit_obj || cli->emit_a || cli->emit_pe ||
        strcmp(target, "obj") == 0 || strcmp(target, "a") == 0 ||
        strcmp(target, "static") == 0) {
        char object[PAXSY_PATH_MAX];
        snprintf(object, sizeof(object), "%s/build/bin/%s.o", root, nm);
        if (stat(object, &st) != 0) return 0;
    }
    if (cli->emit_a || strcmp(target, "a") == 0 ||
        strcmp(target, "static") == 0) {
        char archive[PAXSY_PATH_MAX];
        snprintf(archive, sizeof(archive), "%s/build/artifacts/%s.a", root, nm);
        if (stat(archive, &st) != 0) return 0;
    }
    if (!pe_target && cli->emit_pe) {
        char pe[PAXSY_PATH_MAX];
        snprintf(pe, sizeof(pe), "%s/build/%s.exe", root, nm);
        if (stat(pe, &st) != 0) return 0;
    }
    /* Freshness is a content property, not a timestamp property. */
    return build_stamp_read(root) == inputs_hash(cli, cfg, srcs);
}

static int build_common(const Cli *cli, int write_artifacts) {
    const char *root = cli->path ? cli->path : ".";
    char cfg[PAXSY_PATH_MAX];
    Cli options = *cli;
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "no configuration file in '%s'\n", root);
        return 2;
    }
    if (apply_quality_config(&options, cfg) != 0) return 2;
    apply_artifact_config(&options, cfg);
    /* The debugger's internal `exe` target must stay native even when
     * the project normally emits PE as a secondary artifact. */
    if (strcmp(options.target, "exe") == 0) options.emit_pe = 0;
    int recursive;
    char recursive_value[32];
    if (toml_get(cfg, "source", "recursive", recursive_value,
                 sizeof(recursive_value)))
        recursive = strcmp(recursive_value, "true") == 0;
    else
        recursive = config_true(cfg, "build", "recursive"); /* legacy */
    options.recursive = recursive;
    /* Zero-initialised: srclist_push grows the array from count/cap,
     * so uninitialised storage would send realloc() to a wild
     * pointer. */
    SrcList srcs = {0};
    if (srcdir_collect(root, recursive, 0, &srcs) < 0) {
        fprintf(stderr, "source discovery failed under '%s'\n", root);
        return 1;
    }
    if (srcs.count == 0) {
        srclist_free(&srcs);
        fprintf(stderr, "no .px sources under '%s/src'\n", root);
        return 1;
    }
    if (write_artifacts && build_is_fresh(&options, root, cfg, &srcs)) {
        srclist_free(&srcs);
        return 0;
    }
    if (write_artifacts) {
        char bd[PAXSY_PATH_MAX];
        snprintf(bd, sizeof(bd), "%s/build", root);
        if (!is_dir(bd)) mkdir(bd, 0755);
    }
    int failed = write_artifacts ? 0 : check_types(&options, &srcs);
    if (failed < 0) {
        fprintf(stderr, "out of memory during analysis\n");
        srclist_free(&srcs);
        return 2;
    }
    if (!failed && write_artifacts) {
        failed = compile_executable(&options, root, cfg, &srcs) ? 1 : 0;
        if (!failed) {
            build_stamp_write(root, inputs_hash(cli, cfg, &srcs));
            paxsy_autocommit(&options, root);
        }
    }
    srclist_free(&srcs);
    return failed ? 1 : 0;
}

int cmd_build(const Cli *cli) { return build_common(cli, 1); }
int cmd_check(const Cli *cli) { return build_common(cli, 0); }

/* Parse a TOML array of quoted strings. The project TOML reader keeps
 * array syntax intact; this small parser deliberately accepts only the
 * string-array subset needed by run profiles and rejects malformed input. */
static int run_parse_array(const char *raw, char **out, int cap) {
    const char *p = raw;
    int n = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p++ != '[') return -1;
    for (;;) {
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (*p == ']') { p++; break; }
        if (*p++ != '"' || n >= cap) goto bad;
        char value[2048]; size_t len = 0;
        while (*p && *p != '"') {
            unsigned char c = (unsigned char)*p++;
            if (c == '\\') {
                c = (unsigned char)*p++;
                if (!c) goto bad;
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                else if (c != '"' && c != '\\') goto bad;
            }
            if (len + 1 >= sizeof(value)) goto bad;
            value[len++] = (char)c;
        }
        if (*p++ != '"') goto bad;
        value[len] = '\0';
        out[n] = strdup(value);
        if (!out[n]) goto bad;
        n++;
        while (isspace((unsigned char)*p)) p++;
        if (*p == ']') { p++; break; }
        if (*p++ != ',') goto bad;
    }
    while (isspace((unsigned char)*p)) p++;
    if (*p) goto bad;
    return n;
bad:
    for (int i = 0; i < n; i++) free(out[i]);
    return -1;
}

static void run_free_array(char **items, int count) {
    for (int i = 0; i < count; i++) free(items[i]);
}

static char *run_expand(const char *input, const char *binary,
                        const char *root, const char *project) {
    size_t cap = strlen(input) + strlen(binary) + strlen(root) +
                 strlen(project) + 32;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t used = 0;
    for (const char *p = input; *p;) {
        const char *value = NULL; size_t token = 0;
        if (strncmp(p, "{binary}", 8) == 0) { value = binary; token = 8; }
        else if (strncmp(p, "{root}", 6) == 0) { value = root; token = 6; }
        else if (strncmp(p, "{project}", 9) == 0) { value = project; token = 9; }
        if (value) {
            size_t n = strlen(value);
            if (used + n + 1 > cap) {
                cap = (used + n + 1) * 2;
                char *grown = realloc(out, cap);
                if (!grown) { free(out); return NULL; }
                out = grown;
            }
            memcpy(out + used, value, n); used += n; p += token;
        } else {
            if (used + 2 > cap) {
                cap *= 2;
                char *grown = realloc(out, cap);
                if (!grown) { free(out); return NULL; }
                out = grown;
            }
            out[used++] = *p++;
        }
    }
    out[used] = '\0';
    return out;
}

int cmd_run(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    char cfg[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) != 0) {
        fprintf(stderr, "no configuration file in '%s'\n", root);
        return 2;
    }
    char nm[128], bin[PAXSY_PATH_MAX], target[64] = "";
    project_name(cfg, nm, sizeof(nm));
    toml_get(cfg, "build", "target", target, sizeof(target));
    int pe = strcmp(target, "pe") == 0 ||
             strcmp(target, "windows-x86_64") == 0 ||
             strcmp(target, "x86_64-windows-gnu") == 0;
    if (paxsy_output_path(root, cfg, nm, pe, bin, sizeof(bin)) != 0) {
        fprintf(stderr, "configured build.output path is too long\n");
        return 2;
    }

    char profile[128] = "native";
    if (cli->run_profile) snprintf(profile, sizeof(profile), "%s", cli->run_profile);
    else toml_get(cfg, "run", "profile", profile, sizeof(profile));
    for (const char *p = profile; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) {
            fprintf(stderr, "invalid run profile name '%s'\n", profile);
            return 2;
        }
    }

    /* Program arguments are appended after profile arguments. */
    int extra = cli->rest_count;
    const char **forward = cli->rest;
    if (extra == 0) {
        static const char *legacy[2]; int n = 0;
        if (cli->arg) legacy[n++] = cli->arg;
        if (cli->value) legacy[n++] = cli->value;
        extra = n; forward = legacy;
    }

    if (strcmp(profile, "native") == 0) {
        if (access(bin, X_OK) != 0) {
            fprintf(stderr, "'%s' does not exist or is not executable; build the project first\n", bin);
            return 1;
        }
        char **argv = calloc((size_t)extra + 2, sizeof(char *));
        if (!argv) return 2;
        argv[0] = bin;
        for (int i = 0; i < extra; i++) argv[i + 1] = (char *)forward[i];
        pid_t pid = fork();
        if (pid == 0) { execv(bin, argv); fprintf(stderr, "cannot execute '%s': %s\n", bin, strerror(errno)); _exit(127); }
        free(argv);
        if (pid < 0) { fprintf(stderr, "failed to run '%s'\n", bin); return 2; }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) return 2;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }

    char section[160], command[512], cwd[2048], raw[8192];
    snprintf(section, sizeof(section), "run.%s", profile);
    if (!toml_get(cfg, section, "command", command, sizeof(command)) || !command[0]) {
        fprintf(stderr, "run profile '%s' requires [%s].command\n", profile, section);
        return 2;
    }
    char *items[128] = {0};
    int item_count = 0;
    if (toml_get(cfg, section, "args", raw, sizeof(raw))) {
        item_count = run_parse_array(raw, items, 128);
        if (item_count < 0) {
            fprintf(stderr, "[%s].args must be a valid array of strings\n", section);
            return 2;
        }
    }
    char *env_items[64] = {0}; int env_count = 0;
    if (toml_get(cfg, section, "env", raw, sizeof(raw))) {
        env_count = run_parse_array(raw, env_items, 64);
        if (env_count < 0) {
            run_free_array(items, item_count);
            fprintf(stderr, "[%s].env must be a valid array of strings\n", section);
            return 2;
        }
    }
    if (item_count + extra + 2 > 256) {
        run_free_array(items, item_count); run_free_array(env_items, env_count);
        fprintf(stderr, "too many run profile arguments (maximum 254)\n"); return 2;
    }
    char *expanded_command = run_expand(command, bin, root, nm);
    char **argv = calloc((size_t)item_count + (size_t)extra + 2, sizeof(char *));
    if (!expanded_command || !argv) {
        free(expanded_command); free(argv); run_free_array(items, item_count); run_free_array(env_items, env_count); return 2;
    }
    argv[0] = expanded_command;
    int argc = 1;
    for (int i = 0; i < item_count; i++) {
        if (strcmp(items[i], "{args}") == 0) {
            for (int j = 0; j < extra; j++) {
                argv[argc] = strdup(forward[j]);
                if (!argv[argc++]) { fprintf(stderr, "out of memory forwarding run arguments\n"); return 2; }
            }
        } else {
            argv[argc] = run_expand(items[i], bin, root, nm);
            if (!argv[argc++]) { fprintf(stderr, "out of memory expanding run profile\n"); return 2; }
        }
    }
    if (!item_count) {
        for (int i = 0; i < extra; i++) {
            argv[argc] = strdup(forward[i]);
            if (!argv[argc++]) { fprintf(stderr, "out of memory forwarding run arguments\n"); return 2; }
        }
    } else {
        int has_args = 0;
        for (int i = 0; i < item_count; i++) if (strcmp(items[i], "{args}") == 0) has_args = 1;
        if (!has_args) for (int i = 0; i < extra; i++) {
            argv[argc] = strdup(forward[i]);
            if (!argv[argc++]) { fprintf(stderr, "out of memory forwarding run arguments\n"); return 2; }
        }
    }
    argv[argc] = NULL;
    cwd[0] = '\0'; toml_get(cfg, section, "working_dir", cwd, sizeof(cwd));
    char *expanded_cwd = run_expand(cwd[0] ? cwd : root, bin, root, nm);
    if (!expanded_cwd) {
        fprintf(stderr, "out of memory expanding run working directory\n");
        for (int i = 0; i < argc; i++) free(argv[i]);
        free(argv); run_free_array(items, item_count); run_free_array(env_items, env_count);
        return 2;
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (expanded_cwd && chdir(expanded_cwd) != 0) { fprintf(stderr, "cannot change run directory to '%s': %s\n", expanded_cwd, strerror(errno)); _exit(126); }
        for (int i = 0; i < env_count; i++) {
            char *entry = run_expand(env_items[i], bin, root, nm);
            char *eq = entry ? strchr(entry, '=') : NULL;
            if (!eq || eq == entry) { fprintf(stderr, "invalid [%s].env entry (expected KEY=VALUE)\n", section); _exit(126); }
            *eq++ = '\0'; if (setenv(entry, eq, 1) != 0) _exit(126);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "cannot execute run profile command '%s': %s\n", argv[0], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) < 0) status = -1;
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv); free(expanded_cwd); run_free_array(items, item_count); run_free_array(env_items, env_count);
    if (status < 0) { fprintf(stderr, "failed to start run profile '%s'\n", profile); return 2; }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
