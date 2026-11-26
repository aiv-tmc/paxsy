#define _GNU_SOURCE
#include "common.h"
#include "commands.h"
#include "cli.h"
#include "recovery_int.h"
#include "recovery_metadata.h"
#include "fsutil.h"
#include "toml.h"
#include "output_path.h"

#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_recovered_file(const char *path,
                                const unsigned char *data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    size_t done = 0;
    if (fd < 0) return -1;
    while (done < size) {
        ssize_t n = write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); unlink(path); return -1; }
        done += (size_t)n;
    }
    {
        int failed = fsync(fd) != 0;
        if (close(fd) != 0) failed = 1;
        if (failed) { unlink(path); return -1; }
    }
    return 0;
}


static void project_name_from_binary(const char *bin, char *name, size_t cap) {
    const char *base = strrchr(bin, '/');
    size_t n = 0;
    base = base ? base + 1 : bin;
    while (*base && n + 1 < cap) {
        unsigned char c = (unsigned char)*base++;
        if (isalnum(c) || c == '_') name[n++] = (char)tolower(c);
        else if (n && name[n - 1] != '_') name[n++] = '_';
    }
    while (n && name[n - 1] == '_') n--;
    if (!n) { snprintf(name, cap, "recovered"); return; }
    if (isdigit((unsigned char)name[0]) && n + 2 < cap) {
        memmove(name + 1, name, n);
        name[0] = 'p';
        n++;
    }
    name[n] = 0;
}

static int write_project_scaffold(const char *bin, const char *root) {
    char name[128], path[PAXSY_PATH_MAX], toml[1024];
    static const char test_source[] =
        "func test_recovered_project(): Void {\n    pass;\n}\n";
    static const char bench_source[] =
        "func bench_recovered_project(): Void {\n    pass;\n}\n";
    project_name_from_binary(bin, name, sizeof(name));
    if (fs_mkdir_p(root) != 0) return -1;
    const char *dirs[] = {"src", "tests", "benches", "assets", "docs"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (snprintf(path, sizeof(path), "%s/%s", root, dirs[i]) >= (int)sizeof(path) ||
            fs_mkdir_p(path) != 0) return -1;
    }
    if (snprintf(path, sizeof(path), "%s/tests/recovered_smoke.px", root) >= (int)sizeof(path) ||
        (access(path, F_OK) != 0 && write_recovered_file(path,
            (const unsigned char *)test_source, sizeof(test_source) - 1) != 0)) return -1;
    if (snprintf(path, sizeof(path), "%s/benches/recovered_smoke.px", root) >= (int)sizeof(path) ||
        (access(path, F_OK) != 0 && write_recovered_file(path,
            (const unsigned char *)bench_source, sizeof(bench_source) - 1) != 0)) return -1;
    if (snprintf(path, sizeof(path), "%s/%s.toml", root, name) >= (int)sizeof(path)) return -1;
    if (access(path, F_OK) == 0) return 0;
    snprintf(toml, sizeof(toml),
        "[project]\nname = \"%s\"\nversion = \"0.1.0\"\n"
        "description = \"Recovered Paxsy project\"\nauthors = []\n\n"
        "[build]\ntarget = \"elf\"\noptimization = \"3\"\n"
        "output = \"build/%s\"\n", name, name);
    return write_recovered_file(path, (const unsigned char *)toml, strlen(toml));
}

static int recover_embedded_sources(const char *bin,
                                    const PaxsySourceArchive *archive) {
    char output[PAXSY_PATH_MAX];
    if (snprintf(output, sizeof(output), "%s.recovered", bin) >=
        (int)sizeof(output)) return -1;
    if (mkdir(output, 0755) != 0) {
        fprintf(stderr,
                "recovery: output directory '%s' already exists or cannot be created\n",
                output);
        return -1;
    }
    for (size_t i = 0; i < archive->count; i++) {
        char path[PAXSY_PATH_MAX], parent[PAXSY_PATH_MAX];
        char *slash;
        if (snprintf(path, sizeof(path), "%s/%s", output,
                     archive->files[i].path) >= (int)sizeof(path))
            goto fail;
        snprintf(parent, sizeof(parent), "%s", path);
        slash = strrchr(parent, '/');
        if (slash) {
            *slash = 0;
            if (fs_mkdir_p(parent) != 0) goto fail;
        }
        if (write_recovered_file(path, archive->files[i].data,
                                 archive->files[i].size) != 0) goto fail;
    }
    if (write_project_scaffold(bin, output) != 0) goto fail;
    printf("Recovered %zu source files: %s\n", archive->count, output);
    return 0;
fail:
    fprintf(stderr, "recovery: failed while restoring source files\n");
    fs_remove_tree(output);
    return -1;
}

/* A binary without source metadata can only be recovered partially. */
static int recover_asm_sources(const char *bin) {
    char asm_dir[PAXSY_PATH_MAX], temp_root[PAXSY_PATH_MAX];
    char out_root[PAXSY_PATH_MAX], src_dir[PAXSY_PATH_MAX];
    DIR *dir;
    struct dirent *entry;
    {
        const char *base = strrchr(bin, '/');
        base = base ? base + 1 : bin;
        if (snprintf(out_root, sizeof(out_root), "%s.recovered", bin) >= (int)sizeof(out_root) ||
            snprintf(src_dir, sizeof(src_dir), "%s/src", out_root) >= (int)sizeof(src_dir) ||
            snprintf(temp_root, sizeof(temp_root), "%s/.recovery-asm", out_root) >= (int)sizeof(temp_root) ||
            snprintf(asm_dir, sizeof(asm_dir), "%s/src", temp_root) >= (int)sizeof(asm_dir))
            return -1;
    }
    if ((access(out_root, F_OK) == 0 && fs_remove_tree(out_root) != 0) ||
        fs_mkdir_p(src_dir) != 0 || fs_mkdir_p(asm_dir) != 0)
        return -1;
    /* Use the same decoder as `disasm`, but keep its intermediate .asm
     * files private to recovery instead of emitting a second public
     * <binary>.disasm project as an accidental side effect. */
    if (paxsy_disassemble_to_dir(bin, asm_dir) != 0) {
        fs_remove_tree(out_root);
        return -1;
    }
    dir = opendir(asm_dir);
    if (!dir) return -1;
    while ((entry = readdir(dir)) != NULL) {
        char in_path[PAXSY_PATH_MAX], out_path[PAXSY_PATH_MAX];
        char stem[192], function_name[224], line[4096];
        size_t n;
        FILE *in, *out;
        if (!strstr(entry->d_name, ".asm") ||
            strcmp(strrchr(entry->d_name, '.') ? strrchr(entry->d_name, '.') : "", ".asm") != 0)
            continue;
        n = strlen(entry->d_name) - 4;
        if (n >= sizeof(stem)) n = sizeof(stem) - 1;
        memcpy(stem, entry->d_name, n); stem[n] = 0;
        if (strcmp(stem, "main") == 0)
            snprintf(function_name, sizeof(function_name), "main");
        else
            snprintf(function_name, sizeof(function_name), "recovered_%s", stem);
        if (snprintf(in_path, sizeof(in_path), "%s/%s", asm_dir, entry->d_name) >= (int)sizeof(in_path) ||
            snprintf(out_path, sizeof(out_path), "%s/%s.px", src_dir, stem) >= (int)sizeof(out_path)) {
            closedir(dir); return -1;
        }
        in = fopen(in_path, "r");
        out = fopen(out_path, "w");
        if (!in || !out) {
            if (in) fclose(in);
            if (out) fclose(out);
            closedir(dir); return -1;
        }
        fprintf(out, "func %s(): Void {\n", function_name);
        while (fgets(line, sizeof(line), in)) {
            char *instruction;
            size_t len;
            if (strlen(line) < 67 || line[0] != ' ') continue;
            instruction = line + 67;
            while (*instruction == ' ') instruction++;
            len = strlen(instruction);
            while (len && (instruction[len - 1] == '\n' || instruction[len - 1] == '\r'))
                instruction[--len] = 0;
            {
                char *comment = strstr(instruction, "  ; ");
                if (comment) *comment = 0;
            }
            if (!*instruction) continue;
            fputs("    asm \"", out);
            for (char *q = instruction; *q; q++) {
                if (*q == '"' || *q == '\\') fputc('\\', out);
                if (*q == '\n') fputs("\n", out);
                else if (*q == '\r') fputs("\\r", out);
                else fputc(*q, out);
            }
            fputs("\";\n", out);
        }
        fputs("}\n", out);
        if (fclose(in) != 0 || fclose(out) != 0) {
            closedir(dir); return -1;
        }
    }
    closedir(dir);
    if (fs_remove_tree(temp_root) != 0) {
        fprintf(stderr, "recovery: could not remove private disassembly workspace\n");
        return -1;
    }
    if (write_project_scaffold(bin, out_root) != 0) return -1;
    printf("Recovered project: %s\n", out_root);
    return 0;
}

static int recovery_binary(const Cli *cli, char *binary, size_t cap) {
    const char *input = cli->path ? cli->path : ".";
    struct stat st;
    if (stat(input, &st) == 0 && S_ISREG(st.st_mode)) {
        return snprintf(binary, cap, "%s", input) < (int)cap ? 0 : -1;
    }
    if (stat(input, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    {
        char cfg[PAXSY_PATH_MAX], name[128], target[64] = "";
        if (toml_find(input, cfg, sizeof(cfg)) != 0) return -1;
        if (!toml_get(cfg, "project", "name", name, sizeof(name)) || !name[0]) {
            const char *base = strrchr(cfg, '/');
            base = base ? base + 1 : cfg;
            size_t n = strlen(base);
            snprintf(name, sizeof(name), "%.*s", (int)(n > 5 ? n - 5 : n), base);
        }
        int pe = toml_get(cfg, "build", "target", target, sizeof(target)) &&
                 (strcmp(target, "pe") == 0 || strstr(target, "windows") != NULL);
        if (paxsy_output_path(input, cfg, name, pe, binary, cap) != 0)
            return -1;
        if (stat(binary, &st) != 0 || !S_ISREG(st.st_mode)) {
            Cli build_cli = *cli;
            build_cli.path = input;
            if (cmd_build(&build_cli) != 0) return -1;
            if (stat(binary, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
        }
    }
    return 0;
}

int cmd_recovery(const Cli *cli) {
    char binary[PAXSY_PATH_MAX];
    PaxsySourceArchive archive;
    int extracted;
    if (recovery_binary(cli, binary, sizeof(binary)) != 0) {
        fprintf(stderr,
                "recovery: expected a binary or a project directory with a configured build output\n");
        return 2;
    }
    extracted = paxsy_extract_sources(binary, &archive);
    if (extracted == 0) {
        int rc = recover_embedded_sources(binary, &archive);
        paxsy_source_archive_free(&archive);
        return rc == 0 ? 0 : 1;
    }
    if (extracted < 0) {
        fprintf(stderr,
                "recovery: '%s' has malformed or unsupported Paxsy source metadata\n",
                binary);
        return 1;
    }
    return recover_asm_sources(binary) == 0 ? 0 : 1;
}
