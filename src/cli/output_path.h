#ifndef PAXSY_OUTPUT_PATH_H
#define PAXSY_OUTPUT_PATH_H

#include "common.h"

int paxsy_output_path(const char *root, const char *cfg,
                      const char *project, int pe,
                      char *out, size_t cap);
int paxsy_pe_aux_path(const char *root, const char *cfg,
                      const char *project, char *out, size_t cap);

#endif
