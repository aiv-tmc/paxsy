#ifndef PAXSY_PKGDIR_H
#define PAXSY_PKGDIR_H

/*
 * pkgdir.h - read-only access to the local package installation
 * directory. The semantic analyser uses it to accept `use` directives
 * that reference packages installed on this machine.
 */

#define PKG_NAME_MAX 64

/* Stores the names of the packages inside <cache root>/installed into
 * the rows of `names` (at most `max`). Returns the number of names. */
int pkg_list_installed(char names[][PKG_NAME_MAX], int max);

#endif /* PAXSY_PKGDIR_H */
