/*
 * cmd_config.c — renatum config {show,test,paths,reload}
 *
 *   show    dump the effective configuration as renatum saw it
 *   test    run config_validate (paths exist, are writable, etc.)
 *   paths   list configured watch directives, one per line
 *   reload  send SIGHUP to renatumd (best-effort via /run/renatum/lock
 *           if it has a stored pid; for now: log and exit until
 *           main.c starts writing the pid into the lock file)
 */

#include "config.h"
#include "log.h"
#include "renatum.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *log_level_name(enum rnt_log_level lvl)
{
    switch (lvl) {
        case RNT_LOG_DEBUG: return "debug";
        case RNT_LOG_INFO:  return "info";
        case RNT_LOG_WARN:  return "warn";
        case RNT_LOG_ERROR: return "error";
    }
    return "?";
}

static const char *fsync_mode_name(int mode)
{
    switch (mode) {
        case FSYNC_FULL: return "full";
        case FSYNC_DATA: return "data";
        case FSYNC_NONE: return "none";
    }
    return "?";
}

static const char *prio_name(enum rnt_priority p)
{
    switch (p) {
        case RNT_PRIO_LOW:    return "low";
        case RNT_PRIO_NORMAL: return "normal";
        case RNT_PRIO_HIGH:   return "high";
    }
    return "?";
}

static void show(const rnt_config_t *cfg)
{
    printf("storage         %s\n",        cfg->store_path);
    printf("index           %s\n",        cfg->index_path);
    printf("log             %s\n",        cfg->log_path ? cfg->log_path : "(syslog)");
    printf("log-level       %s\n",        log_level_name(cfg->log_level));
    printf("control-socket  %s\n",        cfg->control_socket);
    printf("\n");
    printf("inotify-max-watches-warn %u\n", cfg->inotify_max_watches_warn);
    printf("debounce-ms              %u\n", cfg->debounce_ms);
    printf("queue-poll-ms            %u\n", cfg->queue_poll_ms);
    printf("\n");
    printf("compress-default  %s\n",        cfg->compress_default ? "yes" : "no");
    printf("compress-min-size %zu\n",       cfg->compress_min_size);
    printf("fsync-mode        %s\n",        fsync_mode_name(cfg->fsync_mode));
    printf("store-fmode       %04o\n",      cfg->store_fmode);
    printf("\n");
    if (cfg->retention_default_ns == 0) {
        printf("retention-default infinite\n");
    } else {
        printf("retention-default %" PRIu64 "ns\n",
               cfg->retention_default_ns);
    }
    printf("gc-after-prune    %s\n",
           cfg->gc_after_prune ? "yes" : "no");
    printf("\n");
    printf("watching %zu paths:\n", cfg->n_watches);
    for (size_t i = 0; i < cfg->n_watches; i++) {
        const rnt_watch_t *w = &cfg->watches[i];
        printf("  %s\n", w->path);
        printf("    recursive=%s priority=%s compress=%s%s\n",
               w->recursive       ? "yes" : "no",
               prio_name(w->priority),
               w->compress        ? "yes" : "no",
               w->has_exclude     ? " exclude=<regex>" : "");
        if (w->retention_ns == 0) {
            printf("    retention=infinite\n");
        } else {
            printf("    retention=%" PRIu64 "ns\n", w->retention_ns);
        }
    }
}

static void paths(const rnt_config_t *cfg)
{
    for (size_t i = 0; i < cfg->n_watches; i++) {
        printf("%s\n", cfg->watches[i].path);
    }
}

int cmd_config_cmd(int argc, char **argv, const rnt_config_t *cfg)
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: renatum config {show|test|paths|reload}\n");
        return RNT_EXIT_USAGE;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "show") == 0) {
        show(cfg);
        return RNT_EXIT_OK;
    }
    if (strcmp(sub, "paths") == 0) {
        paths(cfg);
        return RNT_EXIT_OK;
    }
    if (strcmp(sub, "test") == 0) {
        if (config_validate(cfg) == 0) {
            printf("config: OK\n");
            return RNT_EXIT_OK;
        }
        fprintf(stderr, "config: validation failed\n");
        return RNT_EXIT_USAGE;
    }
    if (strcmp(sub, "reload") == 0) {
        /* renatumd does not write its pid to /var/lib/renatum/lock
         * yet; once it does, this subcommand can read it and send
         * SIGHUP. For v0.1.0 we surface the limitation rather than
         * pretending to act. */
        fprintf(stderr,
                "renatum config reload: not wired in v0.1.0; "
                "send SIGHUP to renatumd directly\n");
        return RNT_EXIT_ERROR;
    }
    fprintf(stderr, "renatum config: unknown subcommand '%s'\n", sub);
    return RNT_EXIT_USAGE;
}
