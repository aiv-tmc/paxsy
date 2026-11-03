#include "common.h"
#include "nar.h"

#include <stdio.h>
#include <string.h>

/*
 * nar.c - serialisation of the classic ar archive format. Layout:
 * the global header "!<arch>\n", then for every member a 60-byte
 * header (name, timestamp, owner, group, mode, size, magic) followed
 * by the member bytes and one padding newline when the size is odd.
 * The timestamp is written as zero and the ownership fields as
 * numeric zero/empty, which every reader accepts; the mode carries
 * 0o644 for ordinary files.
 */

/* Writes one archive-file header field: `width` characters, the
 * value left-justified and padded with spaces. */
static void field(FILE *f, const char *val, int width) {
    int n = (int)strlen(val);
    if (n > width) n = width;
    fwrite(val, 1, (size_t)n, f);
    for (int i = n; i < width; i++) fputc(' ', f);
}

/* Writes the decimal size field right-justified in `width`. */
static void numfield(FILE *f, unsigned long v, int width) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", v);
    int n = (int)strlen(buf);
    for (int i = n; i < width; i++) fputc(' ', f);
    fwrite(buf, 1, (size_t)n, f);
}

int nar_write(const char *path, const NAMember *members, int nmembers) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fputs("!<arch>\n", f);
    for (int i = 0; i < nmembers; i++) {
        /* The name field ends with '/' in the SysV variant; names
         * longer than 15 characters would need the extended-name
         * table, which object files of this toolchain never
         * produce. */
        char name[18];
        snprintf(name, sizeof(name), "%.15s/", members[i].name);
        field(f, name, 16);
        numfield(f, 0, 12);          /* timestamp: none     */
        field(f, "0", 6);            /* owner uid           */
        field(f, "0", 6);            /* group gid           */
        numfield(f, 0644, 8);        /* mode                */
        numfield(f, (unsigned long)members[i].size, 10);
        fputs("`\n", f);
        fwrite(members[i].data, 1, members[i].size, f);
        if (members[i].size & 1) fputc('\n', f);
    }
    fclose(f);
    return 0;
}
