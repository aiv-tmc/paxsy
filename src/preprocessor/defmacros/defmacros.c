// TODO: Figure out how to implement the OS definition

#include "defmacros.h"
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* Global variables for target platform (set by the caller) */
const char* builtin_target_os = NULL;
const char* builtin_target_arch = NULL;
const char* builtin_target_bits = NULL;

/* Helper: add an object-like macro (no parameters) with a string value. */
static void add_macro(MacroTable* table, const char* name, const char* value) {
    macro_table_add(table, name, value, 0, NULL, 0);
}

/* Helper: add an object-like macro whose value is an integer (converted to string). */
static void add_int_macro(MacroTable* table, const char* name, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    add_macro(table, name, buf);
}

/* Helper: add a function-like macro with a given parameter list.
 * 'param_names' is an array of strings, 'param_count' its length.
 * The 'value' is the replacement text, which may refer to the parameters. */
static void add_macro_with_args(MacroTable* table, const char* name,
                                const char* value, int param_count,
                                const char** param_names) {
    macro_table_add(table, name, value, param_count, param_names, 0);
}

/* Extract x86 generation number from strings like "i386", "i486", "i586", "i686", or "8086".
 * Returns 0 if the architecture string does not denote an x86 variant. */
static int extract_x86_number(const char* arch) {
    if (arch[0] == 'i' && isdigit((unsigned char)arch[1])) {
        return atoi(arch + 1);
    }
    if (strcmp(arch, "8086") == 0) return 86;
    return 0;
}

/* Extract ARM architecture version from strings like "armv4", "armv5", "armv6", "armv7", "armv8", "aarch64".
 * Returns the version number, or 0 if unknown. */
static int extract_arm_version(const char* arch) {
    if (strncmp(arch, "armv", 4) == 0 && isdigit((unsigned char)arch[4])) {
        return atoi(arch + 4);
    }
    if (strcmp(arch, "aarch64") == 0) return 8;
    return 0;
}

/* Main entry point: initialise the macro table with all built-in definitions.
   All detection is performed using the externally supplied strings
   builtin_target_os, builtin_target_arch, builtin_target_bits.
   No C‑specific macros (e.g. sizeof(void*)) are used. */
void builtin_macros_init(MacroTable* table, const char* filename) {
    /* Core language macros (object-like) */
    add_macro(table, "zero",    "(0:@Void)");
    add_macro(table, "elif",    "else->if");
    add_macro(table, "uInt",    "unsigned Int");
    add_macro(table, "uReal",   "unsigned Real");
    add_macro(table, "uChar",   "unsigned Char");
    add_macro(table, "uVoid",   "unsigned Void");
    add_macro(table, "uAuto",   "unsigned Auto");
    add_macro(table, "Str",     "const @Char");
    add_macro(table, "Bool",    "unsigned Int<1>");
    add_macro(table, "true",    "(unsigned Int<1>)(1)");
    add_macro(table, "false",   "(unsigned Int<1>)(0)");

    /* OS identification macros (based on builtin_target_os) */
    if (builtin_target_os) {
        if (strcmp(builtin_target_os, "windows") == 0 ||
            strcmp(builtin_target_os, "win32") == 0 ||
            strcmp(builtin_target_os, "win64") == 0) {
            add_int_macro(table, "__windows__", 1);
        }
        else if (strcmp(builtin_target_os, "linux") == 0) {
            add_int_macro(table, "__Linux__", 1);
            add_int_macro(table, "__UNIX__", 1);
        }
        else if (strcmp(builtin_target_os, "darwin") == 0 ||
                 strcmp(builtin_target_os, "macos") == 0 ||
                 strcmp(builtin_target_os, "apple") == 0) {
            add_int_macro(table, "__macOS__", 1);
            add_int_macro(table, "__UNIX__", 1);
            add_int_macro(table, "__BSD__", 1);
        }
        else if (strcmp(builtin_target_os, "freebsd") == 0) {
            add_int_macro(table, "__freeBSD__", 1);
            add_int_macro(table, "__BSD__", 1);
            add_int_macro(table, "__UNIX__", 1);
        }
        else if (strcmp(builtin_target_os, "openbsd") == 0) {
            add_int_macro(table, "__openBSD__", 1);
            add_int_macro(table, "__BSD__", 1);
            add_int_macro(table, "__UNIX__", 1);
        }
        else if (strcmp(builtin_target_os, "netbsd") == 0) {
            add_int_macro(table, "__netBSD__", 1);
            add_int_macro(table, "__BSD__", 1);
            add_int_macro(table, "__UNIX__", 1);
        }
        else if (strcmp(builtin_target_os, "solaris") == 0) {
            add_int_macro(table, "__solaris__", 1);
            add_int_macro(table, "__UNIX__", 1);
        }
        else if (strcmp(builtin_target_os, "msdos") == 0 ||
                 strcmp(builtin_target_os, "dos") == 0) {
            add_int_macro(table, "__DOS__", 1);
        }
    }

    /* Architecture detection (based on builtin_target_arch) */
    if (builtin_target_arch) {
        const char* arch = builtin_target_arch;
        int x86_num = extract_x86_number(arch);
        if (x86_num) {
            /* x86 family (8086, i386, i486, i586, i686, ...) */
            add_int_macro(table, "__x86__", 1);
            char num_buf[16];
            snprintf(num_buf, sizeof(num_buf), "%d", x86_num);
            add_macro(table, "__x86__", num_buf);
            if (strcmp(arch, "8086") == 0) {
                add_int_macro(table, "__8086__", 1);
            } else {
                char specific[32];
                snprintf(specific, sizeof(specific), "__%s__", arch);
                add_int_macro(table, specific, 1);
            }
        }
        else if (strcmp(arch, "x86_64") == 0 || strcmp(arch, "amd64") == 0) {
            add_int_macro(table, "__x86_64__", 1);
        }
        else if (strncmp(arch, "arm", 3) == 0) {
            /* ARM family */
            add_int_macro(table, "__arm__", 1);
            int arm_ver = extract_arm_version(arch);
            if (arm_ver) {
                char ver_buf[16];
                snprintf(ver_buf, sizeof(ver_buf), "%d", arm_ver);
                add_macro(table, "__arm__", ver_buf);
            } else {
                add_macro(table, "__arm__", "0");
            }
            if (strcmp(arch, "aarch64") == 0) {
                add_int_macro(table, "__aarch64__", 1);
            }
        }
    }

    /* Bitness macro: __bits__ – uses the externally provided builtin_target_bits.
       If not provided, we do NOT fall back to sizeof(void*); we keep it empty.
       The driver must set it appropriately. */
    if (builtin_target_bits && builtin_target_bits[0] != '\0') {
        add_macro(table, "__bits__", builtin_target_bits);
    } else {
        /* No fallback – we leave it undefined to avoid any C‑specific assumption. */
    }

    /* __file__ macro expands to the current source filename (quoted) */
    if (filename) {
        size_t len = strlen(filename);
        char* quoted = malloc(len + 3);
        if (quoted) {
            quoted[0] = '"';
            memcpy(quoted + 1, filename, len);
            quoted[len + 1] = '"';
            quoted[len + 2] = '\0';
            add_macro(table, "__file__", quoted);
            free(quoted);
        } else {
            add_macro(table, "__file__", "\"\"");
        }
    } else {
        add_macro(table, "__file__", "\"\"");
    }

    /* __time__ and __date__ macros (compilation timestamp) */
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char time_buf[16];
    strftime(time_buf, sizeof(time_buf), "\"%H:%M:%S\"", tm_info);
    add_macro(table, "__time__", time_buf);
    char date_buf[16];
    strftime(date_buf, sizeof(date_buf), "\"%b %d %Y\"", tm_info);
    add_macro(table, "__date__", date_buf);
}
