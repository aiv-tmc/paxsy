#include "common.h"
#include "nsel.h"

/*
 * nrt.c - the freestanding start-up emitted at the beginning of the
 * text buffer. The binary carries exactly one runtime routine, the
 * entry point: it calls the user main and turns its error code into
 * the process exit status through the exit syscall. Every other
 * service a program may use (printing, string handling) lives in the
 * standard library, written in Paxsy and compiled like any other
 * function; nothing is provided silently by the backend.
 */

void nrt_emit(NSel *s) {
    NXBuf *b = s->text;
    nx_def(b, s->rt_start);
    nx_call(b, s->lbl_main);
    nx_mov_rr(b, NRDI, NRDX);   /* exit status = main's error code  */
    nx_mov_ri(b, NRAX, 60);     /* SYS_exit                         */
    nx_syscall(b);
}
