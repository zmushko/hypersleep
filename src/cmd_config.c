/*
 * cmd_config.c — hypersleep config {show,test,paths,reload}
 *
 *   show    dump the effective configuration as hypersleep saw it
 *   test    run config_validate (paths exist, are writable, etc.)
 *   paths   list configured watch directives, one per line
 *   reload  send SIGHUP to hypersleepd (best-effort via /run/hypersleep/lock
 *           if it has a stored pid; for now: log and exit until
 *           main.c starts writing the pid into the lock file)
 */

#include "config.h"
#include "log.h"
#include "hypersleep.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static const char *log_level_name(enum hs_log_level lvl)
{
    switch (lvl) {
        case HS_LOG_DEBUG: return "debug";
        case HS_LOG_INFO:  return "info";
        case HS_LOG_WARN:  return "warn";
        case HS_LOG_ERROR: return "error";
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

static const char *prio_name(enum hs_priority p)
{
    switch (p) {
        case HS_PRIO_LOW:    return "low";
        case HS_PRIO_NORMAL: return "normal";
        case HS_PRIO_HIGH:   return "high";
    }
    return "?";
}

static void show(const hs_config_t *cfg)
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
        const hs_watch_t *w = &cfg->watches[i];
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

static void paths(const hs_config_t *cfg)
{
    for (size_t i = 0; i < cfg->n_watches; i++) {
        printf("%s\n", cfg->watches[i].path);
    }
}

int cmd_config_cmd(int argc, char **argv, const hs_config_t *cfg)
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage: hypersleep config {show|test|paths|reload}\n");
        return HS_EXIT_USAGE;
    }
    const char *sub = argv[1];
    if (strcmp(sub, "show") == 0) {
        show(cfg);
        return HS_EXIT_OK;
    }
    if (strcmp(sub, "paths") == 0) {
        paths(cfg);
        return HS_EXIT_OK;
    }
    if (strcmp(sub, "test") == 0) {
        if (config_validate(cfg) == 0) {
            printf("config: OK\n");
            return HS_EXIT_OK;
        }
        fprintf(stderr, "config: validation failed\n");
        return HS_EXIT_USAGE;
    }
    if (strcmp(sub, "reload") == 0) {
        /* hypersleepd does not write its pid to /var/lib/hypersleep/lock
         * yet; once it does, this subcommand can read it and send
         * SIGHUP. For v0.1.0 we surface the limitation rather than
         * pretending to act. */
        fprintf(stderr,
                "hypersleep config reload: not wired in v0.1.0; "
                "send SIGHUP to hypersleepd directly\n");
        return HS_EXIT_ERROR;
    }
    fprintf(stderr, "hypersleep config: unknown subcommand '%s'\n", sub);
    return HS_EXIT_USAGE;
}
