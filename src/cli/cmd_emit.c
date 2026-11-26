#include "common.h"
#include "diag.h"
#include "fsutil.h"
#include "output_path.h"
#include "nemit.h"
#include "nar.h"
#include "hir.h"

#include <stdlib.h>
#include <stdint.h>

int paxsy_emit_archive(const char *root, const char *name,
                        const char *object_path) {
    char archive_path[PAXSY_PATH_MAX];
    char member_name[160];
    FILE *object = fopen(object_path, "rb");
    long object_size = -1;
    uint8_t *data = NULL;
    int rc = 1;
    if (!object || fseek(object, 0, SEEK_END) != 0 ||
        (object_size = ftell(object)) <= 0 ||
        fseek(object, 0, SEEK_SET) != 0) {
        fprintf(stderr, "cannot read object file '%s'\n", object_path);
        goto done;
    }
    data = malloc((size_t)object_size);
    if (!data || fread(data, 1, (size_t)object_size, object) !=
                 (size_t)object_size) {
        fprintf(stderr, "cannot read object file '%s'\n", object_path);
        goto done;
    }
    NAMember member;
    snprintf(archive_path, sizeof(archive_path),
             "%s/build/artifacts/%s.a", root, name);
    snprintf(member_name, sizeof(member_name), "%s.o", name);
    member.name = member_name;
    member.data = data;
    member.size = (size_t)object_size;
    if (nar_write(archive_path, &member, 1) != 0) {
        fprintf(stderr, "archive emission failed\n");
        goto done;
    }
    rc = 0;
done:
    free(data);
    if (object && fclose(object) != 0) rc = 1;
    return rc;
}

int paxsy_emit_pe_image(const char *root, const char *cfg,
                         const char *name, const char *primary_path,
                         int primary_pe, HModule **mods, int nmods,
                         Diag *diag) {
    char path[PAXSY_PATH_MAX];
    if (primary_pe) {
        int n = snprintf(path, sizeof(path), "%s", primary_path);
        if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    } else if (paxsy_pe_aux_path(root, cfg, name,
                                 path, sizeof(path)) != 0) {
        return -1;
    }
    char *slash = strrchr(path, '/');
    if (slash) {
        char parent[PAXSY_PATH_MAX];
        size_t len = (size_t)(slash - path);
        if (len >= sizeof(parent)) return -1;
        memcpy(parent, path, len);
        parent[len] = '\0';
        if (fs_mkdir_p(parent) != 0) {
            fprintf(stderr, "cannot create configured PE output directory\n");
            return -1;
        }
    }
    if (nemit_pe(mods, nmods, diag, path) != 0) {
        fprintf(stderr, "PE emission failed\n");
        return -1;
    }
    return 0;
}

