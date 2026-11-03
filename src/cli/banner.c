#include "common.h"
#include "banner.h"

/*
 * banner.c - all user-facing static text of the tool.
 * ANSI colour escapes are emitted unconditionally; terminals without
 * colour support display the block-drawing glyphs unchanged.
 */

/* Renders the two-column block logo. Left column: logo; right column:
 * version line, documentation link and project site. */
void banner_print(FILE *out) {
    fprintf(out,
        "\033[92m████████████████\033[0m\n"
        "\033[92m      ██████████\033[90m░░\033[0m\t"
            "paxsy \033[91m%s \033[90m%s - %s\033[0m\n"
        "\033[92m  ████  ████████\033[90m░░\033[0m\n"
        "\033[92m  ████  ████████\033[90m░░\033[0m\t"
            "Read the documentation here:\n"
        "\033[92m      ██████████\033[90m░░\033[0m\t\t"
            "\033[90mhttps://github.com/aiv-tmc/paxsy/wiki\033[0m\n"
        "\033[92m  ██████████████\033[90m░░\033[0m\n"
        "\033[92m  ██████████████\033[90m░░\033[0m\t"
            "Our official website:\n"
        "\033[92m████████████████\033[90m░░\033[0m\t\t"
            "\033[90mhttps://paxsy.org\033[0m\n"
        "\033[90m  ░░░░░░░░░░░░░░░░\033[0m\n"
        "\n",
        PAXSY_STATE, PAXSY_VERSION, PAXSY_DATE);
}

/* Complete command reference grouped by area of responsibility. */
void usage_print(FILE *out) {
    fprintf(out,
        "Usage: paxsy <command> [options]\n"
        "\n"
        "Project commands:\n"
        "    init <project>           Create src/main.px and <project>.toml\n"
        "    new <project>            Alias of init\n"
        "    build [path]             Run the full frontend pipeline, write artifacts\n"
        "    check [path]             Verify syntax and semantics, write nothing\n"
        "    run [path]               Execute target/<name> if it exists\n"
        "    test [path]              Compile *_test.px files, list discovered tests\n"
        "    bench [path]             Measure lexer/parser throughput per file\n"
        "    clean [path]             Remove build/, target/ and debug/\n"
        "    tree [path]              Print the project directory layout\n"
        "\n"
        "Code quality:\n"
        "    clippy [path]            Check the source against the clean-code rules\n"
        "      --fix                  Rewrite mechanically fixable violations\n"
        "      --strict               Treat every warning as an error\n"
        "\n"
        "Packages:\n"
        "    fetch                    Rebuild the local package index\n"
        "    search <term>            Search the local package index\n"
        "    install <pkg> [version]  Install a package from the cache\n"
        "    uninstall <pkg>         Remove an installed package\n"
        "    update <pkg>             Refresh an installed package from the cache\n"
        "    add <pkg> [version]      Record a dependency in <project>.toml\n"
        "    remove <pkg>             Drop a dependency from <project>.toml\n"
        "    list                     List installed packages\n"
        "    publish [path]           Assemble a distributable tree into dist/\n"
        "\n"
        "Project information:\n"
        "    meta [path]              Print metadata from <project>.toml\n"
        "    config <section.key> [value]\n"
        "                             Read or write a configuration value\n"
        "    doc [path]               Regenerate doc.md from /// doc comments\n"
        "\n"
        "Artifact formats (frontend archives, see docs/cli.md):\n"
        "    state [path]             Emit target/<name>.a frontend archive\n"
        "    shared [path]            Emit target/<name>.sfa frontend archive\n"
        "\n"
        "Frontend options (build, check, test, clippy):\n"
        "    --ast                    Print the abstract syntax tree\n"
        "    --tokens                 Print the token stream\n"
        "    --max-errors=N           Cap stored diagnostics (default 20)\n"
        "    --recursive              Descend into src/ subdirectories\n"
        "\n"
        "Quality gates:\n"
        "    --Wextra                 Enable extended warnings\n"
        "    --Werror                 Treat warnings as errors\n"
        "    --ignore-warnings        Suppress all warnings\n"
        "\n"
        "General:\n"
        "    -v, --version            Show version\n"
        "    -l, --license            Show license information\n"
        "    --verbose                Detailed progress output\n"
        "    -q, --quiet              Suppress non-diagnostic output\n"
        "    -h, --help               Display this help\n");
}

/* Full MIT license text printed by --license. */
void license_print(FILE *out) {
    fprintf(out,
        "paxsy %s - MIT License\n"
        "\n"
        "Permission is hereby granted, free of charge, to any person\n"
        "obtaining a copy of this software and associated documentation\n"
        "files (the \"Software\"), to deal in the Software without\n"
        "restriction, including without limitation the rights to use,\n"
        "copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
        "copies of the Software, and to permit persons to whom the\n"
        "Software is furnished to do so, subject to the following\n"
        "conditions: the above copyright notice and this permission\n"
        "notice shall be included in all copies or substantial portions\n"
        "of the Software. THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT\n"
        "WARRANTY OF ANY KIND.\n", PAXSY_VERSION);
}
