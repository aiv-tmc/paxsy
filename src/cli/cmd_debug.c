#define _POSIX_C_SOURCE 200809L
/*
 * cmd_debug.c - 'paxsy debug': the integrated debugging session.
 *
 * The debugger is ours: a ptrace session over the project's binary
 * with the symbol table the compiler wrote. Breakpoints, stepping,
 * register inspection and trace policy are read from the project's
 * [debug] TOML table; ordinary use is simply `paxsy debug [path]`.
 *
 * The project is rebuilt first so the binary matches the sources.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "common.h"
#include "commands.h"
#include "cli.h"
#include "toml.h"
#include "output_path.h"


int cmd_builtin_debug_flags(const Cli *cli, const char *bin);

/* Resolves the project executable from its configuration file. */
static int find_binary(const char *root, char *bin, size_t bn) {
    char cfg[PAXSY_PATH_MAX];
    char name[128];
    struct stat st;
    if (toml_find(root, cfg, sizeof(cfg)) != 0) return -1;
    if (!toml_get(cfg, "project", "name", name, sizeof(name))) {
        const char *base = strrchr(cfg, '/');
        base = base ? base + 1 : cfg;
        size_t n = strlen(base);
        snprintf(name, sizeof(name), "%.*s", (int)(n > 5 ? n - 5 : n), base);
    }
    if (paxsy_output_path(root, cfg, name, 0, bin, bn) != 0) return -1;
    return stat(bin, &st) == 0 && S_ISREG(st.st_mode) ? 0 : -1;
}

static int config_bool(const char *cfg, const char *key) {
    char value[16];
    return toml_get(cfg, "debug", key, value, sizeof(value)) &&
           strcmp(value, "true") == 0;
}

static void free_added_breakpoints(Cli *debug_cli, const Cli *original) {
    for (int i = original->nbreaks; i < debug_cli->nbreaks; i++)
        free((void *)debug_cli->breaks[i]);
}

static int config_breakpoints(const char *cfg, Cli *cli) {
    char value[512];
    if (!toml_get(cfg, "debug", "breakpoints", value, sizeof(value))) return 0;
    for (char *part = strtok(value, ","); part; part = strtok(NULL, ",")) {
        char *end;
        char *copy;
        while (*part == ' ' || *part == '\t') part++;
        end = part + strlen(part);
        while (end > part && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!*part) continue;
        if (cli->nbreaks >= 16) {
            fprintf(stderr, "[debug].breakpoints supports at most 16 names\n");
            return -1;
        }
        copy = strdup(part);
        if (!copy) {
            fprintf(stderr, "debug: out of memory while reading breakpoints\n");
            return -1;
        }
        cli->breaks[cli->nbreaks++] = copy;
    }
    return 0;
}


int cmd_debug(const Cli *cli) {
    const char *root = cli->path ? cli->path : ".";
    char bin[PAXSY_PATH_MAX];
    Cli b = *cli;
    int rc;
    char project_cfg[PAXSY_PATH_MAX], enabled[16];

    if (toml_find(root, project_cfg, sizeof(project_cfg)) != 0) {
        fprintf(stderr, "debug: no project configuration in '%s'\n", root);
        return 2;
    }
    if (toml_get(project_cfg, "debug", "enabled", enabled, sizeof(enabled)) &&
        strcmp(enabled, "false") == 0) {
        fprintf(stderr, "debug: disabled by [debug].enabled = false\n");
        return 2;
    }
    b.path = root;
    snprintf(b.target, sizeof(b.target), "exe");
    rc = cmd_build(&b);
    if (rc != 0) {
        fprintf(stderr, "debug: build failed\n");
        return 1;
    }
    if (find_binary(root, bin, sizeof(bin)) != 0) {
        fprintf(stderr, "debug: no binary under '%s/build'\n", root);
        return 1;
    }
    Cli debug_cli = *cli;
    char cfg[PAXSY_PATH_MAX], value[256];
    char configured_trace_path[PAXSY_PATH_MAX];
    if (toml_find(root, cfg, sizeof(cfg)) == 0) {
        debug_cli.dbg_trace_memory |= config_bool(cfg, "trace_memory");
        debug_cli.dbg_trace_registers |= config_bool(cfg, "trace_registers");
        debug_cli.dbg_trace_all |= config_bool(cfg, "trace_all");
        debug_cli.dbg_regs |= config_bool(cfg, "regs");
        debug_cli.dbg_run |= config_bool(cfg, "run");
        debug_cli.dbg_run_explicit |= config_bool(cfg, "run");
        if (debug_cli.dbg_stepi == 0 &&
            toml_get(cfg, "debug", "stepi", value, sizeof(value))) {
            char *end = NULL;
            long steps = strtol(value, &end, 10);
            if (end == value || !end || *end || steps < 0 || steps > 1000000) {
                fprintf(stderr, "invalid [debug].stepi value\n");
                return 2;
            }
            debug_cli.dbg_stepi = (int)steps;
        }
        if (debug_cli.nbreaks == 0 &&
            config_breakpoints(cfg, &debug_cli) != 0) {
            free_added_breakpoints(&debug_cli, cli);
            return 2;
        }
        if (!debug_cli.dbg_trace_file &&
            toml_get(cfg, "debug", "trace_file", value, sizeof(value)) &&
            value[0]) {
            int path_len = value[0] == '/'
                ? snprintf(configured_trace_path, sizeof(configured_trace_path),
                           "%s", value)
                : snprintf(configured_trace_path, sizeof(configured_trace_path),
                           "%s/%s", root, value);
            if (path_len < 0 || (size_t)path_len >= sizeof(configured_trace_path)) {
                fprintf(stderr, "[debug].trace_file path is too long\n");
                free_added_breakpoints(&debug_cli, cli);
                return 2;
            }
            debug_cli.dbg_trace_file = configured_trace_path;
        }
    }
    if (debug_cli.dbg_stepi <= 0 && !debug_cli.dbg_run)
        debug_cli.dbg_run = 1;
    rc = cmd_builtin_debug_flags(&debug_cli, bin);
    free_added_breakpoints(&debug_cli, cli);
    return rc;
}
