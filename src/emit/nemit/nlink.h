#ifndef PAXSY_NLINK_H
#define PAXSY_NLINK_H

/*
 * nlink.h - the static linker of the toolchain. Input is a list of
 * ELF64 relocatable objects written by nelo (nemit_obj) and/or ar
 * archives written by nar; output is a statically linked x86-64
 * executable produced by nelf_write. The linker concatenates the
 * sections, resolves the global symbols across all inputs, applies
 * the R_X86_64_64 relocations and places the entry point at the
 * _start symbol. No external linker is invoked.
 */

#include "diag.h"

/* Links `nobjs` object files (archives are opened and their members
 * linked in archive order) into the executable `out_path`. Reports
 * through `diag` and returns 0 on success. */
int nlink_exec(Diag *diag, const char *out_path, char **objs, int nobjs);

#endif /* PAXSY_NLINK_H */
