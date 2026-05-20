/*
 * snapshot.h — file → CAS pipeline
 *
 * Given an event (path + mask), decide whether to capture and do so.
 * Handles the (mtime, size) pre-check, SHA computation, idempotency
 * lookup, store write, and index insertion as a single logical unit.
 */

#ifndef HYPERSLEEP_SNAPSHOT_H
#define HYPERSLEEP_SNAPSHOT_H

#include "store.h"
#include "index.h"
#include <stdint.h>

typedef struct hs_snapshot hs_snapshot_t;

hs_snapshot_t *snapshot_create(hs_store_t *store, hs_index_t *index);
void            snapshot_destroy(hs_snapshot_t *s);

/* Process a single (debounced) event. */
int snapshot_handle(hs_snapshot_t *s,
                    const char *path,
                    uint32_t event_mask,
                    uint32_t cookie);

/* Force-capture a path regardless of recent state (used by pre-snapshot
 * before destructive --force restore). */
int snapshot_force(hs_snapshot_t *s,
                   const char *path,
                   uint8_t out_sha[HS_SHA_LEN]);

/* Walk a tree and capture anything whose (mtime, size) differs from the
 * latest index entry. Used for IN_Q_OVERFLOW recovery and `--rescan`. */
int snapshot_rescan(hs_snapshot_t *s, const char *root);

#endif /* HYPERSLEEP_SNAPSHOT_H */
