#ifndef PAXSY_SOURCE_H
#define PAXSY_SOURCE_H

/*
 * source.h - in-memory representation of one source file plus a line index
 * used by the diagnostic printer to extract the offending source line.
 */

typedef struct Source Source;
struct Source {
    char *text;       /* NUL-terminated copy of the file contents */
    size_t len;       /* length in bytes, excluding the terminator */
    const char *path; /* file name as supplied on the command line */
    int *line_start;  /* byte offset of the first byte of each line */
    int line_count;   /* number of lines (one more than '\n' count if last line is unterminated) */
};

/* Loads a file fully into memory and builds the line index.
 * Returns 0 on success, -1 if the file cannot be read. */
int source_load(Source *src, const char *path);

/* Returns a pointer to the text of 1-based line n and stores its length
 * in *len (newline excluded). Returns NULL when n is out of range. */
const char *source_line(const Source *src, int n, int *len);

void source_free(Source *src);

#endif /* PAXSY_SOURCE_H */
