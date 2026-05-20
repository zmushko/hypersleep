/*
 * snapshot.c — file → CAS pipeline.
 *
 * For each debounced event:
 *   1. Filter by event type — deletes / moves / writes split off into
 *      their own helpers before we even touch the file.
 *   2. lstat the path. If it is not a regular file, drop the event.
 *   3. Cheap pre-check: same (mtime, size) as the latest index entry?
 *      If yes, this is almost certainly a duplicate event from
 *      librnotify's synthetic IN_CREATE → IN_CLOSE_WRITE pair or from
 *      atomic-save patterns — return without hashing.
 *   4. store_put streams the file, hashes with SHA-256, writes the
 *      blob (or no-ops if the SHA is already there).
 *   5. Idempotency: has this (path, sha) already been captured? If
 *      yes, a touch updated mtime but content is unchanged — skip the
 *      index_insert so we do not create a phantom version.
 *   6. index_insert records the new version; by_sha refcount is
 *      bumped in the same transaction.
 *
 * The cheap pre-check at step 3 is what makes this loop survive the
 * duplicate IN_CREATE events librnotify emits when closing the
 * recursive-watch race condition. Content-addressing at step 4 makes
 * the blob layer idempotent for free. The (path, sha) lookup at step
 * 5 catches the remaining case where mtime moved but content did not.
 */

#include "snapshot.h"
#include "store.h"
#include "index.h"
#include "log.h"
#include "renatum.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Linux glibc/musl spell the nanosecond mtime st_mtim; macOS uses
 * st_mtimespec. Renatum is Linux-only at runtime, but the shim
 * keeps local compile-checks portable. */
#if defined(__APPLE__) && !defined(st_mtim)
# define st_mtim st_mtimespec
#endif

struct rnt_snapshot {
    rnt_store_t *store;
    rnt_index_t *index;
};

/* nftw() has no user-data pointer, so the rescan worker needs a
 * way to find the active snapshot. The daemon is single-threaded
 * for v0.1.0 and only one rescan runs at a time, so a module-local
 * is enough; the alternative would be a global lock or rewriting
 * the walk by hand. */
static rnt_snapshot_t *g_rescan_target;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

rnt_snapshot_t *snapshot_create(rnt_store_t *store, rnt_index_t *index)
{
    if (store == NULL || index == NULL) {
        errno = EINVAL;
        return NULL;
    }
    rnt_snapshot_t *s = calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    s->store = store;
    s->index = index;
    return s;
}

void snapshot_destroy(rnt_snapshot_t *s)
{
    free(s);
}

/* ------------------------------------------------------------------ */
/* delete / move handlers                                             */
/* ------------------------------------------------------------------ */

static int handle_delete(rnt_snapshot_t *s, const char *path)
{
    rnt_deletion_t del;
    memset(&del, 0, sizeof(del));

    /* Backfill the deletion record from the latest known version so
     * a future `renatum recover` knows what was there. If the path
     * has no history we still record the marker — the operator may
     * have deleted a never-captured file and that fact matters. */
    rnt_version_t latest;
    if (index_get_latest(s->index, path, &latest) == 0) {
        memcpy(del.last_sha256, latest.sha256, RNT_SHA_LEN);
        del.last_size = latest.entry.size;
    }
    /* uid of the deleter is not carried by IN_DELETE; we would have
     * to correlate ts/pid heuristically via /proc. Leave at 0 until
     * a future audit pipeline grows that. */
    del.uid = 0;

    return index_record_deletion(s->index, path, now_ns(), &del);
}

static int handle_move_from(rnt_snapshot_t *s,
                            uint32_t cookie, const char *path)
{
    return index_moves_pending(s->index, cookie, path);
}

static int handle_move_to(rnt_snapshot_t *s,
                          uint32_t cookie, const char *path)
{
    char *from = NULL;
    if (index_moves_resolve(s->index, cookie, path, &from) == 0
        && from != NULL)
    {
        log_info("[mv ] %s -> %s", from, path);
        free(from);
    }
    /* The destination is a new path that still needs a content
     * capture; snapshot_handle's MOVED_TO branch falls through to
     * capture_file for that. */
    return 0;
}

/* ------------------------------------------------------------------ */
/* capture core                                                       */
/* ------------------------------------------------------------------ */

static int capture_file(rnt_snapshot_t *s, const char *path,
                        uint32_t event_mask, uint16_t extra_flags,
                        uint8_t *out_sha)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        /* Race: the file vanished between the event and our look —
         * IN_DELETE may already be queued for it. Not an error. */
        log_debug("[skip] %s (vanished: %s)", path, strerror(errno));
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        return 0;
    }

    /* Pre-check: (mtime, size) unchanged vs. the latest index entry
     * means this is almost certainly a duplicate event. Skip without
     * hashing. */
    rnt_version_t latest;
    if (index_get_latest(s->index, path, &latest) == 0
        && latest.entry.size       == (uint64_t)st.st_size
        && latest.entry.mtime_sec  == (int64_t) st.st_mtim.tv_sec
        && latest.entry.mtime_nsec == (int32_t) st.st_mtim.tv_nsec)
    {
        log_debug("[skip] %s (mtime+size unchanged)", path);
        if (out_sha) memcpy(out_sha, latest.sha256, RNT_SHA_LEN);
        return 0;
    }

    uint8_t sha[RNT_SHA_LEN];
    uint16_t flags = extra_flags;
    if (store_put(s->store, path, sha, &flags) != 0) {
        log_warn("store_put failed for %s", path);
        return -1;
    }

    /* Second idempotency gate: the file's content matches a prior
     * version of this path (touch without edit). The blob is already
     * in CAS; do not create a phantom index entry. */
    if (index_has_path_sha(s->index, path, sha)) {
        log_debug("[dup ] %s (path,sha already indexed)", path);
        if (out_sha) memcpy(out_sha, sha, RNT_SHA_LEN);
        return 0;
    }

    int rc = index_insert(s->index, path, sha, &st,
                          event_mask, now_ns(), flags);
    if (rc < 0) {
        log_warn("index_insert failed for %s", path);
        return -1;
    }

    if (out_sha) memcpy(out_sha, sha, RNT_SHA_LEN);

    /* Print a short SHA prefix in the operator-visible log line. */
    char hex[9];
    for (int i = 0; i < 4; i++) {
        snprintf(hex + i * 2, 3, "%02x", sha[i]);
    }
    log_info("[cap ] %s sha=%s..", path, hex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* event dispatch                                                     */
/* ------------------------------------------------------------------ */

int snapshot_handle(rnt_snapshot_t *s, const char *path,
                    uint32_t event_mask, uint32_t cookie)
{
    if (s == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (event_mask & (IN_DELETE | IN_DELETE_SELF)) {
        return handle_delete(s, path);
    }
    if (event_mask & IN_MOVED_FROM) {
        return handle_move_from(s, cookie, path);
    }
    if (event_mask & IN_MOVED_TO) {
        /* Best-effort cookie resolution for the audit log; the
         * destination still needs a content capture below regardless
         * of whether the cookie matched. */
        handle_move_to(s, cookie, path);
    }
    if (event_mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) {
        return capture_file(s, path, event_mask, 0, NULL);
    }
    return 0;
}

int snapshot_force(rnt_snapshot_t *s, const char *path,
                   uint8_t out_sha[RNT_SHA_LEN])
{
    if (s == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }
    return capture_file(s, path, IN_CLOSE_WRITE, RNT_FLAG_PRESNAPSHOT,
                        out_sha);
}

/* ------------------------------------------------------------------ */
/* rescan (for IN_Q_OVERFLOW recovery and `--rescan`)                 */
/* ------------------------------------------------------------------ */

static int rescan_visit(const char *path, const struct stat *st,
                        int typeflag, struct FTW *ftwbuf)
{
    (void)ftwbuf;
    if (g_rescan_target == NULL) return 0;
    if (typeflag != FTW_F) return 0;
    if (!S_ISREG(st->st_mode)) return 0;

    /* RNT_FLAG_SYNTHETIC marks captures that came from a rescan
     * rather than a live event — useful for ops to spot bursts of
     * post-overflow recovery in the index. */
    (void)capture_file(g_rescan_target, path,
                       IN_CLOSE_WRITE, RNT_FLAG_SYNTHETIC, NULL);
    return 0;          /* keep walking even if a single file failed */
}

int snapshot_rescan(rnt_snapshot_t *s, const char *root)
{
    if (s == NULL || root == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (g_rescan_target != NULL) {
        log_warn("snapshot: rescan of %s requested while another is "
                 "already running; refused", root);
        errno = EBUSY;
        return -1;
    }

    log_info("snapshot: rescan starting under %s", root);
    g_rescan_target = s;
    /* FTW_PHYS = use lstat (do not follow symlinks; safety #1).
     * FTW_MOUNT = stay within the same filesystem (a bind-mount or
     * a network mount under the watched root would otherwise drag
     * us off the device). */
    int rc = nftw(root, rescan_visit, 32, FTW_PHYS | FTW_MOUNT);
    g_rescan_target = NULL;
    if (rc < 0) {
        log_error("snapshot: rescan of %s: %s", root, strerror(errno));
        return -1;
    }
    log_info("snapshot: rescan of %s complete", root);
    return 0;
}
