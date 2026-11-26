#include "common.h"
#include "cli.h"
#include "toml.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

/*
 * cli/autocommit.c - the post-build git snapshot.
 * After a successful `paxsy build`, an explicitly enabled project
 * policy may commit changes from Paxsy-managed project paths. The
 * operation never absorbs unrelated staged work or arbitrary files.
 * It is best-effort and never changes the build result.
 */

/* Executes argv in `root`, capturing up to cap-1 bytes of stdout into
 * `capture` (NUL-terminated) when capture is non-NULL. stdin and
 * stderr go to /dev/null so a missing git, a hook failure or a
 * credential prompt can never leak noise into the build output or
 * block on input. Returns the exit status, or -1 when the child could
 * not be started. */
static int git_capture(const char *root, char *const argv[],
                       char *capture, size_t cap) {
    int fds[2] = { -1, -1 };
    if (capture && pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, STDIN_FILENO);
            if (!capture) dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > STDERR_FILENO) close(dn);
        }
        if (capture) {
            close(fds[0]);
            dup2(fds[1], STDOUT_FILENO);
            close(fds[1]);
        }
        if (chdir(root) != 0) _exit(127);
        execvp(argv[0], argv);
        _exit(127);            /* git not installed: silent no-op */
    }
    if (pid < 0) {
        if (capture) { close(fds[0]); close(fds[1]); }
        return -1;
    }
    int status = 0;
    if (capture) {
        size_t used = 0;
        ssize_t n;
        close(fds[1]);
        {
            char discard[512];
            while ((n = read(fds[0], used + 1 < cap
                             ? capture + used : discard,
                             used + 1 < cap ? cap - 1 - used
                                           : sizeof(discard))) > 0) {
                if (used + 1 < cap) used += (size_t)n;
            }
        }
        if (capture) capture[used] = '\0';
        close(fds[0]);
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int git_rc(const char *root, char *const argv[]) {
    return git_capture(root, argv, NULL, 0);
}

static int project_path_exists(const char *root, const char *name) {
    char path[PAXSY_PATH_MAX];
    struct stat st;
    int n = snprintf(path, sizeof(path), "%s/%s", root, name);
    return n > 0 && (size_t)n < sizeof(path) && stat(path, &st) == 0;
}

static int git_include(const char *cfg, const char *key, int fallback) {
    char value[16];
    if (!toml_get(cfg, "git", key, value, sizeof(value))) return fallback;
    return strcmp(value, "true") == 0;
}

/* Reads a boolean policy. Missing keys use the supplied safe default. */
static int git_policy(const char *cfg, const char *key, int fallback) {
    char value[32];
    if (!toml_get(cfg, "git", key, value, sizeof(value))) return fallback;
    return strcmp(value, "true") == 0;
}

static void git_string(const char *cfg, const char *key,
                       char *out, size_t cap, const char *fallback) {
    if (!toml_get(cfg, "git", key, out, (int)cap) || !out[0])
        snprintf(out, cap, "%s", fallback);
}

static int git_output(const char *root, char *const argv[],
                      char *out, size_t cap) {
    if (cap) out[0] = '\0';
    return git_capture(root, argv, out, cap);
}

/* A project branch is created only from the repository's initial/default
 * branch. Existing branches are never checked out or rewritten implicitly. */
static void ensure_work_branch(const Cli *cli, const char *root,
                               const char *cfg) {
    char current[256], wanted[256], project[128];
    char *branch[] = { "git", "symbolic-ref", "--quiet", "--short", "HEAD", NULL };
    char *exists[] = { "git", "show-ref", "--verify", "--quiet", NULL, NULL };
    char *check_name[] = { "git", "check-ref-format", "--branch", NULL, NULL };
    char *create[] = { "git", "switch", "-c", NULL, NULL };
    if (!git_policy(cfg, "auto_branch", 1)) return;
    if (git_output(root, branch, current, sizeof(current)) != 0) return;
    current[strcspn(current, "\r\n \t")] = '\0';
    if (strcmp(current, "main") != 0 && strcmp(current, "master") != 0) return;
    if (!toml_get(cfg, "project", "name", project, sizeof(project)) || !project[0])
        snprintf(project, sizeof(project), "project");
    git_string(cfg, "branch_name", wanted, sizeof(wanted), "paxsy/{project}");
    {
        char expanded[256];
        const char *mark = strstr(wanted, "{project}");
        if (mark) {
            size_t prefix = (size_t)(mark - wanted);
            snprintf(expanded, sizeof(expanded), "%.*s%s%s",
                     (int)prefix, wanted, project, mark + 9);
            snprintf(wanted, sizeof(wanted), "%s", expanded);
        }
    }
    if (!wanted[0] || strchr(wanted, ' ') || wanted[0] == '-') return;
    check_name[3] = wanted;
    if (git_rc(root, check_name) != 0) {
        if (cli->verbose) fprintf(stderr, "Paxsy Git: invalid branch_name '%s'; branch creation skipped\n", wanted);
        return;
    }
    exists[4] = wanted;
    if (git_rc(root, exists) == 0) return;
    create[3] = wanted;
    if (git_rc(root, create) == 0) {
        if (cli->verbose) printf("created Git branch %s\n", wanted);
    } else if (cli->verbose) {
        fprintf(stderr, "Paxsy Git: could not create branch '%s'\n", wanted);
    }
}

/* Commit only explicitly managed paths, and only when the user's index is
 * empty. This avoids accidentally absorbing work staged by the user. */
void paxsy_autocommit(const Cli *cli, const char *root) {
    const char *opt = getenv("PAXSY_NO_GIT");
    char status[4096], subject[256], cfg[PAXSY_PATH_MAX];
    char configured_message[192], config_name[PAXSY_PATH_MAX];
    char project[128] = "project";
    char *probe[] = { "git", "rev-parse", "--is-inside-work-tree", NULL };
    char *staged[] = { "git", "diff", "--cached", "--quiet", "--exit-code", NULL };
    char *diff[] = { "git", "diff", "--cached", "--name-status", NULL };
    char *add[16] = { "git", "add", "-A", "--", NULL };
    char *commit[] = { "git", "commit", "-m", NULL, "-m", NULL, NULL };
    int files = 0, add_count = 4;

    if (opt && opt[0] && strcmp(opt, "0") != 0) return;
    if (toml_find(root, cfg, sizeof(cfg)) != 0) return;
    if (!git_policy(cfg, "enabled", 1)) return;
    if (git_rc(root, probe) != 0) return;
    ensure_work_branch(cli, root, cfg);
    if (!git_policy(cfg, "auto_commit", 1) ||
        !git_policy(cfg, "commit_on_build", 1)) return;
    if (git_rc(root, staged) != 0) {
        if (cli->verbose) fprintf(stderr,
            "Paxsy Git: skipped automatic commit because the index contains staged changes\n");
        return;
    }
    {
        const char *base;
        base = strrchr(cfg, '/'); base = base ? base + 1 : cfg;
        snprintf(config_name, sizeof(config_name), "%s", base);
    }
    if (git_include(cfg, "include_src", 1) && project_path_exists(root, "src")) add[add_count++] = "src";
    if (git_include(cfg, "include_tests", 1) && project_path_exists(root, "tests")) add[add_count++] = "tests";
    if (git_include(cfg, "include_benches", 1) && project_path_exists(root, "benches")) add[add_count++] = "benches";
    if (git_include(cfg, "include_readme", 1) && project_path_exists(root, "README.md")) add[add_count++] = "README.md";
    if (git_include(cfg, "include_gitignore", 1) && project_path_exists(root, ".gitignore")) add[add_count++] = ".gitignore";
    if (git_include(cfg, "include_config", 1)) add[add_count++] = config_name;
    if (add_count == 4) return;
    add[add_count] = NULL;
    if (git_rc(root, add) != 0) return;
    if (git_capture(root, diff, status, sizeof(status)) != 0 || !status[0]) return;
    for (const char *p = status; *p; p++) if (*p == '\n') files++;
    if (!files) files = 1;
    toml_get(cfg, "project", "name", project, sizeof(project));
    git_string(cfg, "commit_message", configured_message,
               sizeof(configured_message), "build: {project} ({files} files)");
    {
        char expanded[256];
        const char *p = strstr(configured_message, "{project}");
        if (p) {
            size_t n = (size_t)(p - configured_message);
            snprintf(expanded, sizeof(expanded), "%.*s%s%s", (int)n,
                     configured_message, project, p + 9);
            memcpy(configured_message, expanded, sizeof(configured_message) - 1);
            configured_message[sizeof(configured_message) - 1] = '\0';
        }
        p = strstr(configured_message, "{files}");
        if (p) {
            size_t n = (size_t)(p - configured_message);
            snprintf(expanded, sizeof(expanded), "%.*s%d%s", (int)n,
                     configured_message, files, p + 7);
            memcpy(configured_message, expanded, sizeof(configured_message) - 1);
            configured_message[sizeof(configured_message) - 1] = '\0';
        }
    }
    snprintf(subject, sizeof(subject), "%s", configured_message);
    commit[3] = subject;
    commit[5] = "Created by Paxsy after a successful build.";
    if (git_rc(root, commit) == 0) {
        if (cli->verbose) printf("Paxsy Git: committed %d managed file(s)\n", files);
    } else if (cli->verbose) {
        fprintf(stderr, "Paxsy Git: commit failed; source changes remain in the working tree\n");
    }
}
