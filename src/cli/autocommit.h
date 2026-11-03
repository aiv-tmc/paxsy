#ifndef PAXSY_AUTOCOMMIT_H
#define PAXSY_AUTOCOMMIT_H

/*
 * autocommit.h - the post-build git snapshot entry point.
 * Declared for build.c; implemented in cli/autocommit.c.
 */

#include "cli.h"

/* Commits every pending change under `root` after a successful
 * build. Best-effort: never affects the build exit status. */
void paxsy_autocommit(const Cli *cli, const char *root);

#endif /* PAXSY_AUTOCOMMIT_H */
