#include "common.h"
#include "cli.h"
#include "banner.h"
#include "commands.h"

/*
 * main.c - entry point of the paxsy driver.
 * Order of interpretation: --version and --license answer immediately;
 * --help (or no command at all) prints the banner plus the usage; any
 * other input is handed to the subcommand dispatcher.
 */

int main(int argc, char **argv) {
    Cli cli;
    if (cli_parse(&cli, argc, argv) != 0) return 2;

    if (cli.version) {
        printf("paxsy %s (%s, %s)\n", PAXSY_VERSION, PAXSY_STATE, PAXSY_DATE);
        return 0;
    }
    if (cli.license) {
        license_print(stdout);
        return 0;
    }
    if (!cli.command) {
        banner_print(stdout);
        usage_print(stdout);
        return cli.help ? 0 : 2;
    }
    if (cli.help) {
        banner_print(stdout);
        usage_print(stdout);
        return 0;
    }
    return commands_run(&cli);
}
