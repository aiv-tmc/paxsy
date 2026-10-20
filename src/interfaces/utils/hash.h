#ifndef U_HASH_H
#define U_HASH_H

#include <stdint.h>

/*
 * Compute a 64-bit FNV-1a hash of a file's contents.
 * Returns 0 if the file cannot be opened.
 */
uint64_t u__hash_file(const char *path);

#endif
