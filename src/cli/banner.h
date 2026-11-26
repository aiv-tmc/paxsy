#ifndef PAXSY_BANNER_H
#define PAXSY_BANNER_H

/*
 * banner.h - visual identity of the command-line tool.
 * banner_print() renders the coloured block logo together with the
 * version line; usage_print() renders the complete command reference;
 * license_print() renders the Apache-2.0 license information shown by --license.
 */

void banner_print(FILE *out);
void usage_print(FILE *out);
void usage_short(FILE *out);
void license_print(FILE *out);

#endif /* PAXSY_BANNER_H */
