/*
 * debounce.h — per-path event coalescing
 *
 * Atomic-save patterns (vim: write tmp + rename) and bulk operations
 * generate bursts of events on the same path. The debouncer accumulates
 * events per path and flushes after `window_ms` of quiet, dispatching a
 * single capture for the file's stable state.
 */

#ifndef HYPERSLEEP_DEBOUNCE_H
#define HYPERSLEEP_DEBOUNCE_H

#include <stdint.h>

typedef struct hs_debounce hs_debounce_t;

typedef void (*hs_debounce_cb)(const char *path,
                                uint32_t accumulated_mask,
                                uint32_t cookie,
                                void *user);

hs_debounce_t *debounce_create(unsigned window_ms,
                                hs_debounce_cb cb,
                                void *user);
void            debounce_destroy(hs_debounce_t *d);

/* Push a new event. Resets the timer for `path`. */
int debounce_push(hs_debounce_t *d,
                  const char *path,
                  uint32_t mask,
                  uint32_t cookie);

/* Called periodically (e.g. on every watcher poll timeout) to flush
 * entries whose quiet window has elapsed. */
void debounce_tick(hs_debounce_t *d);

/* Drain everything synchronously, regardless of timer state. Used on
 * shutdown. */
void debounce_flush_all(hs_debounce_t *d);

#endif /* HYPERSLEEP_DEBOUNCE_H */
