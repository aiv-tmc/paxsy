#define _POSIX_C_SOURCE 200809L

/*
 * cmd_disasm.c - readable, annotated machine-code inspection.
 *
 * GNU objdump performs instruction decoding. Paxsy owns the process
 * invocation, project/artifact discovery, structured parsing, stable
 * formatting, and conservative semantic comments. No shell is involved:
 * user-supplied paths, symbols, and section names are passed as argv
 * elements to execvp().
 */

#include "common.h"
#include "commands.h"
#include "cli.h"
#include "color.h"
#include "output_path.h"
#include "toml.h"
#include "fsutil.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DISASM_LINE_MAX 4096
#define DISASM_ASM_MAX  2048
#define DISASM_BYTES_MAX 128

static const char *path_base(const char *path) {
    const char *slash;
    size_t n;
    if (!path || !*path) return "app";
    n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    slash = path + n;
    while (slash > path && slash[-1] != '/') slash--;
    if (*slash) return slash;
    return "app";
}

static int path_is_regular(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int path_is_directory(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Resolve either an explicit binary path or a project's configured
 * build.output. The latter is the same source of truth used by build/run. */
static int project_binary(const char *root, char *binary, size_t cap) {
    char cfg[PAXSY_PATH_MAX];
    char project[128];
    int pe = 0;

    if (toml_find(root, cfg, (int)sizeof(cfg)) == 0) {
        if (!toml_get(cfg, "project", "name", project,
                      (int)sizeof(project)) || !project[0]) {
            const char *base = path_base(cfg);
            size_t n = strlen(base);
            snprintf(project, sizeof(project), "%.*s",
                     (int)(n > 5 ? n - 5 : n), base);
        }
        {
            char target[64];
            if (toml_get(cfg, "build", "target", target,
                         (int)sizeof(target)) &&
                (strcmp(target, "pe") == 0 ||
                 strstr(target, "windows") != NULL))
                pe = 1;
        }
        return paxsy_output_path(root, cfg, project, pe, binary, cap);
    }

    /* A conventional fallback is useful for hand-written projects that
     * have not adopted a project TOML file yet. */
    {
        const char *base = path_base(root);
        int n;
        if (strcmp(base, ".") == 0 || strcmp(base, "/") == 0)
            base = "app";
        n = snprintf(binary, cap, "%s/build/%s", root, base);
        return n > 0 && (size_t)n < cap ? 0 : -1;
    }
}

static int resolve_binary(const Cli *cli, char *binary, size_t cap,
                          char *project_root, size_t root_cap) {
    const char *input = cli->path ? cli->path : ".";

    if (path_is_regular(input)) {
        if (snprintf(binary, cap, "%s", input) >= (int)cap) return -1;
        project_root[0] = 0;
        return 0;
    }
    if (!path_is_directory(input)) return -1;

    if (snprintf(project_root, root_cap, "%s", input) >= (int)root_cap)
        return -1;
    return project_binary(project_root, binary, cap);
}

/* Return 1 for x86 ELF/PE, 0 when architecture cannot be identified,
 * and -1 for a known non-x86 architecture. Unknown formats retain
 * objdump's native syntax rather than receiving an invalid Intel flag. */
static int binary_is_x86(const char *path) {
    unsigned char h[64];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(h, 1, sizeof(h), f);
    fclose(f);
    if (n >= 20 && memcmp(h, "\x7f" "ELF", 4) == 0) {
        unsigned machine;
        if (h[5] == 1)
            machine = (unsigned)h[18] | ((unsigned)h[19] << 8);
        else if (h[5] == 2)
            machine = ((unsigned)h[18] << 8) | (unsigned)h[19];
        else
            return 0;
        if (machine == 3 || machine == 62) return 1; /* i386 / x86-64 */
        return -1;
    }
    if (n >= 64 && h[0] == 'M' && h[1] == 'Z') {
        uint32_t peoff = (uint32_t)h[0x3c] |
                         ((uint32_t)h[0x3d] << 8) |
                         ((uint32_t)h[0x3e] << 16) |
                         ((uint32_t)h[0x3f] << 24);
        unsigned char pe[6];
        if ((uint64_t)peoff + sizeof(pe) > (uint64_t)LONG_MAX)
            return 0;
        f = fopen(path, "rb");
        if (!f) return 0;
        if (fseek(f, (long)peoff, SEEK_SET) != 0 ||
            fread(pe, 1, sizeof(pe), f) != sizeof(pe)) {
            fclose(f);
            return 0;
        }
        fclose(f);
        if (memcmp(pe, "PE\0\0", 4) != 0) return 0;
        if (pe[4] == 0x4c && pe[5] == 0x01) return 1; /* i386 */
        if (pe[4] == 0x64 && pe[5] == 0x86) return 1; /* x86-64 */
        return -1;
    }
    return 0;
}

static FILE *objdump_open(const char *binary, int x86, pid_t *child_pid) {
    int fds[2];
    pid_t pid;
    FILE *stream;
    char *argv[] = {
        (char *)"objdump", (char *)"-d", (char *)"--show-raw-insn",
        (char *)"--disassemble-zeroes", (char *)"--wide", (char *)"-C",
        (char *)"-M", (char *)"intel", (char *)binary, NULL
    };
    char *generic_argv[] = {
        (char *)"objdump", (char *)"-d", (char *)"--show-raw-insn",
        (char *)"--disassemble-zeroes", (char *)"--wide", (char *)"-C",
        (char *)binary, NULL
    };
    char **args = x86 ? argv : generic_argv;
    if (pipe(fds) != 0) return NULL;
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(126);
        close(fds[1]);
        execvp(args[0], args);
        fprintf(stderr, "disasm: cannot execute objdump: %s\n", strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return NULL; }
    stream = fdopen(fds[0], "r");
    if (!stream) {
        int status;
        close(fds[0]);
        waitpid(pid, &status, 0);
        return NULL;
    }
    *child_pid = pid;
    return stream;
}

static int objdump_close(FILE *stream, pid_t child_pid) {
    int status = 0;
    int close_rc = fclose(stream);
    if (waitpid(child_pid, &status, 0) < 0 || close_rc != 0 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}

static char *skip_space(char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int is_hex_token(const char *p, size_t n) {
    size_t i;
    if (n == 0 || (n & 1u) != 0 || n > 16) return 0;
    for (i = 0; i < n; i++)
        if (!isxdigit((unsigned char)p[i])) return 0;
    return 1;
}

/* Parse an objdump instruction row into address, encoded bytes, and
 * assembly text. Byte groups are kept exactly as printed, which also
 * accommodates fixed-width encodings on non-x86 targets. */
static int parse_instruction(char *line, uint64_t *address,
                              char *bytes, size_t bytes_cap,
                              char *assembly, size_t assembly_cap) {
    char *p = skip_space(line), *end;
    unsigned long long a;
    size_t used = 0;
    int groups = 0;

    errno = 0;
    a = strtoull(p, &end, 16);
    if (end == p || errno != 0 || *end != ':') return 0;
    p = skip_space(end + 1);
    bytes[0] = 0;

    while (*p) {
        char *token = p;
        size_t len;
        while (*p && !isspace((unsigned char)*p)) p++;
        len = (size_t)(p - token);
        if (!is_hex_token(token, len)) {
            p = token;
            break;
        }
        if (used + len + (groups ? 1u : 0u) >= bytes_cap) return 0;
        if (groups) bytes[used++] = ' ';
        memcpy(bytes + used, token, len);
        used += len;
        bytes[used] = 0;
        groups++;
        p = skip_space(p);
    }
    if (!groups || !*p) return 0;
    if (snprintf(assembly, assembly_cap, "%s", p) >=
        (int)assembly_cap)
        return 0;
    *address = (uint64_t)a;
    return 1;
}

static int parse_symbol_header(char *line, uint64_t *address,
                               char *symbol, size_t symbol_cap) {
    char *p = skip_space(line), *end, *lt, *gt;
    unsigned long long a;
    errno = 0;
    a = strtoull(p, &end, 16);
    if (end == p || errno != 0 || *end != ' ' ||
        !(lt = strchr(end, '<')) || !(gt = strrchr(lt, '>')) ||
        gt[1] != ':' || gt[2] != '\0')
        return 0;
    if ((size_t)(gt - lt - 1) >= symbol_cap) return 0;
    memcpy(symbol, lt + 1, (size_t)(gt - lt - 1));
    symbol[gt - lt - 1] = 0;
    *address = (uint64_t)a;
    return 1;
}

static void lower_ascii(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static void split_assembly(const char *assembly, char *mnemonic,
                           size_t mnemonic_cap, char *operands,
                           size_t operands_cap) {
    const char *p = assembly;
    size_t n = 0;
    while (*p && !isspace((unsigned char)*p) && n + 1 < mnemonic_cap)
        mnemonic[n++] = *p++;
    mnemonic[n] = 0;
    while (*p && isspace((unsigned char)*p)) p++;
    snprintf(operands, operands_cap, "%s", p);
    lower_ascii(mnemonic);
}

static void print_symbol(FILE *out, uint64_t address, const char *symbol) {
    fprintf(out, "; %016" PRIx64 " <%s>\n", address, symbol);
}

static void print_instruction(FILE *out, uint64_t address, const char *bytes,
                              const char *assembly, int annotate_x86) {
    char mnemonic[64], operands[DISASM_ASM_MAX];
    split_assembly(assembly, mnemonic, sizeof(mnemonic),
                   operands, sizeof(operands));
    fprintf(out, "  %016" PRIx64 "  %-45s  %-9s", address, bytes, mnemonic);
    if (operands[0]) fprintf(out, " %s", operands);
    (void)annotate_x86;
    fputc('\n', out);
}

static int open_function_file(const char *outdir, const char *symbol,
                              FILE **current, char *current_path,
                              size_t path_cap) {
    char safe[192];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)symbol;
         *p && n + 1 < sizeof(safe); p++) {
        if (isalnum(*p) || *p == '_') safe[n++] = (char)*p;
        else if (n == 0 || safe[n - 1] != '_') safe[n++] = '_';
    }
    while (n && safe[n - 1] == '_') n--;
    if (!n) { memcpy(safe, "function", 8); n = 8; }
    safe[n] = 0;
    if (*current) { fclose(*current); *current = NULL; }
    if (snprintf(current_path, path_cap, "%s/%s.asm", outdir, safe) >=
        (int)path_cap) return -1;
    *current = fopen(current_path, "w");
    if (!*current) return -1;
    return 0;
}

static int disassemble_file(const char *binary, int x86, const char *syntax,
                            const char *outdir, int quiet) {
    FILE *in, *current = NULL;
    pid_t child_pid = -1;
    char line[DISASM_LINE_MAX], symbol[1024], current_path[PAXSY_PATH_MAX] = "";
    size_t instructions = 0, sections = 0, labels = 0, files = 0;
    int read_error = 0, close_rc;
    in = objdump_open(binary, x86, &child_pid);
    if (!in) {
        fprintf(stderr, "disasm: could not start objdump (check that GNU binutils is installed)\n");
        return 2;
    }
    while (fgets(line, sizeof(line), in)) {
        char *p = skip_space(line);
        size_t n = strlen(p);
        uint64_t address;
        char bytes[DISASM_BYTES_MAX], assembly[DISASM_ASM_MAX];
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r')) p[--n] = 0;
        if (!*p) continue;
        if (strncmp(p, "Disassembly of section ", 23) == 0) {
            sections++;
            continue;
        }
        if (parse_symbol_header(p, &address, symbol, sizeof(symbol))) {
            labels++;
            if (open_function_file(outdir, symbol, &current,
                                   current_path, sizeof(current_path)) != 0) {
                fprintf(stderr, "disasm: cannot create output for symbol '%s'\n", symbol);
                read_error = 1;
                break;
            }
            (void)syntax;
            print_symbol(current, address, symbol);
            files++;
            continue;
        }
        if (parse_instruction(p, &address, bytes, sizeof(bytes),
                              assembly, sizeof(assembly))) {
            /* Instructions outside a named symbol are retained in a
             * dedicated file instead of silently discarded. */
            if (!current) {
                if (open_function_file(outdir, "startup", &current,
                                       current_path, sizeof(current_path)) != 0) {
                    read_error = 1;
                    break;
                }
                files++;
            }
            print_instruction(current, address, bytes, assembly, x86);
            instructions++;
        }
    }
    if (ferror(in)) read_error = 1;
    close_rc = objdump_close(in, child_pid);
    if (current && fclose(current) != 0) read_error = 1;
    if (read_error || close_rc != 0) {
        fprintf(stderr, "disasm: objdump failed while decoding '%s'\n", binary);
        return 1;
    }
    if (instructions == 0) {
        fprintf(stderr, "disasm: no instructions were produced; check the file\n");
        return 1;
    }
    if (!quiet) {
        printf("disassembled %zu instructions into %s/\n", instructions, outdir);
        printf("  %zu function/source files, %zu symbol labels, %zu sections\n",
               files, labels, sections);
    }
    return 0;
}

int paxsy_disassemble_to_dir(const char *binary, const char *outdir) {
    int x86 = binary_is_x86(binary) == 1;
    const char *syntax = x86 ? "Intel" : "objdump default for target architecture";
    if (!binary || !outdir || !path_is_regular(binary) ||
        !path_is_directory(outdir)) {
        fprintf(stderr, "disasm: invalid binary or output directory\n");
        return 1;
    }
    return disassemble_file(binary, x86, syntax, outdir, 1);
}

int cmd_disasm(const Cli *cli) {
    const char *input = cli->path ? cli->path : ".";
    char binary[PAXSY_PATH_MAX];
    char project_root[PAXSY_PATH_MAX];
    int x86;
    const char *syntax;
    project_root[0] = 0;
    if (resolve_binary(cli, binary, sizeof(binary), project_root,
                       sizeof(project_root)) != 0 || !path_is_regular(binary)) {
        if (project_root[0] == 0) {
            fprintf(stderr, "disasm: '%s' is not a regular file or project directory\n", input);
            return 1;
        }
        {
            Cli build_cli = *cli;
            build_cli.path = project_root;
            if (cmd_build(&build_cli) != 0) {
                fprintf(stderr, "disasm: project build failed\n");
                return 1;
            }
        }
        if (project_binary(project_root, binary, sizeof(binary)) != 0 ||
            !path_is_regular(binary)) {
            fprintf(stderr, "disasm: configured binary was not produced under '%s'\n", project_root);
            return 1;
        }
    }
    x86 = binary_is_x86(binary) == 1;
    syntax = x86 ? "Intel" : "objdump default for target architecture";
    color_setup();
    {
        char output_root[PAXSY_PATH_MAX], outdir[PAXSY_PATH_MAX];
        const char *base = path_base(binary);
        if (snprintf(output_root, sizeof(output_root), "%s.disasm", base) >=
            (int)sizeof(output_root) ||
            snprintf(outdir, sizeof(outdir), "%s/src", output_root) >=
            (int)sizeof(outdir)) {
            fprintf(stderr, "disasm: output path is too long\n");
            return 1;
        }
        if ((access(output_root, F_OK) == 0 && fs_remove_tree(output_root) != 0) ||
            fs_mkdir_p(outdir) != 0) {
            fprintf(stderr, "disasm: cannot prepare output directory '%s'\n", outdir);
            return 1;
        }
        return disassemble_file(binary, x86, syntax, outdir, 0);
    }
}
