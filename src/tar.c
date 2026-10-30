#include "common.h"
#include "tar.h"

/*
 * tar.c - minimal POSIX ustar writer (no compression, no directories,
 * no links). See header for the determinism guarantees.
 */

/* Writes `width`-byte numeric field: width-1 octal digits plus NUL. */
static void tar_octal(char *field, int width, unsigned long value) {
    snprintf(field, (size_t)width, "%0*lo", width - 1, value);
}

static int write_header(FILE *f, const char *arcname, unsigned long size) {
    unsigned char h[512];
    memset(h, 0, sizeof(h));

    size_t namelen = strlen(arcname);
    if (namelen > 99) namelen = 99;
    memcpy(h, arcname, namelen);              /* name[100] */

    tar_octal((char *)h + 100, 8, 0644);      /* mode[8]   */
    tar_octal((char *)h + 108, 8, 0);         /* uid[8]    */
    tar_octal((char *)h + 116, 8, 0);         /* gid[8]    */
    tar_octal((char *)h + 124, 12, size);     /* size[12]  */
    tar_octal((char *)h + 136, 12, 0);        /* mtime[12] */

    memset(h + 148, ' ', 8);                  /* chksum computed as spaces */
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[154] = '\0';
    h[155] = ' ';

    h[156] = '0';                             /* regular file */
    memcpy(h + 257, "ustar", 6);              /* magic + version */
    h[263] = '0'; h[264] = '0';
    tar_octal((char *)h + 329, 8, 0);         /* devmajor    */
    tar_octal((char *)h + 337, 8, 0);         /* devminor    */

    return fwrite(h, 1, sizeof(h), f) == sizeof(h) ? 0 : -1;
}

/* Pads the member body up to a 512-byte boundary with zeros. */
static int write_padding(FILE *f, unsigned long size) {
    unsigned long pad = (512 - (size % 512)) % 512;
    if (!pad) return 0;
    unsigned char zeros[512];
    memset(zeros, 0, sizeof(zeros));
    return fwrite(zeros, 1, pad, f) == pad ? 0 : -1;
}

int tar_create(const char *archive_path, const char **files, int count) {
    FILE *out = fopen(archive_path, "wb");
    if (!out) return -1;
    int rc = 0;
    for (int i = 0; i < count && rc == 0; i++) {
        FILE *in = fopen(files[i], "rb");
        if (!in) { rc = -1; break; }
        fseek(in, 0, SEEK_END);
        long size = ftell(in);
        rewind(in);
        const char *base = strrchr(files[i], '/');
        base = base ? base + 1 : files[i];
        rc |= write_header(out, base, (unsigned long)size);
        char buf[4096];
        size_t got;
        while (!rc && (got = fread(buf, 1, sizeof(buf), in)) > 0)
            if (fwrite(buf, 1, got, out) != got) rc = -1;
        fclose(in);
        rc |= write_padding(out, (unsigned long)(size < 0 ? 0 : size));
    }
    /* Two zero blocks terminate the archive. */
    unsigned char zeros[512];
    memset(zeros, 0, sizeof(zeros));
    if (fwrite(zeros, 1, sizeof(zeros), out) != sizeof(zeros)) rc = -1;
    if (fwrite(zeros, 1, sizeof(zeros), out) != sizeof(zeros)) rc = -1;
    fclose(out);
    return rc;
}
