#include "common.h"
#include "source.h"

/*
 * source.c - file loading and line-index construction.
 * The line index records the byte offset where every line begins so that
 * retrieving a line is an O(1) lookup followed by a length computation.
 */

int source_load(Source *src, const char *path) {
    memset(src, 0, sizeof(*src));
    src->path = path;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return -1; }
    rewind(f);

    src->text = malloc((size_t)size + 1);
    if (!src->text) { fclose(f); return -1; }
    src->len = fread(src->text, 1, (size_t)size, f);
    src->text[src->len] = '\0';
    fclose(f);

    /* Count lines: one line per '\n', plus a trailing partial line. */
    int count = 1;
    for (size_t i = 0; i < src->len; i++)
        if (src->text[i] == '\n') count++;

    src->line_start = malloc((size_t)count * sizeof(int));
    if (!src->line_start) { free(src->text); src->text = NULL; return -1; }

    int n = 0;
    src->line_start[n++] = 0;
    for (size_t i = 0; i < src->len; i++)
        if (src->text[i] == '\n' && i + 1 < src->len)
            src->line_start[n++] = (int)(i + 1);
    src->line_count = n;
    return 0;
}

const char *source_line(const Source *src, int n, int *len) {
    if (n < 1 || n > src->line_count) return NULL;
    int start = src->line_start[n - 1];
    int end = (n < src->line_count) ? src->line_start[n] : (int)src->len;
    if (end > start && src->text[end - 1] == '\n') end--;
    if (end > start && src->text[end - 1] == '\r') end--;
    *len = end - start;
    return src->text + start;
}

void source_free(Source *src) {
    free(src->text);
    free(src->line_start);
    memset(src, 0, sizeof(*src));
}
