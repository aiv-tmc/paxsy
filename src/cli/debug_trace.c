#include "debug_int.h"

#include <errno.h>
#include <sys/ptrace.h>

/* Writes one target-memory event to the selected trace stream. */
static void trace_memory(Sess *s, const char *operation, uint64_t addr,
                         unsigned long long before,
                         unsigned long long after, int ok) {
    if (!s->cli || !(s->cli->dbg_trace_memory || s->cli->dbg_trace_all))
        return;
    FILE *out = s->trace ? s->trace : stderr;
    if (strcmp(operation, "write") == 0) {
        fprintf(out,
                "memory write pid=%ld address=0x%llx bytes=%zu "
                "before=0x%llx after=0x%llx status=%s\n",
                (long)s->pid, (unsigned long long)addr, sizeof(long),
                before, after, ok ? "ok" : "error");
    } else {
        fprintf(out,
                "memory %-16s pid=%ld address=0x%llx bytes=%zu "
                "value=0x%llx status=%s\n",
                operation, (long)s->pid, (unsigned long long)addr,
                sizeof(long), after, ok ? "ok" : "error");
    }
    fflush(out);
}

long dbg_read_word(Sess *s, uint64_t addr) {
    errno = 0;
    long value = ptrace(PTRACE_PEEKTEXT, s->pid, (void *)addr, 0);
    int ok = errno == 0;
    trace_memory(s, "read", addr, 0,
                 ok ? (unsigned long long)value : 0, ok);
    return value;
}

int dbg_write_word(Sess *s, uint64_t addr, long value) {
    errno = 0;
    long previous = ptrace(PTRACE_PEEKTEXT, s->pid, (void *)addr, 0);
    int readable = errno == 0;
    trace_memory(s, "read-before-write", addr, 0,
                 readable ? (unsigned long long)previous : 0, readable);
    int rc = (int)ptrace(PTRACE_POKETEXT, s->pid, (void *)addr,
                         (void *)(intptr_t)value);
    trace_memory(s, "write", addr,
                 readable ? (unsigned long long)previous : 0,
                 (unsigned long long)value, rc == 0);
    return rc;
}

