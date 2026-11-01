#define _POSIX_C_SOURCE 200809L

/*
 * pxrt_ext.c - the print shims of the Paxsy runtime.
 *
 * The C backend emits its own private println helpers, but the LLVM
 * assembly (--emit-llvm) and MLIR (--emit-mlir) units declare the two
 * entry points below as extern and link against this file together
 * with the channel runtime in pxrt.c. Both forms follow the
 * std::inout::println contract: the value is printed followed by a
 * single newline; errors of the underlying stream are ignored, which
 * matches the frontend-only error model where println reports its
 * failures through the T! result handled by the caller.
 */

#include "pxrt.h"

#include <stdio.h>

/* Prints one signed 32-bit integer and a newline. */
void paxsy_print_i32(int v) {
    printf("%d\n", v);
}

/* Prints one NUL-terminated byte string and a newline. */
void paxsy_print_str(const char *s) {
    fputs(s, stdout);
    fputc('\n', stdout);
}
