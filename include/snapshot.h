/*
 * snapshot.h — file → CAS pipeline
 *
 * Given an event (path + mask), decide whether to capture and do so.
 * Handles the (mtime, size) pre-check, SHA computation, idempotency
 * lookup, store write, and index insertion as a single logical unit.
 */

#ifndef RENATUM_SNAPSHOT_H
#define RENATUM_SNAPSHOT_H

#include "store.h"
#include "index.h"
#include <stdint.h>

typedef struct rnt_snapshot rnt_snapshot_t;

rnt_snapshot_t *snapshot_create(rnt_store_t *store, rnt_index_t *index);
void            snapshot_destroy(rnt_snapshot_t *s);

/* Process a single (debounced) event. */
int snapshot_handle(rnt_snapshot_t *s,
                    const char *path,
                    uint32_t event_mask,
                    uint32_t cookie);

/* Force-capture a path regardless of recent state (used by pre-snapshot
 * before destructive --force restore). */
int snapshot_force(rnt_snapshot_t *s,
                   const char *path,
                   uint8_t out_sha[RNT_SHA_LEN]);

/* Walk a tree and capture anything whose (mtime, size) differs from the
 * latest index entry. Used for IN_Q_OVERFLOW recovery and `--rescan`. */
int snapshot_rescan(rnt_snapshot_t *s, const char *root);

#endif /* RENATUM_SNAPSHOT_H */
