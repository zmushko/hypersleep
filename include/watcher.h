/*
 * watcher.h — librnotify event loop wrapper
 *
 * Owns the librnotify Notify* handle and pumps events into the debouncer.
 * Drives IN_Q_OVERFLOW rescans and per-event metrics.
 */

#ifndef RENATUM_WATCHER_H
#define RENATUM_WATCHER_H

#include "renatum.h"
#include "config.h"
#include "snapshot.h"

#include <signal.h>

typedef struct rnt_watcher rnt_watcher_t;

/* Create a watcher driven by `cfg`. The watcher dispatches every
 * coalesced event through `snapshot` (the caller retains ownership
 * of both `cfg` and `snapshot`). The volatile flag `stop_flag` is
 * polled by watcher_run on every epoll wake; setting it to zero
 * from a signal handler causes a clean shutdown. */
rnt_watcher_t *watcher_create(const rnt_config_t *cfg,
                              rnt_snapshot_t *snapshot,
                              volatile sig_atomic_t *stop_flag);
void           watcher_destroy(rnt_watcher_t *w);

/* Run the event loop until *stop_flag drops to zero. */
int watcher_run(rnt_watcher_t *w);

/* Used by control socket to trigger synchronous capture. */
int watcher_force_snapshot(rnt_watcher_t *w, const char *path,
                           uint8_t out_sha[RNT_SHA_LEN]);

#endif /* RENATUM_WATCHER_H */
