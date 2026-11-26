#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "recovery_int.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Produce a machine-level listing for binaries without Paxsy source
 * metadata. The path is passed as an argv item; no shell is involved. */
FILE *objdump_open(const char *path, pid_t *child_pid) {
    int fds[2];
    pid_t pid;
    FILE *stream;
    if (pipe(fds) != 0) return NULL;
    pid = fork();
    if (pid == 0) {
        int null_fd;
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(126);
        close(fds[1]);
        null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        execlp("objdump", "objdump", "-d", "--wide", "-C", "--",
               path, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return NULL; }
    stream = fdopen(fds[0], "r");
    if (!stream) {
        int status;
        close(fds[0]);
        waitpid(pid, &status, 0);
        return NULL;
    }
    *child_pid = pid;
    return stream;
}

int objdump_close(FILE *stream, pid_t child_pid) {
    int status = 0;
    int close_rc = fclose(stream);
    if (waitpid(child_pid, &status, 0) < 0 || close_rc != 0 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}
