#include "defmacros.h"
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* Global variables for target platform (to be set by caller) */
const char* builtin_target_os = NULL;
const char* builtin_target_arch = NULL;

/* Helper to add an object-like macro with a string value */
static void add_macro(MacroTable* table, const char* name, const char* value) {
    macro_table_add(table, name, value, 0, NULL, 0);
}

/* Helper to add an integer macro (value converted to string) */
static void add_int_macro(MacroTable* table, const char* name, int value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    add_macro(table, name, buf);
}

/* Extract x86 generation number from strings like "i386", "i486", "i586", "i686", or "8086". Returns 0 if not x86. */
static int extract_x86_number(const char* arch) {
    if (arch[0] == 'i' && isdigit(arch[1])) {
        return atoi(arch + 1);
    }
    if (strcmp(arch, "8086") == 0) return 86;
    return 0;
}

/* Extract ARM architecture version from strings like "armv4", "armv5", "armv6", "armv7", "armv8", "aarch64". Returns 0 if unknown. */
static int extract_arm_version(const char* arch) {
    if (strncmp(arch, "armv", 4) == 0 && isdigit(arch[4])) {
        return atoi(arch + 4);
    }
    if (strcmp(arch, "aarch64") == 0) return 8;
    return 0;
}

void builtin_macros_init(MacroTable* table, const char* filename) {
    add_macro(table, "zero",    "(@Void)(0)");
    add_macro(table, "elif",    "else => if");
    add_macro(table, "Str",     "@Char");
    add_macro(table, "Bool",    "Int<1>");
    add_macro(table, "true",    "(Int<8>)(1)");
    add_macro(table, "false",   "(Int<8>)(0)");
    add_macro(table, "sti",     "interflag (Int<8>)(1)");
    add_macro(table, "cli",     "Interflag (Int<8>)(0)");

    /* Syntax errors */
    add_macro(table, "EC_SyntaxGeneric",            "0x7A00");
    add_macro(table, "EC_SyntaxUnexpectedToken",    "0x7A01");
    add_macro(table, "EC_SyntaxUnexpectedEOF",      "0x7A02");
    add_macro(table, "EC_SyntaxInvalidChar",        "0x7A03");
    add_macro(table, "EC_SyntaxMissingSemicolon",   "0x7A04");
    add_macro(table, "EC_SyntaxInvalidStatement",   "0x7A05");
    add_macro(table, "EC_SyntaxUnclosedQuote",      "0x7A06");
    add_macro(table, "EC_SyntaxMissingParen",       "0x7A07");

    /* Lexical errors */
    add_macro(table, "EC_LexerInvalidNumber",   "0xE000");
    add_macro(table, "EC_LexerInvalidEscape",   "0xE001");
    add_macro(table, "EC_LexerUnclosedString",  "0xE002");
    add_macro(table, "EC_LexerUnknownChar",     "0xE003");

    /* Semantic errors */
    add_macro(table, "EC_SemMismatch",          "0xA400");
    add_macro(table, "EC_SemInvalidCast",       "0xA401");
    add_macro(table, "EC_SemUndefinedVar",      "0xA402");
    add_macro(table, "EC_SemInvalidOperation",  "0xA403");
    add_macro(table, "EC_SemRedeclaration",     "0xA404");
    add_macro(table, "EC_SemUndeclaredSymbol",  "0xA405");
    add_macro(table, "EC_SemUninitialized",     "0xA406");
    add_macro(table, "EC_SemAssignToConst",     "0xA407");
    add_macro(table, "EC_SemTypeError",         "0xA408");
    add_macro(table, "EC_SemUnusedVariable",    "0xA409");
    add_macro(table, "EC_SemMissingReturn",     "0xA40A");

    /* Preprocessor errors */
    add_macro(table, "EC_PpUnknownDir",         "0x4C00");
    add_macro(table, "EC_PpDirTooLong",         "0x4C01");
    add_macro(table, "EC_PpMacroDefFailed",     "0x4C02");
    add_macro(table, "EC_PpInvalidDir",         "0x4C03");
    add_macro(table, "EC_PpUndefined",          "0x4C04");
    add_macro(table, "EC_PpMacroRecursion",     "0x4C05");
    add_macro(table, "EC_PpDuplicateDir",       "0x4C06");

    /* Compile errors */
    add_macro(table, "EC_ComFailCreate",        "0xFF00");

    /* Memory errors */
    add_macro(table, "EC_MemoryAllocation",     "0x6B00");
    add_macro(table, "EC_MemoryOverflow",       "0x6B01");
    add_macro(table, "EC_MemoryInvalidFree",    "0x6B02");

    /* Runtime errors */
    add_macro(table, "EC_RuntimeDivByZero",     "0x2300");
    add_macro(table, "EC_RuntimeOutOfBounds",   "0x2301");
    add_macro(table, "EC_RuntimeOverflow",      "0x2302");

    /* I/O errors */
    add_macro(table, "EC_IoFileNotFound",       "0x8200");
    add_macro(table, "EC_IoDoubleFile",         "0x8201");
    add_macro(table, "EC_IoPermissionDenied",   "0x8202");
    add_macro(table, "EC_IoRead",               "0x8203");
    add_macro(table, "EC_IoWrite",              "0x8204");

    /* Input flags errors */
    add_macro(table, "EC_InputMultiModFlags",   "0x8900");
    add_macro(table, "EC_InputInvalidFlag",     "0x8901");
    add_macro(table, "EC_InputNoSource",        "0x8902");

    if (builtin_target_os) {
        if (strcmp(builtin_target_os, "windows") == 0 ||
            strcmp(builtin_target_os, "win32") == 0 ||
            strcmp(builtin_target_os, "win64") == 0) {
            add_int_macro(table, "__WIN__", 1);
        }
        else if (strcmp(builtin_target_os, "linux") == 0) {
            add_int_macro(table, "__LINUX__", 1);
            add_int_macro(table, "__UNIX__", 1);
        }
        else if (strcmp(builtin_target_os, "darwin") == 0 ||
                 strcmp(builtin_target_os, "macos") == 0 ||
                 strcmp(builtin_target_os, "apple") == 0) {
            add_int_macro(table, "__MACOS__", 1);
            add_int_macro(table, "__UNIX__", 1);
            add_int_macro(table, "__BSD__", 1);
        }
        else if (strcmp(builtin_target_os, "freebsd") == 0) {
            add_int_macro(table, "__FREEBSD__", 1);
            add_int_macro(table, "__BSD__", 1);
            add_int_macro(table, "__UNIX__", 1);
        }
        else if (strcmp(builtin_target_os, "openbsd") == 0) {
            add_int_macro(table, "__OPENBSD__", 1);
            add_int_macro(table, "__BSD__", 1);
            add_int_macro(table, "__UNIX__", 1);
        }
        else if (strcmp(builtin_target_os, "netbsd") == 0) {
            add_int_macro(table, "__NETBSD__", 1);
            add_int_macro(table, "__BSD__", 1);
            add_int_macro(table, "__UNIX__", 1);
        }
        else if (strcmp(builtin_target_os, "solaris") == 0) {
            add_int_macro(table, "__SOLARIS__", 1);
            add_int_macro(table, "__UNIX__", 1);
        }
        else if (strcmp(builtin_target_os, "msdos") == 0 ||
                 strcmp(builtin_target_os, "dos") == 0) {
            add_int_macro(table, "__DOS__", 1);
        }
        /* Add more OS types as needed */
    }

    if (builtin_target_arch) {
        const char* arch = builtin_target_arch;
        int x86_num = extract_x86_number(arch);
        if (x86_num) {
            /* x86 family (8086, i386, i486, i586, i686, ...) */
            add_int_macro(table, "__x86__", 1);      /* family indicator */
            char num_buf[16];
            snprintf(num_buf, sizeof(num_buf), "%d", x86_num);
            add_macro(table, "__x86__", num_buf);  /* numeric version */

            /* Define specific macro like __i386__, __i486__, etc. */
            if (strcmp(arch, "8086") == 0) {
                add_int_macro(table, "__8086__", 1);
            } else {
                char specific[32];
                snprintf(specific, sizeof(specific), "__%s__", arch);
                add_int_macro(table, specific, 1);
            }
        }
        else if (strcmp(arch, "x86_64") == 0) {
            add_int_macro(table, "__x86_64__", 1);
        }
        else if (strcmp(arch, "amd64") == 0) {
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
            add_int_macro(table, "__arm__", 1);
            if (strcmp(arch, "aarch64") == 0) {
                add_int_macro(table, "__aarch64__", 1);
            }
        }
    }

    if (filename) {
        size_t len = strlen(filename);
        char* quoted = malloc(len + 3);
        if (quoted) {
            quoted[0] = '"';
            memcpy(quoted + 1, filename, len);
            quoted[len + 1] = '"';
            quoted[len + 2] = '\0';
            add_macro(table, "__FILE__", quoted);
            free(quoted);
        } else {
            add_macro(table, "__FILE__", "\"\"");
        }
    } else {
        add_macro(table, "__FILE__", "\"\"");
    }

    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);

    char time_buf[16];
    strftime(time_buf, sizeof(time_buf), "\"%H:%M:%S\"", tm_info);
    add_macro(table, "__TIME__", time_buf);

    char date_buf[16];
    strftime(date_buf, sizeof(date_buf), "\"%b %d %Y\"", tm_info);
    add_macro(table, "__DATE__", date_buf);

    int bits = (int)(sizeof(void*) * 8);
    char bits_buf[8];
    snprintf(bits_buf, sizeof(bits_buf), "%d", bits);
    add_macro(table, "__BITS__", bits_buf);
}
