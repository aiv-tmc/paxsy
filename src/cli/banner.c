#include "common.h"
#include "banner.h"
#include "color.h"

/*
 * banner.c - all user-facing static text of the tool.
 * Colour escapes are enabled only for interactive terminal output.
 */

/* Renders the two-column block logo. Left column: logo; right column:
 * version line, documentation link and project site. */
void banner_print(FILE *out) {
    color_setup();
    fprintf(out,
        "%s████████████████%s\n"
        "%s      ██████████%s%s░░%s\t"
            "paxsy %s%s%s (%s, %s)\n"
        "%s  ████  ████████%s%s░░%s\n"
        "%s  ████  ████████%s%s░░%s\tRead the documentation here:\n"
        "%s      ██████████%s%s░░%s\t\t%shttps://github.com/aiv-tmc/paxsy/wiki%s\n"
        "%s  ██████████████%s%s░░%s\n"
        "%s  ██████████████%s%s░░%s\tOur official website:\n"
        "%s████████████████%s%s░░%s\t\t%shttps://paxsy.org%s\n"
        "%s  ░░░░░░░░░░░░░░░░%s\n\n",
        c_green, c_reset,
        c_green, c_reset, c_dim, c_reset,
        c_cyan, PAXSY_VERSION, c_reset, PAXSY_STATE, PAXSY_DATE,
        c_green, c_reset, c_dim, c_reset,
        c_green, c_reset, c_dim, c_reset,
        c_green, c_reset, c_dim, c_reset, c_dim, c_reset,
        c_green, c_reset, c_dim, c_reset,
        c_green, c_reset, c_dim, c_reset,
        c_green, c_reset, c_dim, c_reset, c_dim, c_reset,
        c_dim, c_reset);
}

/* The essential commands, shown with the banner when paxsy runs
 * bare; the full reference answers --help. */
static void usage_heading(FILE *out, const char *label) {
    fprintf(out, "%s%s%s\n", c_cyan, label, c_reset);
}

void usage_short(FILE *out) {
    color_setup();
    fprintf(out,
        "%sUsage:%s paxsy <command> [options]\n\n",
        c_cyan, c_reset);
    usage_heading(out, "Commands");
    fprintf(out,
        "    new <project>            Create a new project\n"
        "    build [path]             Build the target configured in <project>.toml\n"
        "    check [path]             Verify syntax and semantics\n"
        "    run [path] [-- <args>]   Execute the configured runtime profile\n"
        "    test [path]              Discover and validate tests in tests/\n"
        "    bench [path]             Benchmark source files in benches/\n"
        "    fmt [path]               Format source files silently on success\n"
        "    clippy [path]            Check code quality\n"
        "    debug [path]             Run the integrated debugger\n"
        "    clean [path]             Remove generated artifacts\n\n"
        "Run 'paxsy --help' for the complete command reference.\n");
}

void usage_print(FILE *out) {
    color_setup();
    fprintf(out, "%sUsage:%s paxsy <command> [path] [arguments]\n\n",
            c_cyan, c_reset);

    usage_heading(out, "Everyday commands");
    fprintf(out,
        "    new <name>               Create a project\n"
        "    build [path]             Build the project\n"
        "    check [path]             Check source without building\n"
        "    run [path] [-- args]     Run the configured execution profile\n"
        "    test [path]              Run project tests\n"
        "    bench [path]             Run benchmarks\n"
        "    fmt [path]               Format source files\n"
        "    clippy [path]            Check code quality\n"
        "    debug [path]             Debug the project executable\n"
        "    disasm [path|binary]     Inspect machine code\n"
        "    recovery [path|binary]   Restore source or machine-level representation\n"
        "    clean [path]             Remove generated files\n"
        "    tree [path]              Show project files\n\n");

    usage_heading(out, "Packages and project tools");
    fprintf(out,
        "    fetch                    Refresh package index\n"
        "    search <term>            Search packages\n"
        "    install <pkg> [version]  Install a package\n"
        "    uninstall <pkg>         Remove an installed package\n"
        "    update <pkg> [version]  Update a package\n"
        "    add <pkg> [version]     Add a project dependency\n"
        "    remove <pkg>            Remove a project dependency\n"
        "    list                     List installed packages\n"
        "    publish [path]           Prepare a distributable project\n"
        "    meta [path]              Show project metadata\n"
        "    config <key> [value]     Read or change a TOML setting\n"
        "    state [path]             Create a frontend state archive\n"
        "    shared [path]            Create a shared frontend archive\n"
        "    link <out> -- <obj...>   Link object files\n\n");

    usage_heading(out, "Small set of options");
    fprintf(out,
        "    --check                  Check formatting without writing\n"
        "    --fix                    Apply safe clippy fixes\n"
        "    --no-git                 Create a project without Git\n"
        "    --verbose                Show additional progress\n"
        "    --profile                Show build timings; with run, select a profile\n"
        "    run --profile NAME       Select a [run.NAME] execution profile\n"
        "    -q, --quiet              Hide optional progress\n"
        "    -h, --help               Show this help\n"
        "    -v, --version            Show version\n"
        "    -l, --license            Show license\n\n"
        "Build, lint, formatting and debugger policies belong in the project TOML.\n"
        "See docs/ToolchainUserManual.md for command and configuration details.\n");
}

/* License identity and canonical text location shown by --license. */
void license_print(FILE *out) {
    fprintf(out,
        "paxsy %s - Apache License, Version 2.0\n"
        "Copyright 2026 Paxsy contributors\n\n"
        "Licensed under the Apache License, Version 2.0 (the \"License\");\n"
        "you may not use this file except in compliance with the License.\n"
        "You may obtain a copy of the License at\n\n"
        "    https://www.apache.org/licenses/LICENSE-2.0\n\n"
        "Unless required by applicable law or agreed to in writing,\n"
        "software distributed under the License is distributed on an \"AS IS\" BASIS,\n"
        "WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\n"
        "See LICENSE and NOTICE in the source distribution.\n", PAXSY_VERSION);
}
