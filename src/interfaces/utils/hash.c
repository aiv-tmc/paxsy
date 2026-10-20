#include "hash.h"
#include <stdio.h>

uint64_t u__hash_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint64_t hash = 1469598103934665603ULL; // FNV offset basis
    const uint64_t prime = 1099511628211ULL;
    int c;
    while ((c = fgetc(f)) != EOF) {
        hash ^= (uint64_t)c;
        hash *= prime;
    }
    fclose(f);
    return hash;
}
