/*
 * retention.h — blob-level garbage collection after an index prune.
 *
 * index_prune_by_age leaves an LMDB-consistent state (by_sha entries
 * with zero refcount get removed in the same transaction), but the
 * actual blob files in the CAS remain. retention_sweep_orphans
 * walks the CAS and unlinks every blob the index no longer
 * references.
 */

#ifndef RENATUM_RETENTION_H
#define RENATUM_RETENTION_H

#include "index.h"
#include "store.h"

#include <stddef.h>

/* Sweep the CAS for blobs whose by_sha refcount is zero (or absent).
 * Unlinks each such blob from disk. Returns 0 on success and stores
 * the number of removed blobs into *out_removed (may be NULL).
 * Returns -1 on hard error. */
int retention_sweep_orphans(rnt_index_t *idx, rnt_store_t *store,
                            size_t *out_removed);

#endif /* RENATUM_RETENTION_H */
