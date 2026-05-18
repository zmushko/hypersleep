/*
 * store.h — content-addressable storage for captured blobs
 *
 * Layout: <root>/<sha[0:2]>/<sha[2:]>
 * Blobs are either raw bytes or zstd-framed when RNT_FLAG_COMPRESSED is set
 * in the corresponding file entry.
 *
 * Thread safety: store_put() is safe for a single writer (the daemon).
 * store_get_* are safe for multiple concurrent readers.
 */

#ifndef RENATUM_STORE_H
#define RENATUM_STORE_H

#include "renatum.h"
#include <stddef.h>

typedef struct rnt_store rnt_store_t;

/* Open or create the CAS rooted at `path`. */
rnt_store_t *store_open(const char *path);
void         store_close(rnt_store_t *s);

/* Copy the file at `src_path` into the store under its SHA-256 digest.
 * On success, writes the digest into out_sha (must be RNT_SHA_LEN bytes).
 * No-op (with success) if a blob with this digest already exists.
 * Performs fsync on the blob and its directory before returning. */
int store_put(rnt_store_t *s,
              const char *src_path,
              uint8_t out_sha[RNT_SHA_LEN],
              uint16_t *out_flags);

/* Open the blob with the given SHA. Returns a file descriptor positioned
 * at offset 0, or -1 on error. Caller closes. */
int store_open_blob(rnt_store_t *s, const uint8_t sha[RNT_SHA_LEN]);

/* True if a blob with this SHA exists in the store. */
bool store_has(rnt_store_t *s, const uint8_t sha[RNT_SHA_LEN]);

/* Remove a blob (used by GC after refcount drops to zero). */
int store_remove(rnt_store_t *s, const uint8_t sha[RNT_SHA_LEN]);

/* Iterate over all blobs (for verify). Callback returns 0 to continue,
 * non-zero to abort iteration with that value. */
typedef int (*rnt_store_iter_fn)(const uint8_t sha[RNT_SHA_LEN],
                                  off_t size,
                                  void *user);
int store_iterate(rnt_store_t *s, rnt_store_iter_fn fn, void *user);

/* Re-hash a blob and compare against its filename. Returns 0 if ok,
 * 1 if mismatch, negative on error. */
int store_verify_blob(rnt_store_t *s, const uint8_t sha[RNT_SHA_LEN]);

#endif /* RENATUM_STORE_H */
