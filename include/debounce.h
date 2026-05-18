/*
 * debounce.h — per-path event coalescing
 *
 * Atomic-save patterns (vim: write tmp + rename) and bulk operations
 * generate bursts of events on the same path. The debouncer accumulates
 * events per path and flushes after `window_ms` of quiet, dispatching a
 * single capture for the file's stable state.
 */

#ifndef RENATUM_DEBOUNCE_H
#define RENATUM_DEBOUNCE_H

#include <stdint.h>

typedef struct rnt_debounce rnt_debounce_t;

typedef void (*rnt_debounce_cb)(const char *path,
                                uint32_t accumulated_mask,
                                uint32_t cookie,
                                void *user);

rnt_debounce_t *debounce_create(unsigned window_ms,
                                rnt_debounce_cb cb,
                                void *user);
void            debounce_destroy(rnt_debounce_t *d);

/* Push a new event. Resets the timer for `path`. */
int debounce_push(rnt_debounce_t *d,
                  const char *path,
                  uint32_t mask,
                  uint32_t cookie);

/* Called periodically (e.g. on every watcher poll timeout) to flush
 * entries whose quiet window has elapsed. */
void debounce_tick(rnt_debounce_t *d);

/* Drain everything synchronously, regardless of timer state. Used on
 * shutdown. */
void debounce_flush_all(rnt_debounce_t *d);

#endif /* RENATUM_DEBOUNCE_H */
