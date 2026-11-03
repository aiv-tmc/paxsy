#ifndef PAXSY_NPE_H
#define PAXSY_NPE_H

/*
 * npe.h - the PE32+ image writer of the native emission pass. The
 * unit becomes a Windows executable: DOS header with a pointer to
 * the PE signature, COFF header, the PE32+ optional header, three
 * section headers (.text, .rdata, .data) and the raw section data.
 * The image carries no imports, no relocations and no TLS: every
 * address constant is absolute against the fixed image base, which
 * is the standard shape of a freestanding console program. The
 * caller passes the section bytes with their final RVAs and the
 * image base; the writer computes file offsets, headers and sizes.
 */

#include <stdint.h>
#include <stddef.h>

/* Serialises a PE32+ image to `path`. va_* are the section RVAs;
 * image_base is the preferred load address baked into every
 * absolute constant by the caller. Returns 0 on success. */
int npe_write(const char *path,
              const uint8_t *text, size_t text_len,
              const uint8_t *ro, size_t ro_len,
              const uint8_t *data, size_t data_len,
              uint64_t image_base,
              uint64_t va_text, uint64_t va_ro, uint64_t va_data);

#endif /* PAXSY_NPE_H */
