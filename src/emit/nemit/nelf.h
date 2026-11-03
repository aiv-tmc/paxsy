#ifndef PAXSY_NELF_H
#define PAXSY_NELF_H

/*
 * nelf.h - the 64-bit ELF executable writer for the native
 * emission pass. The layout is computed first so the caller can
 * patch absolute address constants into the code buffer, then the
 * image is serialised in one pass. The output is a statically
 * linked, position-dependent x86-64 executable with no interpreter
 * and no dynamic section: two load segments cover read-execute
 * memory (headers, code, read-only data) and read-write memory
 * (initialised data plus the bss tail).
 */

#include "common.h"

#include <stdint.h>

typedef struct {
    uint64_t va_text;   /* virtual address of the code            */
    uint64_t va_ro;     /* virtual address of the read-only data  */
    uint64_t va_data;   /* virtual address of the writable data   */
    uint32_t off_text;  /* file offset of the code              */
    uint32_t off_ro;    /* file offset of the read-only data    */
    uint32_t off_data;  /* file offset of the writable data     */
    uint64_t entry;     /* absolute entry-point address         */
    uint32_t file_size; /* total bytes of the image             */
} NELayout;

/* Fills `L` with the image geometry for the given section sizes and
 * the code offset of the entry point. */
void nelf_layout(size_t text_len, size_t ro_len, size_t data_len,
                 size_t bss_len, uint32_t entry_off, NELayout *L);

/* Serialises the image described by `L` to `path`. Returns 0 on
 * success. */
int nelf_write(const char *path, const NELayout *L,
               const uint8_t *text, size_t text_len,
               const uint8_t *ro, size_t ro_len,
               const uint8_t *data, size_t data_len, size_t bss_len);

#endif /* PAXSY_NELF_H */
