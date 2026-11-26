#include "common.h"
#include "cli.h"
#include "banner.h"
#include "commands.h"
#include "color.h"

/*
 * main.c - entry point of the paxsy driver.
 * Version, licence, and help requests are handled before dispatch;
 * every other invocation is handed to the subcommand dispatcher.
 */

int main(int argc, char **argv) {
    Cli cli;
    if (cli_parse(&cli, argc, argv) != 0) return 2;

    if (cli.version) {
        color_setup();
        printf("%spaxsy%s %s (%s, %s)\n", c_cyan, c_reset,
               PAXSY_VERSION, PAXSY_STATE, PAXSY_DATE);
        return 0;
    }
    if (cli.license) {
        license_print(stdout);
        return 0;
    }
    if (cli.help) {
        usage_print(stdout);
        return 0;
    }
    if (!cli.command) {
        banner_print(stdout);
        usage_short(stdout);
        return 0;
    }
    return commands_run(&cli);
}
