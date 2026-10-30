#ifndef PAXSY_FSUTIL_H
#define PAXSY_FSUTIL_H

/*
 * fsutil.h - small file-system helpers shared by the command
 * implementations: directory tests, mkdir -p, file copies, recursive
 * copies and recursive deletion. Every function returns 0 on success.
 */

int fs_is_dir(const char *path);
int fs_is_file(const char *path);

/* Creates every missing directory component of path. */
int fs_mkdir_p(const char *path);

/* Copies one file, truncating the destination. */
int fs_copy_file(const char *src, const char *dst);

/* Recursively copies a directory tree. */
int fs_copy_tree(const char *src, const char *dst);

/* Recursively deletes a directory tree (rm -rf equivalent). */
int fs_remove_tree(const char *path);

#endif /* PAXSY_FSUTIL_H */
