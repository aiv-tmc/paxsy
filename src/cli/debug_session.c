#include "debug_int.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>

/* Installs a software breakpoint while retaining the displaced word. */
const char *sym_find(const Sess *s, const char *name,
                            uint64_t *addr) {
    size_t i;
    size_t ln = strlen(name);
    for (i = 0; i < s->nsyms; i++)
        if (strncmp(s->syms[i].name, name, ln) == 0 &&
            s->syms[i].name[ln] == 0) {
            *addr = s->syms[i].addr;
            return s->syms[i].name;
        }
    return NULL;
}

int bp_plant(Sess *s, uint64_t addr) {
    if (s->nbp >= 16) {
        fprintf(stderr, "debug: breakpoint limit (16) exceeded\n");
        return -1;
    }
    errno = 0;
    s->bp[s->nbp].saved = dbg_read_word(s, addr);
    if (errno) {
        fprintf(stderr, "debug: cannot read breakpoint address 0x%llx: %s\n",
                (unsigned long long)addr, strerror(errno));
        return -1;
    }
    if (dbg_write_word(s, addr,
                       (s->bp[s->nbp].saved & ~0xffL) | 0xcc) != 0) {
        fprintf(stderr, "debug: cannot patch breakpoint address 0x%llx: %s\n",
                (unsigned long long)addr, strerror(errno));
        return -1;
    }
    s->bp[s->nbp].addr = addr;
    s->bp[s->nbp].active = 1;
    s->nbp++;
    printf("breakpoint %d at 0x%llx\n", s->nbp,
           (unsigned long long)addr);
    return 0;
}

/* Records a breakpoint for the next 'run' (no inferior yet). */


void sess_regs(Sess *s);

int sess_wait(Sess *s) {
    int st;
    if (waitpid(s->pid, &st, 0) < 0) {
        fprintf(stderr, "debug: waitpid failed: %s\n", strerror(errno));
        s->failed = 1;
        s->running = 0;
        return -1;
    }
    if (WIFEXITED(st)) {
        s->result = WEXITSTATUS(st);
        printf("[inferior exited with code %d]\n", s->result);
        s->running = 0;
        return -1;
    }
    if (WIFSIGNALED(st)) {
        s->result = 128 + WTERMSIG(st);
        printf("[inferior died from signal %d]\n", WTERMSIG(st));
        s->running = 0;
        return -1;
    }
    if (WIFSTOPPED(st)) {
        struct user_regs_struct r;
        if (WSTOPSIG(st) == SIGTRAP &&
            ptrace(PTRACE_GETREGS, s->pid, 0, &r) == 0) {
            int i, hit = 0;
            uint64_t ip = (uint64_t)r.rip;
            for (i = 0; i < s->nbp; i++)
                if (s->bp[i].active && ip - 1 == s->bp[i].addr) {
                    printf("breakpoint %d, at 0x%llx\n", i + 1,
                           (unsigned long long)s->bp[i].addr);
                    s->hit_breakpoint = 1;
                    /* Restore the instruction before stepping over it. */
                    if (dbg_write_word(s, s->bp[i].addr,
                                       s->bp[i].saved) != 0) {
                        fprintf(stderr, "debug: cannot restore breakpoint: %s\n",
                                strerror(errno));
                        s->failed = 1;
                        s->running = 0;
                        return -1;
                    }
                    s->bp[i].active = 0;
                    r.rip = s->bp[i].addr;
                    if (ptrace(PTRACE_SETREGS, s->pid, 0, &r) != 0 ||
                        ptrace(PTRACE_SINGLESTEP, s->pid, 0, 0) != 0 ||
                        waitpid(s->pid, &st, 0) < 0) {
                        fprintf(stderr, "debug: failed to step over breakpoint: %s\n",
                                strerror(errno));
                        s->failed = 1;
                        s->running = 0;
                        return -1;
                    }
                    /* Re-arm the breakpoint after the displaced instruction. */
                    if (dbg_write_word(s, s->bp[i].addr,
                                       (s->bp[i].saved & ~0xffL) | 0xcc) != 0) {
                        fprintf(stderr, "debug: failed to re-arm breakpoint: %s\n",
                                strerror(errno));
                        s->failed = 1;
                        s->running = 0;
                        return -1;
                    }
                    s->bp[i].active = 1;
                    hit = 1;
                }
            if (hit) {
                if (s->cli && (s->cli->dbg_regs ||
                    s->cli->dbg_trace_registers || s->cli->dbg_trace_all))
                    sess_regs(s);
                return 0;
            }
        }
        if (!s->started) {
            s->started = 1;
            return 0;
        }
        if (!s->stepping) {
            s->pending_signal = WSTOPSIG(st);
            printf("[stopped by signal %d]\n", s->pending_signal);
            sess_regs(s);
        }
        return 0;
    }
    return 0;
}

void sess_continue(Sess *s) {
    int signal_to_deliver = s->pending_signal;
    s->pending_signal = 0;
    if (ptrace(PTRACE_CONT, s->pid, 0,
               (void *)(intptr_t)signal_to_deliver) != 0) {
        fprintf(stderr, "debug: cannot continue process: %s\n", strerror(errno));
        s->failed = 1;
        s->running = 0;
        return;
    }
    sess_wait(s);
}

void sess_stepi(Sess *s) {
    int signal_to_deliver = s->pending_signal;
    s->pending_signal = 0;
    if (ptrace(PTRACE_SINGLESTEP, s->pid, 0,
               (void *)(intptr_t)signal_to_deliver) != 0) {
        fprintf(stderr, "debug: cannot single-step process: %s\n", strerror(errno));
        s->failed = 1;
        s->running = 0;
        return;
    }
    s->stepping = 1;
    sess_wait(s);
    s->stepping = 0;
    if (s->running) printf("[stepped]\n");
}

void sess_regs(Sess *s) {
    struct user_regs_struct r;
    if (ptrace(PTRACE_GETREGS, s->pid, 0, &r) != 0) {
        fprintf(stderr, "debug: cannot read registers: %s\n", strerror(errno));
        s->failed = 1;
        return;
    }
    if (s->cli && (s->cli->dbg_trace_registers || s->cli->dbg_trace_all)) {
        FILE *out = s->trace ? s->trace : stderr;
        fprintf(out,
                "registers rip=0x%llx rflags=0x%llx rax=0x%llx "
                "rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx "
                "rdi=0x%llx rbp=0x%llx rsp=0x%llx r8=0x%llx "
                "r9=0x%llx r10=0x%llx r11=0x%llx r12=0x%llx "
                "r13=0x%llx r14=0x%llx r15=0x%llx\n",
                (unsigned long long)r.rip, (unsigned long long)r.eflags,
                (unsigned long long)r.rax, (unsigned long long)r.rbx,
                (unsigned long long)r.rcx, (unsigned long long)r.rdx,
                (unsigned long long)r.rsi, (unsigned long long)r.rdi,
                (unsigned long long)r.rbp, (unsigned long long)r.rsp,
                (unsigned long long)r.r8, (unsigned long long)r.r9,
                (unsigned long long)r.r10, (unsigned long long)r.r11,
                (unsigned long long)r.r12, (unsigned long long)r.r13,
                (unsigned long long)r.r14, (unsigned long long)r.r15);
        fflush(out);
    }
    printf("rip=0x%llx  rflags=0x%llx  rax=0x%llx  rbx=0x%llx\n",
           (unsigned long long)r.rip, (unsigned long long)r.eflags,
           (unsigned long long)r.rax, (unsigned long long)r.rbx);
    printf("rcx=0x%llx  rdx=0x%llx  rsi=0x%llx  rdi=0x%llx\n",
           (unsigned long long)r.rcx, (unsigned long long)r.rdx,
           (unsigned long long)r.rsi, (unsigned long long)r.rdi);
    printf("rbp=0x%llx  rsp=0x%llx  r8=0x%llx  r9=0x%llx\n",
           (unsigned long long)r.rbp, (unsigned long long)r.rsp,
           (unsigned long long)r.r8, (unsigned long long)r.r9);
    printf("r10=0x%llx  r11=0x%llx  r12=0x%llx  r13=0x%llx\n",
           (unsigned long long)r.r10, (unsigned long long)r.r11,
           (unsigned long long)r.r12, (unsigned long long)r.r13);
    printf("r14=0x%llx  r15=0x%llx\n",
           (unsigned long long)r.r14, (unsigned long long)r.r15);
}

/* Config-driven session. Queued breakpoints plant at the first stop;
 * the remaining actions run in the configured order: step, inspect,
 * continue. */
