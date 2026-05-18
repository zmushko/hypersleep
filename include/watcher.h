/*
 * watcher.h — librnotify event loop wrapper
 *
 * Owns the librnotify Notify* handle and pumps events into the debouncer.
 * Drives IN_Q_OVERFLOW rescans and per-event metrics.
 */

#ifndef RENATUM_WATCHER_H
#define RENATUM_WATCHER_H

#include "config.h"

typedef struct rnt_watcher rnt_watcher_t;

rnt_watcher_t *watcher_create(const rnt_config_t *cfg);
void           watcher_destroy(rnt_watcher_t *w);

/* Run the event loop until SIGTERM / SIGINT. */
int watcher_run(rnt_watcher_t *w);

/* Used by control socket to trigger synchronous capture. */
int watcher_force_snapshot(rnt_watcher_t *w, const char *path,
                           uint8_t out_sha[RNT_SHA_LEN]);

#endif /* RENATUM_WATCHER_H */
