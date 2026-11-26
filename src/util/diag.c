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
    it->note = NULL;
    it->help = NULL;
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

void diag_report_detail(Diag *d, DiagKind kind, const char *path,
                        int line, int col, int len,
                        const char *note, const char *help,
                        const char *fmt, ...) {
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
    Diagnostic *it = &d->items[d->count - 1];
    if (note) {
        it->note = malloc(strlen(note) + 1);
        if (it->note) strcpy(it->note, note);
    }
    if (help) {
        it->help = malloc(strlen(help) + 1);
        if (it->help) strcpy(it->help, help);
    }
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

/* Prints a compact source window around the diagnostic. The location
 * header owns the file and severity; source gutters stay neutral so
 * colour is reserved for the severity and the exact highlighted span. */
static void print_source_line(const Source *src, int line,
                              const Diagnostic *it, const char *accent,
                              int mark) {
    if (!src) return;
    int len = 0;
    const char *text = source_line(src, line, &len);
    if (!text) return;
    fprintf(stderr, "    %4d | %.*s\n", line, len, text);
    if (!mark) return;
    int caret = it->col - 1;
    if (caret < 0) caret = 0;
    if (caret > len) caret = len;
    int width = it->len;
    if (caret + width > len) width = len - caret > 0 ? len - caret : 1;
    fprintf(stderr, "         | %s%*s", accent, caret, "");
    for (int i = 0; i < width; i++) fputc('^', stderr);
    fprintf(stderr, "%s\n", c_reset);
}

/* Finds the innermost brace-delimited source body containing a line.
 * Strings and comments are skipped so braces inside literals do not
 * distort the context window shown for a diagnostic. */
static void source_body_span(const Source *src, int target,
                             int *first, int *last) {
    int stack[256], depth = 0, best = 0;
    int line = 1, close_line = 0;
    enum { SC_NORMAL, SC_LINE, SC_BLOCK, SC_STRING, SC_CHAR } state = SC_NORMAL;
    int escaped = 0;
    *first = *last = 0;
    for (size_t i = 0; i < src->len; i++) {
        char c = src->text[i];
        char next = i + 1 < src->len ? src->text[i + 1] : '\0';
        if (state == SC_LINE) {
            if (c == '\n') { state = SC_NORMAL; line++; }
            continue;
        }
        if (state == SC_BLOCK) {
            if (c == '*' && next == '/') { state = SC_NORMAL; i++; continue; }
            if (c == '\n') line++;
            continue;
        }
        if (state == SC_STRING || state == SC_CHAR) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if ((state == SC_STRING && c == '"') ||
                     (state == SC_CHAR && c == '\'')) state = SC_NORMAL;
            if (c == '\n') line++;
            continue;
        }
        if (c == '/' && next == '/') { state = SC_LINE; i++; continue; }
        if (c == '/' && next == '*') { state = SC_BLOCK; i++; continue; }
        if (c == '"') { state = SC_STRING; continue; }
        if (c == '\'') { state = SC_CHAR; continue; }
        if (c == '{') {
            if (depth < (int)(sizeof(stack) / sizeof(stack[0])))
                stack[depth++] = line;
        } else if (c == '}' && depth > 0) {
            int opening = stack[--depth];
            if (opening <= target && target <= line && opening >= best) {
                best = opening;
                close_line = line;
            }
        }
        if (c == '\n') line++;
    }
    if (best && close_line) { *first = best; *last = close_line; }
}

void diag_print_all(const Diag *d, const struct Source *src) {
    int nerr = 0, nwarn = 0;
    for (int i = 0; i < d->count; i++) {
        const Diagnostic *it = &d->items[i];
        const char *accent = it->kind == DK_ERROR ? c_red : c_yellow;
        const char *label = it->kind == DK_ERROR ? "error" : "warning";
        fprintf(stderr, "%s: %s%s%s: %s:\n",
                diag_path(it->path), accent, label, c_reset, it->msg);
        Source loaded;
        const Source *shown = src;
        int owns_source = 0;
        if (!shown && it->path && it->path[0] != '<' &&
            source_load(&loaded, it->path) == 0) {
            shown = &loaded;
            owns_source = 1;
        }
        int body_first = 0, body_last = 0;
        if (shown) source_body_span(shown, it->line, &body_first, &body_last);
        if (shown && body_first > 0 && body_first < it->line - 1) {
            print_source_line(shown, body_first, it, accent, 0);
            fprintf(stderr, "         | ...\n");
        } else if (shown && it->line > 1) {
            print_source_line(shown, it->line - 1, it, accent, 0);
        }
        print_source_line(shown, it->line, it, accent, 1);
        if (shown && it->line < shown->line_count) {
            if (body_last > it->line + 1) {
                fprintf(stderr, "         | ...\n");
                print_source_line(shown, body_last, it, accent, 0);
            } else {
                print_source_line(shown, it->line + 1, it, accent, 0);
            }
        }
        if (owns_source) source_free(&loaded);
        if (it->note) fprintf(stderr, "%snote%s: %s\n", c_cyan, c_reset, it->note);
        if (it->help) fprintf(stderr, "%shelp%s: %s\n", c_green, c_reset, it->help);
        if (it->kind == DK_ERROR) nerr++; else nwarn++;
        /* Keep independent diagnostics visually distinct. */
        if (i + 1 < d->count) fputc('\n', stderr);
    }
    if (nerr || nwarn) {
        fprintf(stderr, "%d error%s, %d warning%s\n",
                nerr, nerr == 1 ? "" : "s",
                nwarn, nwarn == 1 ? "" : "s");
        /* Keep later backend/linker diagnostics visually separate from
         * the frontend diagnostic summary. */
        fputc('\n', stderr);
    }
    if (d->overflow)
        fprintf(stderr, "%s: error: too many errors, aborting\n",
                diag_path(d->count ? d->items[0].path : "<input>"));
}

void diag_free(Diag *d) {
    for (int i = 0; i < d->count; i++) {
        free(d->items[i].msg);
        free(d->items[i].note);
        free(d->items[i].help);
    }
    free(d->items);
    memset(d, 0, sizeof(*d));
}
