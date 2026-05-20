/*
 * config.h — parsed configuration
 *
 * The config file is line-oriented; see docs/config.md for the format.
 */

#ifndef HYPERSLEEP_CONFIG_H
#define HYPERSLEEP_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <regex.h>

enum hs_log_level {
    HS_LOG_DEBUG,
    HS_LOG_INFO,
    HS_LOG_WARN,
    HS_LOG_ERROR,
};

enum hs_priority {
    HS_PRIO_LOW,
    HS_PRIO_NORMAL,
    HS_PRIO_HIGH,
};

typedef struct hs_watch {
    char         *path;
    regex_t       exclude;
    bool          has_exclude;
    bool          recursive;
    uint64_t      retention_ns;     /* 0 = infinite */
    enum hs_priority priority;
    bool          compress;
    /* Parser bookkeeping: true if the watch directive set the field
     * explicitly. Used by config_load to apply retention-default /
     * compress-default after the whole file is parsed, so directive
     * order in the config file does not matter. */
    bool          set_retention;
    bool          set_compress;
} hs_watch_t;

typedef struct hs_config {
    /* Paths */
    char *store_path;
    char *index_path;
    char *log_path;
    char *control_socket;
    char *lock_path;

    /* Logging */
    enum hs_log_level log_level;

    /* Watch list */
    hs_watch_t *watches;
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
} hs_config_t;

hs_config_t *config_load(const char *path);
void          config_free(hs_config_t *cfg);

/* Validation: check syntax, paths exist, regexes compile, etc.
 * Returns 0 ok, prints diagnostics on errors. */
int config_validate(const hs_config_t *cfg);

#endif /* HYPERSLEEP_CONFIG_H */
