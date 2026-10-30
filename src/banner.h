#ifndef PAXSY_BANNER_H
#define PAXSY_BANNER_H

/*
 * banner.h - visual identity of the command-line tool.
 * banner_print() renders the coloured block logo together with the
 * version line; usage_print() renders the complete command reference;
 * license_print() renders the MIT license text shown by --license.
 */

void banner_print(FILE *out);
void usage_print(FILE *out);
void license_print(FILE *out);

#endif /* PAXSY_BANNER_H */
