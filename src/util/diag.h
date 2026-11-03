#ifndef PAXSY_DIAG_H
#define PAXSY_DIAG_H

/*
 * diag.h - central diagnostics manager.
 * All lexer, parser and semantic errors flow through a single Diag
 * instance so the driver can report them with source excerpts and
 * enforce an error limit instead of stopping at the first failure.
 * Each diagnostic carries the length of the offending token so the
 * printer can underline it with carets. Warnings can be suppressed
 * wholesale (the --ignore-warnings gate); suppressed warnings are
 * never stored, so they are neither printed nor counted anywhere.
 */

/* Forward declaration: diag.c pairs this with the definition in source.h. */
struct Source;

typedef enum { DK_ERROR, DK_WARNING } DiagKind;

typedef struct {
    DiagKind kind;
    const char *path;
    int line, col;
    int len;        /* length of the offending token, >= 1 */
    char *msg;      /* formatted, heap-owned */
} Diagnostic;

typedef struct {
    Diagnostic *items;
    int count, cap;
    int max_errors;        /* error reports beyond this are dropped */
    int overflow;          /* set when the limit was hit */
    int suppress_warnings; /* when set, warning reports are discarded */
} Diag;

void diag_init(Diag *d, int max_errors);
void diag_free(Diag *d);

/* Formatted report; varargs message is captured immediately. */
void diag_report(Diag *d, DiagKind kind, const char *path,
                 int line, int col, const char *fmt, ...);

/* Same as diag_report, but underlines `len` characters. */
void diag_report_full(Diag *d, DiagKind kind, const char *path,
                      int line, int col, int len, const char *fmt, ...);

int diag_error_count(const Diag *d);
int diag_warning_count(const Diag *d);

/* Prints every stored diagnostic to stderr. When src is not NULL a
 * source excerpt is shown: the offending line, a caret run under the
 * reported token and a closing gutter bar. */
void diag_print_all(const Diag *d, const struct Source *src);

#endif /* PAXSY_DIAG_H */
