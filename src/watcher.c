/*
 * watcher.c — librnotify v3 driver. One Notify per cfg watch
 * directive; all notify fds multiplex through a single epoll. Events
 * are filtered by the per-watch exclude regex, pushed into the
 * debouncer, and dispatched into snapshot.c when the quiet window
 * elapses.
 *
 * librnotify v3 quirk we work around: with timeout=0, waitNotify
 * returns 0 for both "event delivered" and "no event ready"
 * (because the documented "returns the original timeout" coincides
 * with the success code). We disambiguate by checking `path` —
 * librnotify only allocates the out-path when an event is actually
 * delivered. Pre-set it to NULL before each call.
 *
 * Per epoll wake:
 *   1. drain each ready notify fd via non-blocking waitNotify.
 *   2. IN_Q_OVERFLOW on a unit triggers debounce_flush_all and
 *      snapshot_rescan for that root — safety property #4 ("no
 *      event silently lost").
 *   3. exclude regex is applied against the full path here rather
 *      than in librnotify, since librnotify matches only the entry
 *      name and docs/config.md promises full-path semantics.
 *   4. debounce_tick fires at the end of every wake (including
 *      timeout-only wakes) so a quiet window can elapse during a
 *      lull.
 *
 * Shutdown: the loop polls *stop_flag every wake. SIGTERM/SIGINT
 * handlers in main.c set it to zero; worst-case latency is one
 * queue_poll_ms.
 */

#include "watcher.h"
#include "config.h"
#include "debounce.h"
#include "log.h"
#include "snapshot.h"

#include "rnotify.h"     /* third_party/librnotify */

#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <unistd.h>

#define DRAIN_BATCH_MAX 256

struct watch_unit {
    Notify             *notify;
    const rnt_watch_t  *cfg;     /* borrowed pointer into rnt_config_t */
    int                 fd;      /* cached notifyFd(notify) */
};

struct ext_attachment {
    bool   active;
    int    fd;
    void (*fn)(void *user);
    void  *user;
};

struct rnt_watcher {
    const rnt_config_t        *cfg;
    rnt_snapshot_t            *snapshot;
    rnt_debounce_t            *debouncer;
    struct watch_unit         *units;
    size_t                     n_units;
    int                        epfd;
    volatile sig_atomic_t     *stop_flag;
    struct ext_attachment      ext;
};

/* ------------------------------------------------------------------ */
/* debouncer callback                                                 */
/* ------------------------------------------------------------------ */

static void dispatch(const char *path, uint32_t mask, uint32_t cookie,
                     void *user)
{
    rnt_watcher_t *w = user;
    /* snapshot_handle logs its own specific failure; we choose to
     * keep running rather than abort the daemon on a single bad
     * capture. */
    (void)snapshot_handle(w->snapshot, path, mask, cookie);
}

/* ------------------------------------------------------------------ */
/* setup                                                              */
/* ------------------------------------------------------------------ */

static int epoll_add(int epfd, int fd, void *ptr)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = EPOLLIN;
    ev.data.ptr = ptr;
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

static int install_one_watch(rnt_watcher_t *w, const rnt_watch_t *spec)
{
    /* librnotify gets NULL as the exclude regex; we re-filter the
     * full path ourselves in drain_unit. */
    Notify *n = initNotify(spec->path, IN_ALL_EVENTS, NULL);
    if (n == NULL) {
        if (errno == ENOSPC) {
            log_warn("watcher: %s: out of inotify watches; consider "
                     "sysctl -w fs.inotify.max_user_watches=524288",
                     spec->path);
        } else {
            log_warn("watcher: initNotify(%s) failed: %s",
                     spec->path, strerror(errno));
        }
        return -1;
    }
    int fd = notifyFd(n);
    if (fd < 0) {
        log_error("watcher: notifyFd(%s) failed", spec->path);
        freeNotify(n);
        return -1;
    }

    struct watch_unit *u = &w->units[w->n_units];
    u->notify = n;
    u->cfg    = spec;
    u->fd     = fd;

    if (epoll_add(w->epfd, fd, u) < 0) {
        log_error("watcher: epoll_ctl ADD for %s: %s",
                  spec->path, strerror(errno));
        freeNotify(n);
        return -1;
    }
    w->n_units++;
    log_info("watcher: watching %s", spec->path);
    return 0;
}

rnt_watcher_t *watcher_create(const rnt_config_t *cfg,
                              rnt_snapshot_t *snapshot,
                              volatile sig_atomic_t *stop_flag)
{
    if (cfg == NULL || snapshot == NULL || stop_flag == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (cfg->n_watches == 0) {
        log_error("watcher: no watch directives in config");
        errno = ENOENT;
        return NULL;
    }

    rnt_watcher_t *w = calloc(1, sizeof(*w));
    if (w == NULL) return NULL;
    w->cfg       = cfg;
    w->snapshot  = snapshot;
    w->stop_flag = stop_flag;
    w->epfd      = -1;

    w->units = calloc(cfg->n_watches, sizeof(*w->units));
    if (w->units == NULL) goto err;

    w->debouncer = debounce_create(cfg->debounce_ms, dispatch, w);
    if (w->debouncer == NULL) goto err;

    w->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (w->epfd < 0) {
        log_error("watcher: epoll_create1: %s", strerror(errno));
        goto err;
    }

    /* Tolerate per-watch failure: one bad path should not nuke the
     * whole daemon. Only bail out if nothing got installed at all. */
    for (size_t i = 0; i < cfg->n_watches; i++) {
        install_one_watch(w, &cfg->watches[i]);
    }
    if (w->n_units == 0) {
        log_error("watcher: no watch could be installed");
        goto err;
    }
    return w;

err:
    watcher_destroy(w);
    return NULL;
}

void watcher_destroy(rnt_watcher_t *w)
{
    if (w == NULL) return;
    if (w->debouncer) debounce_destroy(w->debouncer);
    if (w->units) {
        for (size_t i = 0; i < w->n_units; i++) {
            if (w->units[i].notify) freeNotify(w->units[i].notify);
        }
        free(w->units);
    }
    if (w->epfd >= 0) close(w->epfd);
    free(w);
}

/* ------------------------------------------------------------------ */
/* event drain                                                        */
/* ------------------------------------------------------------------ */

static void drain_unit(rnt_watcher_t *w, struct watch_unit *u)
{
    for (int i = 0; i < DRAIN_BATCH_MAX; i++) {
        char    *path   = NULL;
        uint32_t mask   = 0;
        uint32_t cookie = 0;

        int rc = waitNotify(u->notify, &path, &mask, /*timeout*/ 0, &cookie);
        if (rc == -1) {
            if (errno != EINTR && errno != EAGAIN) {
                log_warn("watcher: waitNotify on %s: %s",
                         u->cfg->path, strerror(errno));
            }
            free(path);
            return;
        }
        /* librnotify v3: timeout=0 returns 0 both on "event ready"
         * and on "nothing pending". Disambiguate via `path`. */
        if (path == NULL) {
            return;
        }

        if (mask & IN_Q_OVERFLOW) {
            log_warn("watcher: IN_Q_OVERFLOW on %s; flushing debouncer "
                     "and rescanning", u->cfg->path);
            debounce_flush_all(w->debouncer);
            if (snapshot_rescan(w->snapshot, u->cfg->path) < 0) {
                log_error("watcher: rescan of %s failed", u->cfg->path);
            }
            free(path);
            continue;
        }

        if (u->cfg->has_exclude
            && regexec(&u->cfg->exclude, path, 0, NULL, 0) == 0)
        {
            log_debug("watcher: excluded %s", path);
            free(path);
            continue;
        }

        if (debounce_push(w->debouncer, path, mask, cookie) < 0) {
            log_warn("watcher: debounce_push %s: %s",
                     path, strerror(errno));
        }
        free(path);
    }
    /* Hit the batch cap; remaining queue picked up on the next wake. */
}

/* ------------------------------------------------------------------ */
/* event loop                                                         */
/* ------------------------------------------------------------------ */

int watcher_run(rnt_watcher_t *w)
{
    if (w == NULL) {
        errno = EINVAL;
        return -1;
    }

    struct epoll_event events[16];
    int poll_ms = (int)w->cfg->queue_poll_ms;
    if (poll_ms <= 0) poll_ms = 500;

    while (*w->stop_flag) {
        int n = epoll_wait(w->epfd, events,
                           (int)(sizeof(events) / sizeof(events[0])),
                           poll_ms);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("watcher: epoll_wait: %s", strerror(errno));
            return -1;
        }
        for (int i = 0; i < n; i++) {
            /* events[i].data.ptr == NULL is the sentinel for the
             * external attachment (see watcher_attach_fd). */
            if (events[i].data.ptr == NULL) {
                if (w->ext.active && w->ext.fn) {
                    w->ext.fn(w->ext.user);
                }
            } else {
                drain_unit(w, events[i].data.ptr);
            }
        }
        /* Tick the debouncer regardless of why we woke — pending
         * entries whose quiet window has elapsed need a push even
         * if no new events arrived in this iteration. */
        debounce_tick(w->debouncer);
    }

    log_info("watcher: stop_flag dropped, draining debouncer");
    debounce_flush_all(w->debouncer);
    return 0;
}

/* ------------------------------------------------------------------ */
/* control-socket entry point                                         */
/* ------------------------------------------------------------------ */

int watcher_force_snapshot(rnt_watcher_t *w, const char *path,
                           uint8_t out_sha[RNT_SHA_LEN])
{
    if (w == NULL || path == NULL || out_sha == NULL) {
        errno = EINVAL;
        return -1;
    }
    return snapshot_force(w->snapshot, path, out_sha);
}

int watcher_attach_fd(rnt_watcher_t *w, int fd,
                      void (*fn)(void *user), void *user)
{
    if (w == NULL || fn == NULL || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (w->ext.active) {
        errno = EBUSY;
        return -1;
    }
    /* NULL ptr is the sentinel for "external" in the dispatch
     * loop; watch_unit pointers are always non-NULL. */
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = EPOLLIN;
    ev.data.ptr = NULL;
    if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        log_error("watcher: epoll_ctl ADD external fd %d: %s",
                  fd, strerror(errno));
        return -1;
    }
    w->ext.active = true;
    w->ext.fd     = fd;
    w->ext.fn     = fn;
    w->ext.user   = user;
    return 0;
}
