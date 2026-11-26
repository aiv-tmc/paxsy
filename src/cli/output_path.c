#include "common.h"
#include "output_path.h"
#include "toml.h"

int paxsy_output_path(const char *root, const char *cfg,
                      const char *project, int pe,
                      char *out, size_t cap) {
    char configured[PAXSY_PATH_MAX];
    if (toml_get(cfg, "build", "output", configured,
                 sizeof(configured)) && configured[0]) {
        int n = configured[0] == '/'
            ? snprintf(out, cap, "%s", configured)
            : snprintf(out, cap, "%s/%s", root, configured);
        return n > 0 && (size_t)n < cap ? 0 : -1;
    }
    int n = pe
        ? snprintf(out, cap, "%s/build/%s.exe", root, project)
        : snprintf(out, cap, "%s/build/%s", root, project);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int paxsy_pe_aux_path(const char *root, const char *cfg,
                      const char *project, char *out, size_t cap) {
    char configured[PAXSY_PATH_MAX];
    if (toml_get(cfg, "build", "pe_output", configured,
                 sizeof(configured)) && configured[0]) {
        int n = configured[0] == '/'
            ? snprintf(out, cap, "%s", configured)
            : snprintf(out, cap, "%s/%s", root, configured);
        return n > 0 && (size_t)n < cap ? 0 : -1;
    }
    int n = snprintf(out, cap, "%s/build/%s.exe", root, project);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}
