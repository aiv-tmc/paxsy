#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "commands.h"
#include "cli.h"
#include "diag.h"
#include "nemit/nlink.h"

/*
 * cli/cmd_link.c - `paxsy link <output> -- <objects...>`.
 *
 * Links relocatable objects (and ar archives of them) into a
 * statically linked x86-64 executable without invoking any external
 * tool: the linker reads the toolchain's own object format, resolves
 * the global symbols, applies the relocations and writes the image
 * through the shared ELF writer.
 */

int cmd_link(const Cli *cli) {
    if (!cli->path || cli->rest_count == 0) {
        fprintf(stderr,
                "usage: paxsy link <output> -- <objects...>\n");
        return 1;
    }
    Diag d;
    diag_init(&d, cli->max_errors);
    char **objs = calloc((size_t)cli->rest_count, sizeof(char *));
    for (int i = 0; i < cli->rest_count; i++)
        objs[i] = (char *)cli->rest[i];
    int rc = nlink_exec(&d, cli->path, objs, cli->rest_count);
    if (diag_error_count(&d) > 0)
        diag_print_all(&d, NULL);
    diag_free(&d);
    free(objs);
    return rc;
}
