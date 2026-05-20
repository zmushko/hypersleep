/*
 * debounce.c — per-path event coalescing.
 *
 * Atomic-save patterns (vim's swap+rename, rsync bulk writes) and the
 * synthetic IN_CREATE + IN_CLOSE_WRITE pair librnotify emits during
 * its initial recursive scan produce bursts of events on a single
 * path. Snapshotting each one would waste I/O and pollute the
 * version history with intermediate states.
 *
 * The debouncer accumulates events keyed by path. Every push resets
 * the entry's last-event timestamp; debounce_tick (called from the
 * watcher loop on every poll timeout) flushes entries whose quiet
 * window has elapsed. The flushed event carries the OR of every mask
 * seen in the burst, so the consumer (snapshot.c) sees the complete
 * intent even though only one capture happens.
 *
 * Data structure: a singly-linked list. Pending entries are typically
 * fewer than ten during steady state; even mass rsync rarely exceeds
 * a few hundred concurrently. A real hash map would be faster but
 * adds enough code to be a regression risk for v0.1.0. Revisit if
 * stress tests find a hot spot.
 *
 * Clock: CLOCK_MONOTONIC. We do not want NTP adjustments or wall-
 * clock jumps to make a quiet window appear to elapse prematurely
 * or get stuck.
 */

#include "debounce.h"
#include "log.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct deb_entry {
    char              *path;
    uint64_t           last_ts_ns;
    uint32_t           mask;
    uint32_t           cookie;
    struct deb_entry  *next;
};

struct hs_debounce {
    uint64_t            window_ns;
    hs_debounce_cb     cb;
    void               *user;
    struct deb_entry   *head;
};

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

hs_debounce_t *debounce_create(unsigned window_ms,
                                hs_debounce_cb cb,
                                void *user)
{
    if (cb == NULL) {
        errno = EINVAL;
        return NULL;
    }
    hs_debounce_t *d = calloc(1, sizeof(*d));
    if (d == NULL) return NULL;
    d->window_ns = (uint64_t)window_ms * 1000000ULL;
    d->cb        = cb;
    d->user      = user;
    return d;
}

static void free_entry(struct deb_entry *e)
{
    if (e == NULL) return;
    free(e->path);
    free(e);
}

void debounce_destroy(hs_debounce_t *d)
{
    if (d == NULL) return;
    struct deb_entry *e = d->head;
    while (e != NULL) {
        struct deb_entry *next = e->next;
        free_entry(e);
        e = next;
    }
    free(d);
}

int debounce_push(hs_debounce_t *d, const char *path,
                  uint32_t mask, uint32_t cookie)
{
    if (d == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }

    uint64_t now = mono_ns();

    /* Existing entry for this path? OR the new mask in and reset
     * the timer. The cookie is updated to the most recent non-zero
     * value — for an IN_MOVED_FROM/_TO pair on the same path
     * (unusual but possible: mv foo foo) we want the destination's
     * cookie to win so the consumer can pair it up. */
    for (struct deb_entry *e = d->head; e != NULL; e = e->next) {
        if (strcmp(e->path, path) == 0) {
            e->mask       |= mask;
            e->last_ts_ns  = now;
            if (cookie != 0) e->cookie = cookie;
            return 0;
        }
    }

    struct deb_entry *e = calloc(1, sizeof(*e));
    if (e == NULL) return -1;
    e->path = strdup(path);
    if (e->path == NULL) { free(e); return -1; }
    e->last_ts_ns = now;
    e->mask       = mask;
    e->cookie     = cookie;
    e->next       = d->head;
    d->head       = e;
    return 0;
}

/* Detach entries that are due for dispatch. Returns the head of a
 * private list the caller will walk + free. Splitting the walk from
 * the dispatch keeps the public list safe even if the user callback
 * pushes new events for the same path. */
static struct deb_entry *take_due(hs_debounce_t *d, uint64_t now)
{
    struct deb_entry *due = NULL;
    struct deb_entry **link = &d->head;
    while (*link != NULL) {
        struct deb_entry *e = *link;
        if (now - e->last_ts_ns >= d->window_ns) {
            *link = e->next;
            e->next = due;
            due = e;
        } else {
            link = &e->next;
        }
    }
    return due;
}

void debounce_tick(hs_debounce_t *d)
{
    if (d == NULL) return;
    uint64_t now = mono_ns();
    struct deb_entry *due = take_due(d, now);
    while (due != NULL) {
        struct deb_entry *next = due->next;
        d->cb(due->path, due->mask, due->cookie, d->user);
        free_entry(due);
        due = next;
    }
}

void debounce_flush_all(hs_debounce_t *d)
{
    if (d == NULL) return;
    struct deb_entry *e = d->head;
    d->head = NULL;
    while (e != NULL) {
        struct deb_entry *next = e->next;
        d->cb(e->path, e->mask, e->cookie, d->user);
        free_entry(e);
        e = next;
    }
}
