/*
 * index.h — LMDB-backed index over captured file versions
 *
 * The index has several named sub-databases:
 *   files       — versions of paths, keyed by (path || 0x00 || ts || seq)
 *   by_sha      — refcount of blob references
 *   deletions   — deletion markers
 *   moves       — pending move-from records awaiting their move-to twin
 *   meta        — global key-value (schema version, daemon stats)
 *
 * Concurrency model:
 *   - Daemon holds the writer; CLI tools open read-only environments.
 *   - LMDB enforces single-writer / multi-reader.
 *   - Writer transactions are kept short (single event at a time).
 */

#ifndef HYPERSLEEP_INDEX_H
#define HYPERSLEEP_INDEX_H

#include "hypersleep.h"
#include <sys/stat.h>

typedef struct hs_index hs_index_t;
typedef struct hs_index_cursor hs_index_cursor_t;

enum hs_index_mode {
    HS_IDX_READ,
    HS_IDX_WRITE,
};

hs_index_t *index_open(const char *path, enum hs_index_mode mode);
void         index_close(hs_index_t *idx);

/* Insert a new version entry for `path`. Increments by_sha refcount.
 * Returns 0 on success, 1 if an identical (path, sha) already existed
 * (no insert performed), negative on error. */
int index_insert(hs_index_t *idx,
                 const char *path,
                 const uint8_t sha[HS_SHA_LEN],
                 const struct stat *st,
                 uint32_t event_mask,
                 uint64_t captured_ns,
                 uint16_t flags);

/* Fetch the most recently captured version of `path`. */
int index_get_latest(hs_index_t *idx,
                     const char *path,
                     hs_version_t *out);

/* Look up a version by its (path, sha) — used by the idempotency check. */
bool index_has_path_sha(hs_index_t *idx,
                        const char *path,
                        const uint8_t sha[HS_SHA_LEN]);

/* Cursor iteration over versions of a single path, oldest first. */
hs_index_cursor_t *index_iter_path(hs_index_t *idx, const char *path);
int  index_cursor_next(hs_index_cursor_t *c, hs_version_t *out);
void index_cursor_close(hs_index_cursor_t *c);

/* Deletions */
int index_record_deletion(hs_index_t *idx,
                          const char *path,
                          uint64_t deleted_ns,
                          const hs_deletion_t *del);

/* Moves */
int index_moves_pending(hs_index_t *idx, uint32_t cookie,
                        const char *from_path);
int index_moves_resolve(hs_index_t *idx, uint32_t cookie,
                        const char *to_path,
                        char **out_from /* caller frees */);
int index_moves_gc(hs_index_t *idx, uint64_t older_than_ns);

/* Garbage collection: decrement refcount, delete blobs with refcount 0.
 * Returns number of blobs freed via out_freed (may be NULL). */
int index_prune_by_age(hs_index_t *idx, uint64_t older_than_ns,
                       size_t *out_pruned);

/* Look up the by_sha refcount for a digest. Returns 0 with *out=0 when
 * the SHA is absent (unreferenced), 0 with *out>0 when present, -1 on
 * error. */
int index_sha_refcount(hs_index_t *idx,
                       const uint8_t sha[HS_SHA_LEN],
                       uint32_t *out);

/* Schema migration on open if needed. Returns HS_SCHEMA_VERSION
 * supported, or negative if incompatible. */
int index_schema_check(hs_index_t *idx);

#endif /* HYPERSLEEP_INDEX_H */
