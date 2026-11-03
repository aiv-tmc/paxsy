#ifndef PAXSY_PKGINT_H
#define PAXSY_PKGINT_H

/*
 * pkgint.h - internals of the package manager shared between the index
 * commands (cmd_pkg.c) and the package operations (cmd_pkg_ops.c).
 */

/* Resolves the cache root, creating its subdirectories. */
int pkg_cache_root(char *out, int n);

/* Fetches pkg into the cache. Returns 1 when a fresh copy was
 * downloaded, 0 when the network failed but a usable cache entry
 * remains, and -1 when the package is unavailable both ways. */
int pkg_ensure_cached(const char *root, const char *pkg, const char *version);

#endif /* PAXSY_PKGINT_H */
