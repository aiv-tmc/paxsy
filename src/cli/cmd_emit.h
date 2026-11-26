#ifndef PAXSY_CMD_EMIT_H
#define PAXSY_CMD_EMIT_H
#include "common.h"
#include "diag.h"
#include "hir.h"
int paxsy_emit_archive(const char *root, const char *name, const char *object_path);
int paxsy_emit_pe_image(const char *root, const char *cfg, const char *name,
                        const char *primary_path, int primary_pe,
                        HModule **mods, int nmods, Diag *diag);
#endif
