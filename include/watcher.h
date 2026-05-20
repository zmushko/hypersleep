/*
 * watcher.h — librnotify event loop wrapper
 *
 * Owns the librnotify Notify* handle and pumps events into the debouncer.
 * Drives IN_Q_OVERFLOW rescans and per-event metrics.
 */

#ifndef HYPERSLEEP_WATCHER_H
#define HYPERSLEEP_WATCHER_H

#include "hypersleep.h"
#include "config.h"
#include "snapshot.h"

#include <signal.h>

typedef struct hs_watcher hs_watcher_t;

/* Create a watcher driven by `cfg`. The watcher dispatches every
 * coalesced event through `snapshot` (the caller retains ownership
 * of both `cfg` and `snapshot`). The volatile flag `stop_flag` is
 * polled by watcher_run on every epoll wake; setting it to zero
 * from a signal handler causes a clean shutdown. */
hs_watcher_t *watcher_create(const hs_config_t *cfg,
                              hs_snapshot_t *snapshot,
                              volatile sig_atomic_t *stop_flag);
void           watcher_destroy(hs_watcher_t *w);

/* Run the event loop until *stop_flag drops to zero. */
int watcher_run(hs_watcher_t *w);

/* Used by control socket to trigger synchronous capture. */
int watcher_force_snapshot(hs_watcher_t *w, const char *path,
                           uint8_t out_sha[HS_SHA_LEN]);

/* Attach an external readable fd to the watcher's epoll loop. The
 * `fn` callback fires when the fd becomes readable; it must drain
 * the fd before returning. Only one external attachment is
 * supported at a time (sufficient for the control socket); a
 * second call returns EBUSY. */
int watcher_attach_fd(hs_watcher_t *w, int fd,
                      void (*fn)(void *user), void *user);

#endif /* HYPERSLEEP_WATCHER_H */
