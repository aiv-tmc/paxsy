#include "common.h"
#include "diag.h"
#include "source.h"
#include "color.h"

/*
 * diag.c - diagnostics storage and rendering.
 * Messages are rendered into a fixed buffer at report time, so callers
 * do not need to keep their format arguments alive. The listing is
 * plain white text: the gutter, the source line and the location
 * prefix carry no colour, only the severity label and the caret run
 * are tinted (red for errors, yellow for warnings).
 */

void diag_init(Diag *d, int max_errors) {
    memset(d, 0, sizeof(*d));
    d->max_errors = max_errors > 0 ? max_errors : 20;
    color_setup();
}

static void diag_push(Diag *d, DiagKind kind, const char *path,
                      int line, int col, int len, const char *msg) {
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 16;
        d->items = realloc(d->items, (size_t)d->cap * sizeof(Diagnostic));
        if (!d->items) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
    Diagnostic *it = &d->items[d->count++];
    it->kind = kind; it->path = path; it->line = line; it->col = col;
    it->len = len < 1 ? 1 : len;
    it->msg = malloc(strlen(msg) + 1);
    if (!it->msg) { fprintf(stderr, "out of memory\n"); exit(1); }
    strcpy(it->msg, msg);
}

void diag_report_full(Diag *d, DiagKind kind, const char *path,
                      int line, int col, int len, const char *fmt, ...) {
    if (kind == DK_ERROR && diag_error_count(d) >= d->max_errors) {
        d->overflow = 1;
        return;
    }
    if (kind == DK_WARNING && d->suppress_warnings) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    diag_push(d, kind, path, line, col, len, buf);
}

void diag_report(Diag *d, DiagKind kind, const char *path,
                 int line, int col, const char *fmt, ...) {
    if (kind == DK_ERROR && diag_error_count(d) >= d->max_errors) {
        d->overflow = 1;
        return;
    }
    if (kind == DK_WARNING && d->suppress_warnings) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    diag_push(d, kind, path, line, col, 1, buf);
}

int diag_error_count(const Diag *d) {
    int n = 0;
    for (int i = 0; i < d->count; i++)
        if (d->items[i].kind == DK_ERROR) n++;
    return n;
}

int diag_warning_count(const Diag *d) {
    int n = 0;
    for (int i = 0; i < d->count; i++)
        if (d->items[i].kind == DK_WARNING) n++;
    return n;
}

/* Paths are collected from directory scans that prefix every entry
 * with "./"; the prefix is noise in listings, so it is stripped for
 * display. A bare "." is reported as-is. */
static const char *diag_path(const char *path) {
    while (path && path[0] == '.' && path[1] == '/') path += 2;
    return path && *path ? path : "<input>";
}

/* Renders one source line and a caret run under the offending token:
 * the line number is right-aligned in six columns, the caret row
 * indents by the same gutter so both bars align, and the carets are
 * tinted with the severity colour.
 *
 *       10 | edgf}
 *          |  ^^^^    */
static void print_snippet(const Source *src, const Diagnostic *it,
                          const char *accent) {
    if (!src || it->line < 1 || it->line > src->line_count) return;
    int len;
    const char *text = source_line(src, it->line, &len);
    if (!text) return;

    /* Source line with a six-column gutter; the "|" lands at the
     * same offset as the caret row's bar. */
    fprintf(stderr, "%6d | %.*s\n", it->line, len, text);

    /* Caret run: `it->len` carets starting at column `it->col`. */
    int caret = it->col - 1;
    if (caret < 0) caret = 0;
    if (caret > len) caret = len;
    int width = it->len;
    if (caret + width > len) width = len - caret > 0 ? len - caret : 1;
    fprintf(stderr, "       | %s%*s", accent, caret, "");
    for (int i = 0; i < width; i++) fputc('^', stderr);
    fprintf(stderr, "%s\n", c_reset);
}

void diag_print_all(const Diag *d, const struct Source *src) {
    for (int i = 0; i < d->count; i++) {
        const Diagnostic *it = &d->items[i];
        const char *accent = it->kind == DK_ERROR ? c_red : c_yellow;
        const char *label = it->kind == DK_ERROR ? "error" : "warning";
        fprintf(stderr, "%s:%d:%d: %s%s:%s %s:\n",
                diag_path(it->path), it->line, it->col,
                accent, label, c_reset, it->msg);
        print_snippet(src, it, accent);
    }
    if (d->overflow)
        fprintf(stderr, "%s%s: error: too many errors, aborting%s\n",
                c_red, diag_path(d->count ? d->items[0].path : "<input>"),
                c_reset);
}

void diag_free(Diag *d) {
    for (int i = 0; i < d->count; i++) free(d->items[i].msg);
    free(d->items);
    memset(d, 0, sizeof(*d));
}
