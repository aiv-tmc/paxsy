/* fileno and isatty require POSIX feature macros under -std=c99. */
#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "color.h"

#include <unistd.h>

/*
 * color.c - colour state. Enabled only when stderr is a terminal and
 * the NO_COLOR environment variable is absent; every global points to
 * the empty string otherwise, which keeps call sites branch-free.
 */

const char *c_red;
const char *c_green;
const char *c_yellow;
const char *c_cyan;
const char *c_dim;
const char *c_reset;

void color_setup(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    if (getenv("NO_COLOR") || !isatty(fileno(stderr))) {
        c_red = c_green = c_yellow = c_cyan = c_dim = c_reset = "";
        return;
    }
    c_red = "\033[91m";
    c_green = "\033[92m";
    c_yellow = "\033[93m";
    c_cyan = "\033[96m";
    c_dim = "\033[90m";
    c_reset = "\033[0m";
}
