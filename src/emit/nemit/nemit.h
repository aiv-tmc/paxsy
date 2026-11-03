#ifndef PAXSY_NEMIT_H
#define PAXSY_NEMIT_H

/*
 * nemit.h - the native emission pass. nemit_build() turns a set of
 * typed HIR modules into a statically linked x86-64 ELF executable;
 * nemit_obj() emits the same unit as an ELF relocatable object and
 * nemit_pe() as a PE32+ image. All modes run entirely in-process:
 * instruction selection, machine-code encoding, the start-up
 * sequence and the image writers are produced by the compiler, and
 * no external assembler, compiler or linker is invoked. Unsupported
 * constructs are diagnosed through the shared diagnostic stream and
 * count as errors, so an output file only ever appears for a fully
 * supported program.
 */

#include "hir.h"
#include "diag.h"

/* Emits a statically linked x86-64 ELF executable at `out_path`. */
int nemit_build(HModule **mods, int nmods, Diag *diag, const char *out_path);

/* Emits an ELF64 relocatable object (.o): the code and data
 * sections, a symbol table with every global function and object
 * plus _start, and one R_X86_64_64 relocation per absolute address
 * constant. */
int nemit_obj(HModule **mods, int nmods, Diag *diag, const char *out_path);

/* Emits a PE32+ image (Windows executable) at `out_path`: the same
 * sections laid out at the conventional PE addresses with the
 * start-up as the entry point. */
int nemit_pe(HModule **mods, int nmods, Diag *diag, const char *out_path);

#endif /* PAXSY_NEMIT_H */
