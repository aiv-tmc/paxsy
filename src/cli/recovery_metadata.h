#ifndef PAXSY_RECOVERY_METADATA_H
#define PAXSY_RECOVERY_METADATA_H

#include "srcdir.h"
#include <stddef.h>

typedef struct {
    char *path;
    unsigned char *data;
    size_t size;
} PaxsyRecoveredFile;

typedef struct {
    PaxsyRecoveredFile *files;
    size_t count;
} PaxsySourceArchive;

/* Embed project source files in a non-loadable ELF section. This is
 * opt-in because an executable containing source also contains the
 * source's intellectual property. */
int paxsy_embed_sources(const char *binary, const char *root,
                        const SrcList *sources);
/* Returns 0 when source is present, 1 when absent, -1 when malformed. */
int paxsy_extract_sources(const char *binary, PaxsySourceArchive *archive);
void paxsy_source_archive_free(PaxsySourceArchive *archive);

#endif
