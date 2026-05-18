/*
 * config.h — parsed configuration
 *
 * The config file is line-oriented; see docs/config.md for the format.
 */

#ifndef RENATUM_CONFIG_H
#define RENATUM_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <regex.h>

enum rnt_log_level {
    RNT_LOG_DEBUG,
    RNT_LOG_INFO,
    RNT_LOG_WARN,
    RNT_LOG_ERROR,
};

enum rnt_priority {
    RNT_PRIO_LOW,
    RNT_PRIO_NORMAL,
    RNT_PRIO_HIGH,
};

typedef struct rnt_watch {
    char         *path;
    regex_t       exclude;
    bool          has_exclude;
    bool          recursive;
    uint64_t      retention_ns;     /* 0 = infinite */
    enum rnt_priority priority;
    bool          compress;
} rnt_watch_t;

typedef struct rnt_config {
    /* Paths */
    char *store_path;
    char *index_path;
    char *log_path;
    char *control_socket;

    /* Logging */
    enum rnt_log_level log_level;

    /* Watch list */
    rnt_watch_t *watches;
    size_t       n_watches;

    /* Tuning */
    unsigned inotify_max_watches_warn;
    unsigned debounce_ms;
    unsigned queue_poll_ms;

    /* Storage policy */
    bool      compress_default;
    size_t    compress_min_size;
    enum { FSYNC_FULL, FSYNC_DATA, FSYNC_NONE } fsync_mode;
    unsigned  store_fmode;

    /* Retention defaults */
    uint64_t  retention_default_ns;
    bool      gc_after_prune;
} rnt_config_t;

rnt_config_t *config_load(const char *path);
void          config_free(rnt_config_t *cfg);

/* Validation: check syntax, paths exist, regexes compile, etc.
 * Returns 0 ok, prints diagnostics on errors. */
int config_validate(const rnt_config_t *cfg);

#endif /* RENATUM_CONFIG_H */
