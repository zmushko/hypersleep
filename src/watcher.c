/*
 * watcher.c — librnotify event loop
 *
 * Owns the Notify* handle from librnotify and pumps events into the
 * debouncer. On every poll-timeout, calls debounce_tick() to flush any
 * entries whose quiet window has elapsed.
 *
 * Handles IN_Q_OVERFLOW by triggering a full rescan.
 */

#include "watcher.h"
#include "snapshot.h"
#include "debounce.h"
#include "store.h"
#include "index.h"
#include "config.h"
#include "log.h"

#include "rnotify.h"   /* from third_party/librnotify */

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/inotify.h>

extern volatile sig_atomic_t g_running;  /* defined in main.c */
extern volatile sig_atomic_t g_reload;

struct rnt_watcher {
    const rnt_config_t *cfg;
    Notify             *ntf;
    rnt_store_t        *store;
    rnt_index_t        *index;
    rnt_snapshot_t     *snap;
    rnt_debounce_t     *deb;
};

static void on_debounced_event(const char *path, uint32_t mask,
                               uint32_t cookie, void *user) {
    rnt_watcher_t *w = (rnt_watcher_t *)user;
    snapshot_handle(w->snap, path, mask, cookie);
}

rnt_watcher_t *watcher_create(const rnt_config_t *cfg) {
    rnt_watcher_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->cfg = cfg;

    /* TODO:
     *   - open store/index here (or accept them as args from main.c)
     *   - build paths[] and combined exclude regex from cfg->watches
     *   - call initNotify(paths, mask, exclude)
     *   - create debouncer with cb=on_debounced_event
     *   - create snapshot pipeline
     */

    return w;
}

void watcher_destroy(rnt_watcher_t *w) {
    if (!w) return;
    if (w->deb)   debounce_destroy(w->deb);
    if (w->snap)  snapshot_destroy(w->snap);
    if (w->ntf)   freeNotify(w->ntf);
    free(w);
}

int watcher_run(rnt_watcher_t *w) {
    /* The mask we ask librnotify for. */
    /* TODO: pass this in initNotify() — currently illustrative only. */
    (void)(IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM
         | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF
         | IN_ATTRIB);

    while (g_running) {
        if (g_reload) {
            log_info("SIGHUP received — reloading config");
            /* TODO: reload watches diff */
            g_reload = 0;
        }

        char    *path   = NULL;
        uint32_t mask   = 0;
        uint32_t cookie = 0;

        /* librnotify API: waitNotify(handle, &path, &mask, timeout_ms, &cookie)
         * returns 0 on event, >0 on timeout, <0 on error.
         * Adjust call signature to whatever librnotify actually exports. */
        int rc = -1;
        /* rc = waitNotify(w->ntf, &path, &mask, w->cfg->queue_poll_ms, &cookie); */

        if (rc == 0 && path) {
            if (mask & IN_Q_OVERFLOW) {
                log_warn("IN_Q_OVERFLOW — triggering full rescan");
                for (size_t i = 0; i < w->cfg->n_watches; i++) {
                    snapshot_rescan(w->snap, w->cfg->watches[i].path);
                }
            } else {
                debounce_push(w->deb, path, mask, cookie);
            }
            free(path);
        } else if (rc > 0) {
            /* poll timeout — natural moment to flush expired debounced events */
            debounce_tick(w->deb);
        } else if (rc < 0) {
            log_error("waitNotify failed");
            break;
        }
    }

    debounce_flush_all(w->deb);
    return 0;
}

int watcher_force_snapshot(rnt_watcher_t *w, const char *path,
                           uint8_t out_sha[RNT_SHA_LEN]) {
    return snapshot_force(w->snap, path, out_sha);
}
