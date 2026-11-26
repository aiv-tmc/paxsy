#include "debug_int.h"

#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

/* Runs the project binary under the flag-driven debugger session. */
int cmd_builtin_debug_flags(const Cli *cli, const char *bin) {
    Sess s;
    memset(&s, 0, sizeof(s));
    s.pid = -1;
    s.cli = cli;
    if (cli->dbg_trace_file) {
        s.trace = fopen(cli->dbg_trace_file, "a");
        if (!s.trace) {
            fprintf(stderr, "cannot open trace file '%s'\n", cli->dbg_trace_file);
            return 1;
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    if (elf_load_syms(bin, &s.syms, &s.nsyms) != 0) {
        fprintf(stderr, "debug: no symbols in '%s'\n", bin);
        if (s.trace) fclose(s.trace);
        return 2;
    }
    s.pid = fork();
    if (s.pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) _exit(126);
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    if (s.pid < 0) {
        fprintf(stderr, "debug: cannot start the inferior\n");
        s.failed = 1;
        goto debug_cleanup;
    }
    s.running = 1;
    if (sess_wait(&s) != 0 || !s.running) goto debug_cleanup;
    {
        int bi;
        for (bi = 0; bi < cli->nbreaks; bi++) {
            uint64_t addr;
            if (!sym_find(&s, cli->breaks[bi], &addr)) {
                fprintf(stderr, "debug: no symbol '%s'\n",
                        cli->breaks[bi]);
                s.failed = 1;
                continue;
            }
            if (bp_plant(&s, addr) != 0) s.failed = 1;
        }
    }
    if (cli->dbg_stepi > 0 && !s.failed) {
        int k;
        for (k = 0; k < cli->dbg_stepi && s.running; k++)
            sess_stepi(&s);
    }
    if ((cli->dbg_regs || cli->dbg_trace_registers || cli->dbg_trace_all) &&
        s.running) sess_regs(&s);
    while (s.running && cli->dbg_run && !s.failed) {
        s.hit_breakpoint = 0;
        sess_continue(&s);
        if (s.hit_breakpoint && !cli->dbg_run_explicit) break;
    }
debug_cleanup:
    if (s.running) {
        ptrace(PTRACE_KILL, s.pid, 0, 0);
        waitpid(s.pid, NULL, 0);
        s.running = 0;
    }
    {
        size_t i;
        for (i = 0; i < s.nsyms; i++) free(s.syms[i].name);
        free(s.syms);
    }
    if (s.trace) fclose(s.trace);
    return s.failed ? 2 : s.result;
}
