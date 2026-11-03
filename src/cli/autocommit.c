#include "common.h"
#include "cli.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

/*
 * cli/autocommit.c - the post-build git snapshot.
 * After a successful `paxsy build` the project history records what
 * the build touched: changed sources, regenerated units, the report.
 * The whole step is best-effort - it never turns a successful build
 * into a failure and stays silent unless verbose mode is on.
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
        while (used + 1 < cap &&
               (n = read(fds[0], capture + used, cap - 1 - used)) > 0)
            used += (size_t)n;
        capture[used] = '\0';
        close(fds[0]);
    }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int git_rc(const char *root, char *const argv[]) {
    return git_capture(root, argv, NULL, 0);
}

/* Commits every pending change under the project root. Skipped when:
 *   - PAXSY_NO_GIT is set (automation opt-out),
 *   - the project is not inside a git work tree,
 *   - the work tree is clean (no empty commits),
 *   - git is missing or the commit fails (verbose warning only).
 * The message subject carries the change count; the body lists the
 * porcelain status lines verbatim so `git log` shows what moved. */
void paxsy_autocommit(const Cli *cli, const char *root) {
    const char *opt = getenv("PAXSY_NO_GIT");
    char status[4096];
    char subject[128];
    char *probe[] = { "git", "rev-parse", "--is-inside-work-tree", NULL };
    char *st[]    = { "git", "status", "--porcelain", NULL };
    char *add[]   = { "git", "add", "-A", NULL };
    char *commit1[] = { "git", "commit", "-q", "-m", NULL, "-m", NULL, NULL };
    char *commit2[] = { "git", "-c", "user.name=paxsy",
                        "-c", "user.email=paxsy@localhost",
                        "commit", "-q", "-m", NULL, "-m", NULL, NULL };
    int files = 0;

    if (opt && opt[0] && strcmp(opt, "0") != 0) return;
    if (git_rc(root, probe) != 0) return;
    if (git_capture(root, st, status, sizeof(status)) != 0) return;
    if (status[0] == '\0') return;
    for (const char *p = status; *p; p++)
        if (*p == '\n') files++;
    if (files == 0) files = 1;
    snprintf(subject, sizeof(subject), "paxsy build: %d file(s) changed",
             files);

    if (git_rc(root, add) != 0) return;
    commit1[4] = subject;
    commit1[6] = status;
    if (git_rc(root, commit1) == 0) {
        if (cli->verbose)
            printf("committed %d changed file(s) to git\n", files);
        return;
    }
    /* The user's git identity is unconfigured (fresh checkout, CI);
     * retry with the paxsy bot identity instead of failing. */
    commit2[6] = subject;
    commit2[8] = status;
    if (git_rc(root, commit2) == 0) {
        if (cli->verbose)
            printf("committed %d changed file(s) to git\n", files);
    } else if (cli->verbose) {
        fprintf(stderr, "paxsy: git commit failed; skipping autocommit\n");
    }
}
