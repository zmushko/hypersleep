/*
 * cli.c — hypersleep CLI entry point
 *
 * Dispatches to subcommand handlers based on argv[1]. Each subcommand
 * has its own file (cmd_*.c) and a function with signature:
 *
 *    int cmd_<name>(int argc, char **argv, const hs_config_t *cfg);
 *
 * Subcommands open the index read-only via index_open(... HS_IDX_READ)
 * and the store via store_open(). They communicate with the daemon
 * only through the LMDB index and (for pre-snapshot) via the Unix
 * control socket.
 */

#include "hypersleep.h"
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

/* Subcommand handlers (defined in cmd_*.c) */
int cmd_status (int argc, char **argv, const hs_config_t *cfg);
int cmd_log    (int argc, char **argv, const hs_config_t *cfg);
int cmd_show   (int argc, char **argv, const hs_config_t *cfg);
int cmd_diff   (int argc, char **argv, const hs_config_t *cfg);
int cmd_wake(int argc, char **argv, const hs_config_t *cfg);
int cmd_find   (int argc, char **argv, const hs_config_t *cfg);
int cmd_purge  (int argc, char **argv, const hs_config_t *cfg);
int cmd_verify (int argc, char **argv, const hs_config_t *cfg);
int cmd_config_cmd(int argc, char **argv, const hs_config_t *cfg);

struct subcmd {
    const char *name;
    int (*fn)(int, char **, const hs_config_t *);
    const char *summary;
};

static const struct subcmd commands[] = {
    { "status",  cmd_status,  "show daemon and store status" },
    { "log",     cmd_log,     "list versions of a path" },
    { "show",    cmd_show,    "print a version's content to stdout" },
    { "diff",    cmd_diff,    "compare two versions" },
    { "wake",    cmd_wake,    "wake a stored version to a file" },
    { "find",    cmd_find,    "search the history" },
    { "purge",   cmd_purge,   "purge old versions by age" },
    { "verify",  cmd_verify,  "check store integrity" },
    { "config",  cmd_config_cmd, "inspect or test config" },
    { NULL, NULL, NULL }
};

static void usage(void) {
    fprintf(stderr,
        "Usage: hypersleep <command> [args...]\n"
        "\n"
        "Commands:\n");
    for (const struct subcmd *c = commands; c->name; c++)
        fprintf(stderr, "  %-10s %s\n", c->name, c->summary);
    fprintf(stderr,
        "\n"
        "Run 'hypersleep <command> --help' for command-specific options.\n");
}

int main(int argc, char **argv) {
    const char *config_path = "/etc/hypersleep/hypersleep.conf";

    /* Pre-scan for --config since getopt doesn't see subcommand args */
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--config") == 0
         || strcmp(argv[i], "-c") == 0) {
            config_path = argv[i + 1];
            break;
        }
    }

    if (argc < 2 || strcmp(argv[1], "--help") == 0
                 || strcmp(argv[1], "-h") == 0) {
        usage();
        return argc < 2 ? HS_EXIT_USAGE : 0;
    }

    if (strcmp(argv[1], "--version") == 0
     || strcmp(argv[1], "-V") == 0) {
        printf("hypersleep %d.%d.%d\n",
               HYPERSLEEP_VERSION_MAJOR,
               HYPERSLEEP_VERSION_MINOR,
               HYPERSLEEP_VERSION_PATCH);
        return 0;
    }

    hs_config_t *cfg = config_load(config_path);
    if (!cfg) {
        fprintf(stderr, "hypersleep: failed to load config %s\n", config_path);
        return HS_EXIT_USAGE;
    }

    log_init(cfg, /*foreground*/ true);

    int rc = HS_EXIT_USAGE;
    for (const struct subcmd *c = commands; c->name; c++) {
        if (strcmp(argv[1], c->name) == 0) {
            rc = c->fn(argc - 1, argv + 1, cfg);
            goto done;
        }
    }

    fprintf(stderr, "hypersleep: unknown command '%s'\n\n", argv[1]);
    usage();

done:
    config_free(cfg);
    log_close();
    return rc;
}
