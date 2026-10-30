#include "common.h"
#include "diag.h"
#include "source.h"
#include "color.h"

/*
 * diag.c - diagnostics storage and rendering.
 * Messages are rendered into a fixed buffer at report time, so callers
 * do not need to keep their format arguments alive. Rendering uses the
 * shared colour state: dim gutters, a coloured severity label and a
 * caret run of the same colour under the offending token.
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

/* Renders one source line, a caret run under the offending token and
 * a closing gutter bar, in the shape:
 *       10 | edgf}
 *          |  ^^^^
 *          |        */
static void print_snippet(const Source *src, const Diagnostic *it,
                          const char *accent) {
    if (!src || it->line < 1 || it->line > src->line_count) return;
    int len;
    const char *text = source_line(src, it->line, &len);
    if (!text) return;

    /* Source line with a six-column dim gutter. */
    fprintf(stderr, "%s %6d | %s%.*s%s\n",
            c_dim, it->line, c_reset, len, text, c_reset);

    /* Caret run: `it->len` carets starting at column `it->col`. */
    int caret = it->col - 1;
    if (caret < 0) caret = 0;
    if (caret > len) caret = len;
    int width = it->len;
    if (caret + width > len) width = len - caret > 0 ? len - caret : 1;
    fprintf(stderr, "%s       | %s%*s", c_dim, accent, caret, "");
    for (int i = 0; i < width; i++) fputc('^', stderr);
    fprintf(stderr, "%s\n", c_reset);

    /* Closing gutter bar, matching the requested shape. */
    fprintf(stderr, "%s       |%s\n", c_dim, c_reset);
}

void diag_print_all(const Diag *d, const struct Source *src) {
    for (int i = 0; i < d->count; i++) {
        const Diagnostic *it = &d->items[i];
        const char *accent = it->kind == DK_ERROR ? c_red : c_yellow;
        const char *label = it->kind == DK_ERROR ? "error" : "warning";
        fprintf(stderr, "%s%s:%d:%d:%s %s%s%s: %s:\n",
                c_dim, it->path, it->line, it->col, c_reset,
                accent, label, c_reset, it->msg);
        print_snippet(src, it, accent);
    }
    if (d->overflow)
        fprintf(stderr, "%s%s: error: too many errors, aborting%s\n",
                c_red, d->count ? d->items[0].path : "<input>", c_reset);
}

void diag_free(Diag *d) {
    for (int i = 0; i < d->count; i++) free(d->items[i].msg);
    free(d->items);
    memset(d, 0, sizeof(*d));
}
