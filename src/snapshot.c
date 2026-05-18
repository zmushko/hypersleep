/*
 * snapshot.c — file → CAS pipeline
 *
 * For each debounced event:
 *   1. Filter by event type (deletes / moves / writes handled separately).
 *   2. lstat the path. If not a regular file, return.
 *   3. Cheap pre-check: same (mtime, size) as latest index entry?
 *      If yes, this is most likely a duplicate event — return without
 *      doing any I/O on the file.
 *   4. Compute SHA-256.
 *   5. Idempotency: has this (path, sha) been captured before? If yes,
 *      a `touch` updated mtime but content is unchanged — skip.
 *   6. store_put() the blob. No-op if SHA already in CAS.
 *   7. index_insert() the version. by_sha refcount auto-incremented.
 *
 * This design tolerates the duplicate IN_CREATE events that librnotify
 * may emit when closing the recursive-watch race condition. The CAS
 * naturally deduplicates content; the (path, sha) idempotency check in
 * the index naturally deduplicates events.
 */

#include "snapshot.h"
#include "store.h"
#include "index.h"
#include "log.h"
#include "renatum.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <fcntl.h>

struct rnt_snapshot {
    rnt_store_t *store;
    rnt_index_t *index;
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

rnt_snapshot_t *snapshot_create(rnt_store_t *store, rnt_index_t *index) {
    rnt_snapshot_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->store = store;
    s->index = index;
    return s;
}

void snapshot_destroy(rnt_snapshot_t *s) {
    free(s);
}

static int handle_delete(rnt_snapshot_t *s, const char *path) {
    rnt_deletion_t del = { 0 };
    /* TODO: populate del.uid (from /proc?), last_sha, last_size from latest entry */
    return index_record_deletion(s->index, path, now_ns(), &del);
}

static int handle_move_from(rnt_snapshot_t *s,
                            uint32_t cookie, const char *path) {
    return index_moves_pending(s->index, cookie, path);
}

static int handle_move_to(rnt_snapshot_t *s,
                          uint32_t cookie, const char *path) {
    char *from = NULL;
    if (index_moves_resolve(s->index, cookie, path, &from) == 0 && from) {
        log_info("[mv ] %s -> %s", from, path);
        free(from);
    }
    /* Fall through: the destination is a new path that must be captured. */
    return 0;
}

static int capture_file(rnt_snapshot_t *s, const char *path,
                        uint32_t event_mask, uint16_t extra_flags) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode))  return 0;

    /* Cheap duplicate-event check */
    rnt_version_t latest;
    if (index_get_latest(s->index, path, &latest) == 0) {
        if (latest.entry.size == (uint64_t)st.st_size
            && latest.entry.mtime_sec  == st.st_mtim.tv_sec
            && latest.entry.mtime_nsec == st.st_mtim.tv_nsec) {
            log_debug("[skip] %s (mtime+size unchanged)", path);
            return 0;
        }
    }

    /* Real capture: store_put hashes internally and writes blob if new */
    uint8_t sha[RNT_SHA_LEN];
    uint16_t flags = extra_flags;
    if (store_put(s->store, path, sha, &flags) != 0) {
        log_warn("store_put failed for %s", path);
        return -1;
    }

    /* Idempotency: same content already in history for this path? */
    if (index_has_path_sha(s->index, path, sha)) {
        log_debug("[dup ] %s (path,sha already indexed)", path);
        return 0;
    }

    int rc = index_insert(s->index, path, sha, &st,
                          event_mask, now_ns(), flags);
    if (rc < 0) {
        log_warn("index_insert failed for %s", path);
        return -1;
    }

    char hex[9];
    for (int i = 0; i < 4; i++)
        snprintf(hex + i*2, 3, "%02x", sha[i]);
    log_info("[cap ] %s sha=%s..", path, hex);
    return 0;
}

int snapshot_handle(rnt_snapshot_t *s, const char *path,
                    uint32_t event_mask, uint32_t cookie) {
    if (event_mask & (IN_DELETE | IN_DELETE_SELF))
        return handle_delete(s, path);

    if (event_mask & IN_MOVED_FROM)
        return handle_move_from(s, cookie, path);

    if (event_mask & IN_MOVED_TO)
        handle_move_to(s, cookie, path);
        /* fall through to capture the destination as a new file */

    if (event_mask & (IN_CLOSE_WRITE | IN_MOVED_TO))
        return capture_file(s, path, event_mask, 0);

    return 0;
}

int snapshot_force(rnt_snapshot_t *s, const char *path,
                   uint8_t out_sha[RNT_SHA_LEN]) {
    if (capture_file(s, path, IN_CLOSE_WRITE, RNT_FLAG_PRESNAPSHOT) != 0)
        return -1;
    /* TODO: fetch sha back from index for return */
    (void)out_sha;
    return 0;
}

int snapshot_rescan(rnt_snapshot_t *s, const char *root) {
    /* TODO: nftw(root, ..., FTW_PHYS) → for each regular file,
     * call capture_file() with synthetic flag set */
    (void)s; (void)root;
    log_info("rescan of %s requested (not yet implemented)", root);
    return 0;
}
