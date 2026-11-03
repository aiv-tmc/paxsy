#ifndef PAXSY_NELO_H
#define PAXSY_NELO_H

/*
 * nelo.h - the 64-bit ELF relocatable-object writer of the native
 * emission pass. A unit is serialised as ET_REL: the code, the
 * read-only data and the writable data become sections, every
 * global function and global object becomes a symbol, and every
 * absolute address constant becomes one R_X86_64_64 relocation
 * against the section symbol of its target section, with the
 * section-relative offset carried in the addend. A consumer - the
 * toolchain's own linker or an external one - applies the
 * relocations when placing the sections. No program headers exist
 * in a relocatable object; the section headers alone describe the
 * file. All integers are little-endian, matching the x86-64 target.
 */

#include <stdint.h>
#include <stddef.h>

/* Section indices used by nelo symbols and relocations. The writer
 * owns section symbols 1..3 in this exact order. */
enum {
    NELO_SEC_TEXT = 1,
    NELO_SEC_RODATA = 2,
    NELO_SEC_DATA = 3
};

/* One global symbol. info combines STB_GLOBAL (always) with
 * STT_FUNC for code and STT_OBJECT for data. shndx is one of the
 * NELO_SEC_* indices; value is the offset inside that section. */
typedef struct {
    const char *name;
    uint64_t value;
    uint64_t size;
    uint8_t info;
    uint16_t shndx;
} NESym;

/* One absolute-address constant: the 8 bytes at offset `pos` inside
 * .text refer to `addend` bytes into section `sect`; the writer
 * zeroes the placeholder bytes and records the pair as a
 * relocation. */
typedef struct {
    uint64_t pos;
    uint8_t sect;
    uint64_t addend;
} NEReloc;

/* Serialises one relocatable object to `path`. The writer prepends
 * the null symbol and the three section symbols to `syms`, so
 * sh_info of .symtab always points at the first caller symbol.
 * Returns 0 on success. */
int nelo_write(const char *path,
               const uint8_t *text, size_t text_len,
               const uint8_t *ro, size_t ro_len,
               const uint8_t *data, size_t data_len,
               const NESym *syms, int nsyms,
               const NEReloc *relocs, int nrelocs);

#endif /* PAXSY_NELO_H */
