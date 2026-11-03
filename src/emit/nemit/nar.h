#ifndef PAXSY_NAR_H
#define PAXSY_NAR_H

/*
 * nar.h - the ar archive writer of the native emission pass. An
 * archive is the verbatim concatenation of member files under a
 * global magic and one fixed-size header per member; the variant
 * written here omits the optional symbol-table member, so a member
 * is looked up by scanning the archive - which is exactly what the
 * toolchain's own linker does. Member data is padded to an even
 * size with a single newline, per the archive format.
 */

#include <stddef.h>

/* One archive member. */
typedef struct {
    const char *name;       /* file name inside the archive        */
    const void *data;       /* member bytes                        */
    size_t size;            /* member size in bytes                */
} NAMember;

/* Writes `nmembers` members to `path` in archive order. Returns 0
 * on success. */
int nar_write(const char *path, const NAMember *members, int nmembers);

#endif /* PAXSY_NAR_H */
