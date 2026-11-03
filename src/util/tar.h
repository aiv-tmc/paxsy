#ifndef PAXSY_TAR_H
#define PAXSY_TAR_H

/*
 * tar.h - deterministic ustar archive writer used by the artifact
 * commands (state/shared). All metadata fields are fixed (mode 0644,
 * zero uid/gid/mtime) so identical inputs always yield identical
 * archives, matching the deterministic-builds commitment of the
 * toolchain manual.
 */

/* Packs `count` regular files into archive_path. Each member is stored
 * under its base name. Returns 0 on success. */
int tar_create(const char *archive_path, const char **files, int count);

#endif /* PAXSY_TAR_H */
