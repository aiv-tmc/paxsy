#ifndef DEFMACROS_H
#define DEFMACROS_H

#include "../directive/define/macro.h"

/* Target OS and architecture strings.
   These should be set by the caller before calling builtin_macros_init.
   Examples:
     builtin_target_os = "linux", "windows", "darwin", "freebsd", "solaris", "msdos"
     builtin_target_arch = "i386", "i486", "i586", "i686", "x86_64", "amd64", "armv7", "aarch64", etc.
   If not set, no OS/architecture macros will be defined (except __CPU__ = "unknown").
*/
extern const char* builtin_target_os;
extern const char* builtin_target_arch;

void builtin_macros_init(MacroTable* table, const char* filename);

#endif
