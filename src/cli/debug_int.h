#ifndef PAXSY_DEBUG_INT_H
#define PAXSY_DEBUG_INT_H

#include "common.h"
#include "cli.h"

#include <stdint.h>
#include <sys/types.h>

/* ELF function symbol used for function-entry breakpoints. */
typedef struct {
    char *name;
    uint64_t addr;
} Sym;

typedef struct {
    uint64_t addr;
    long saved;
    int active;
} DebugBreakpoint;

typedef struct {
    pid_t pid;
    int running;
    int started;
    int hit_breakpoint;
    int failed;
    int result;
    int pending_signal;
    int stepping;
    Sym *syms;
    size_t nsyms;
    DebugBreakpoint bp[16];
    int nbp;
    const Cli *cli;
    FILE *trace;
} Sess;

int elf_load_syms(const char *path, Sym **out, size_t *count);
const char *sym_find(const Sess *session, const char *name, uint64_t *addr);
int bp_plant(Sess *session, uint64_t addr);
long dbg_read_word(Sess *session, uint64_t addr);
int dbg_write_word(Sess *session, uint64_t addr, long value);
int sess_wait(Sess *session);
void sess_continue(Sess *session);
void sess_stepi(Sess *session);
void sess_regs(Sess *session);

#endif
