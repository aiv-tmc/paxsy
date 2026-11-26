#ifndef PAXSY_RECOVERY_INT_H
#define PAXSY_RECOVERY_INT_H

#include <stdio.h>
#include <sys/types.h>

FILE *objdump_open(const char *path, pid_t *child_pid);
int objdump_close(FILE *stream, pid_t child_pid);
/* Disassemble an already-resolved binary into a caller-owned directory.
 * Unlike the public CLI command, this helper does not create a .disasm
 * project beside the input; recovery uses a private temporary directory. */
int paxsy_disassemble_to_dir(const char *binary, const char *outdir);

#endif
