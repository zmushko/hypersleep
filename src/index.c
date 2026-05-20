/*
 * index.c — LMDB-backed index over captured file versions.
 *
 * Five named sub-databases (see docs/architecture.md):
 *
 *   files       key  = <path-bytes> 0x00 <be64 captured_ts> <be32 seq>
 *               val  = hs_file_entry (packed)
 *   by_sha      key  = sha256[32]
 *               val  = uint32_t refcount
 *   deletions   key  = <path-bytes> 0x00 <be64 deleted_ts>
 *               val  = hs_deletion (packed)
 *   moves       key  = <be32 cookie>
 *               val  = <be64 ts_ns> <from_path null-terminated>
 *   meta        key  = ASCII string ("schema_version", ...)
 *               val  = caller-defined; we use packed integers
 *
 * Key encoding rationale: big-endian timestamps make lexicographic
 * order on the raw key match chronological order, so a cursor seek
 * to the path prefix walks the versions oldest first. The 0x00
 * separator delimits the variable-length path from the fixed-width
 * tail; paths cannot contain NUL on POSIX so the separator is
 * unambiguous.
 *
 * Path length cap: LMDB's default max key is 511 bytes, of which we
 * spend 13 (separator + ts + seq). The remaining bytes are plenty
 * for typical filesystems; very deep paths are rejected at insert
 * time with a clear errno.
 *
 * Concurrency: single writer, many readers. Each public function
 * opens its own transaction (write or read-only as appropriate);
 * cursor iteration holds a read txn open until cursor_close.
 */

#include "index.h"
#include "log.h"
#include "hypersleep.h"

#include <errno.h>
#include <inttypes.h>
#include <lmdb.h>

/* struct stat carries the nanosecond mtime under different names on
 * different libcs. Linux glibc/musl: st_mtim. macOS: st_mtimespec.
 * Hypersleep is Linux-only at runtime, but the shim keeps local
 * compile-checks portable. */
#if defined(__APPLE__) && !defined(st_mtim)
# define st_mtim st_mtimespec
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define INDEX_MAP_SIZE  (1ULL << 32)   /* 4 GiB virtual reservation */
#define INDEX_MAX_DBIS  16
#define INDEX_PATH_MAX  498            /* 511 - 13 (sep + ts + seq) */

struct hs_index {
    MDB_env *env;
    MDB_dbi  files;
    MDB_dbi  by_sha;
    MDB_dbi  deletions;
    MDB_dbi  moves;
    MDB_dbi  meta;
    bool     readonly;
};

struct hs_index_cursor {
    hs_index_t *idx;
    MDB_txn     *txn;
    MDB_cursor  *cursor;
    uint8_t     *prefix;       /* path-bytes + 0x00 separator */
    size_t       prefix_len;
    bool         started;
    bool         exhausted;
};

/* ------------------------------------------------------------------ */
/* tiny helpers                                                       */
/* ------------------------------------------------------------------ */

static void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)(v & 0xff); v >>= 8; }
}
static void put_be32(uint8_t *p, uint32_t v)
{
    for (int i = 3; i >= 0; i--) { p[i] = (uint8_t)(v & 0xff); v >>= 8; }
}
static uint64_t get_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static int build_files_key(uint8_t *buf, size_t cap, size_t *out_len,
                           const char *path, uint64_t ts, uint32_t seq)
{
    size_t plen = strlen(path);
    if (plen > INDEX_PATH_MAX) {
        log_error("index: path too long for key (%zu bytes, max %d): %s",
                  plen, INDEX_PATH_MAX, path);
        errno = ENAMETOOLONG;
        return -1;
    }
    if (plen + 13 > cap) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(buf, path, plen);
    buf[plen] = 0x00;
    put_be64(buf + plen + 1, ts);
    put_be32(buf + plen + 9, seq);
    *out_len = plen + 13;
    return 0;
}

static int build_deletions_key(uint8_t *buf, size_t cap, size_t *out_len,
                               const char *path, uint64_t ts)
{
    size_t plen = strlen(path);
    if (plen > INDEX_PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (plen + 9 > cap) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(buf, path, plen);
    buf[plen] = 0x00;
    put_be64(buf + plen + 1, ts);
    *out_len = plen + 9;
    return 0;
}

static int build_path_prefix(uint8_t *buf, size_t cap, size_t *out_len,
                             const char *path)
{
    size_t plen = strlen(path);
    if (plen + 1 > cap) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(buf, path, plen);
    buf[plen] = 0x00;
    *out_len = plen + 1;
    return 0;
}

static int lmdb_fail(int rc, const char *what)
{
    log_error("index: %s: %s", what, mdb_strerror(rc));
    return -1;
}

/* ------------------------------------------------------------------ */
/* env management                                                     */
/* ------------------------------------------------------------------ */

hs_index_t *index_open(const char *path, enum hs_index_mode mode)
{
    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    /* Ensure the directory exists. Mode bits are advisory; the
     * daemon's umask owns the final permission anyway. */
    if (mkdir(path, 0750) < 0 && errno != EEXIST) {
        log_error("index: cannot create %s: %s", path, strerror(errno));
        return NULL;
    }

    hs_index_t *idx = calloc(1, sizeof(*idx));
    if (idx == NULL) {
        log_error("index: out of memory");
        return NULL;
    }
    idx->readonly = (mode == HS_IDX_READ);

    int rc = mdb_env_create(&idx->env);
    if (rc != 0) { lmdb_fail(rc, "mdb_env_create"); goto err; }

    rc = mdb_env_set_maxdbs(idx->env, INDEX_MAX_DBIS);
    if (rc != 0) { lmdb_fail(rc, "mdb_env_set_maxdbs"); goto err; }

    rc = mdb_env_set_mapsize(idx->env, INDEX_MAP_SIZE);
    if (rc != 0) { lmdb_fail(rc, "mdb_env_set_mapsize"); goto err; }

    unsigned env_flags = MDB_NOTLS;
    if (idx->readonly) env_flags |= MDB_RDONLY;
    /* 0644 (not 0660) so a hypersleep CLI run by any local user
     * can open the LMDB env read-only after the daemon has created
     * data.mdb / lock.mdb. Restrictive prod setups put
     * /var/lib/hypersleep itself behind 2750 + group ownership;
     * the per-file mode matters less than the parent directory's
     * traversal bits. */
    rc = mdb_env_open(idx->env, path, env_flags, 0644);
    if (rc != 0) { lmdb_fail(rc, "mdb_env_open"); goto err; }

    /* Open all sub-DBs in one txn so the DBI handles are valid for
     * the rest of the env's life. */
    MDB_txn *txn = NULL;
    rc = mdb_txn_begin(idx->env, NULL,
                       idx->readonly ? MDB_RDONLY : 0, &txn);
    if (rc != 0) { lmdb_fail(rc, "mdb_txn_begin (open dbs)"); goto err; }

    unsigned dbi_flags = idx->readonly ? 0 : MDB_CREATE;
    if (mdb_dbi_open(txn, "files",     dbi_flags, &idx->files)     != 0 ||
        mdb_dbi_open(txn, "by_sha",    dbi_flags, &idx->by_sha)    != 0 ||
        mdb_dbi_open(txn, "deletions", dbi_flags, &idx->deletions) != 0 ||
        mdb_dbi_open(txn, "moves",     dbi_flags, &idx->moves)     != 0 ||
        mdb_dbi_open(txn, "meta",      dbi_flags, &idx->meta)      != 0)
    {
        mdb_txn_abort(txn);
        log_error("index: failed to open one of the sub-databases");
        goto err;
    }
    rc = mdb_txn_commit(txn);
    if (rc != 0) { lmdb_fail(rc, "mdb_txn_commit (open dbs)"); goto err; }

    if (index_schema_check(idx) < 0) goto err;
    return idx;

err:
    if (idx) {
        if (idx->env) mdb_env_close(idx->env);
        free(idx);
    }
    return NULL;
}

void index_close(hs_index_t *idx)
{
    if (idx == NULL) return;
    if (idx->env) mdb_env_close(idx->env);
    free(idx);
}

int index_schema_check(hs_index_t *idx)
{
    if (idx == NULL) {
        errno = EINVAL;
        return -1;
    }
    MDB_txn *txn = NULL;
    int rc = mdb_txn_begin(idx->env, NULL,
                           idx->readonly ? MDB_RDONLY : 0, &txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_begin (schema)");

    MDB_val key = { .mv_size = strlen("schema_version"),
                    .mv_data = (void *)"schema_version" };
    MDB_val val = {0};

    rc = mdb_get(txn, idx->meta, &key, &val);
    if (rc == MDB_NOTFOUND) {
        if (idx->readonly) {
            mdb_txn_abort(txn);
            log_error("index: schema_version missing in read-only env "
                      "(uninitialised index?)");
            return -1;
        }
        uint32_t v = HYPERSLEEP_SCHEMA_VERSION;
        MDB_val sv = { .mv_size = sizeof(v), .mv_data = &v };
        rc = mdb_put(txn, idx->meta, &key, &sv, 0);
        if (rc != 0) { mdb_txn_abort(txn); return lmdb_fail(rc, "mdb_put schema"); }
        rc = mdb_txn_commit(txn);
        if (rc != 0) return lmdb_fail(rc, "mdb_txn_commit (init schema)");
        log_info("index: initialised at schema version %u",
                 HYPERSLEEP_SCHEMA_VERSION);
        return 0;
    }
    if (rc != 0) {
        mdb_txn_abort(txn);
        return lmdb_fail(rc, "mdb_get schema");
    }
    if (val.mv_size != sizeof(uint32_t)) {
        mdb_txn_abort(txn);
        log_error("index: schema_version has unexpected size %zu", val.mv_size);
        return -1;
    }
    uint32_t found;
    memcpy(&found, val.mv_data, sizeof(found));
    mdb_txn_abort(txn);

    if (found != HYPERSLEEP_SCHEMA_VERSION) {
        log_error("index: schema version mismatch: on-disk %u, code %u "
                  "(migration not yet implemented)",
                  found, HYPERSLEEP_SCHEMA_VERSION);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* by_sha refcount helpers                                            */
/* ------------------------------------------------------------------ */

static int by_sha_get(MDB_txn *txn, MDB_dbi dbi,
                      const uint8_t sha[HS_SHA_LEN], uint32_t *out)
{
    MDB_val k = { .mv_size = HS_SHA_LEN, .mv_data = (void *)sha };
    MDB_val v = {0};
    int rc = mdb_get(txn, dbi, &k, &v);
    if (rc == MDB_NOTFOUND) { *out = 0; return 0; }
    if (rc != 0) return rc;
    if (v.mv_size != sizeof(uint32_t)) return MDB_BAD_VALSIZE;
    memcpy(out, v.mv_data, sizeof(uint32_t));
    return 0;
}

static int by_sha_put(MDB_txn *txn, MDB_dbi dbi,
                      const uint8_t sha[HS_SHA_LEN], uint32_t val)
{
    MDB_val k = { .mv_size = HS_SHA_LEN, .mv_data = (void *)sha };
    MDB_val v = { .mv_size = sizeof(val), .mv_data = &val };
    return mdb_put(txn, dbi, &k, &v, 0);
}

static int by_sha_del(MDB_txn *txn, MDB_dbi dbi,
                      const uint8_t sha[HS_SHA_LEN])
{
    MDB_val k = { .mv_size = HS_SHA_LEN, .mv_data = (void *)sha };
    return mdb_del(txn, dbi, &k, NULL);
}

/* ------------------------------------------------------------------ */
/* files: insert / lookup / iterate                                   */
/* ------------------------------------------------------------------ */

/* Pick the next free `seq` for (path, ts). With a ns-resolution
 * clock and a single-threaded writer collisions are theoretical,
 * but the schema reserves the field for safety. */
static int next_seq_for(MDB_txn *txn, MDB_dbi dbi,
                        const char *path, uint64_t ts,
                        uint32_t *out_seq)
{
    uint8_t buf[INDEX_PATH_MAX + 13];
    for (uint32_t seq = 0; seq < 1024; seq++) {
        size_t klen = 0;
        if (build_files_key(buf, sizeof(buf), &klen, path, ts, seq) < 0) {
            return -1;
        }
        MDB_val k = { .mv_size = klen, .mv_data = buf };
        MDB_val v = {0};
        int rc = mdb_get(txn, dbi, &k, &v);
        if (rc == MDB_NOTFOUND) { *out_seq = seq; return 0; }
        if (rc != 0) return -1;
    }
    log_error("index: 1024 collisions on (path, ts) — bug?");
    errno = EAGAIN;
    return -1;
}

int index_insert(hs_index_t *idx, const char *path,
                 const uint8_t sha[HS_SHA_LEN],
                 const struct stat *st,
                 uint32_t event_mask, uint64_t captured_ns,
                 uint16_t flags)
{
    if (idx == NULL || path == NULL || sha == NULL || st == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (idx->readonly) {
        errno = EROFS;
        return -1;
    }

    /* Caller usually has a (mtime, size) pre-check, but the
     * (path, sha) duplicate check is the canonical idempotency
     * gate. */
    if (index_has_path_sha(idx, path, sha)) {
        return 1;
    }

    MDB_txn *txn = NULL;
    int rc = mdb_txn_begin(idx->env, NULL, 0, &txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_begin (insert)");

    uint32_t seq = 0;
    if (next_seq_for(txn, idx->files, path, captured_ns, &seq) < 0) {
        mdb_txn_abort(txn);
        return -1;
    }

    uint8_t keybuf[INDEX_PATH_MAX + 13];
    size_t  klen = 0;
    if (build_files_key(keybuf, sizeof(keybuf), &klen,
                        path, captured_ns, seq) < 0) {
        mdb_txn_abort(txn);
        return -1;
    }

    hs_file_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.sha256, sha, HS_SHA_LEN);
    entry.size       = (uint64_t)st->st_size;
    entry.mtime_sec  = (int64_t)st->st_mtim.tv_sec;
    entry.mtime_nsec = (int32_t)st->st_mtim.tv_nsec;
    entry.mode       = (uint32_t)st->st_mode;
    entry.uid        = (uint32_t)st->st_uid;
    entry.gid        = (uint32_t)st->st_gid;
    entry.event_mask = event_mask;
    entry.flags      = flags;

    MDB_val k = { .mv_size = klen,           .mv_data = keybuf };
    MDB_val v = { .mv_size = sizeof(entry),  .mv_data = &entry };
    rc = mdb_put(txn, idx->files, &k, &v, MDB_NOOVERWRITE);
    if (rc != 0) {
        mdb_txn_abort(txn);
        return lmdb_fail(rc, "mdb_put files");
    }

    uint32_t refs = 0;
    rc = by_sha_get(txn, idx->by_sha, sha, &refs);
    if (rc != 0) { mdb_txn_abort(txn); return lmdb_fail(rc, "by_sha_get"); }
    rc = by_sha_put(txn, idx->by_sha, sha, refs + 1);
    if (rc != 0) { mdb_txn_abort(txn); return lmdb_fail(rc, "by_sha_put"); }

    rc = mdb_txn_commit(txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_commit (insert)");
    return 0;
}

int index_get_latest(hs_index_t *idx, const char *path, hs_version_t *out)
{
    if (idx == NULL || path == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    uint8_t prefix[INDEX_PATH_MAX + 1];
    size_t  plen = 0;
    if (build_path_prefix(prefix, sizeof(prefix), &plen, path) < 0) {
        return -1;
    }

    MDB_txn *txn = NULL;
    int rc = mdb_txn_begin(idx->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_begin (latest)");
    MDB_cursor *c = NULL;
    rc = mdb_cursor_open(txn, idx->files, &c);
    if (rc != 0) { mdb_txn_abort(txn); return lmdb_fail(rc, "mdb_cursor_open (latest)"); }

    /* Strategy: seek to (path, UINT64_MAX, UINT32_MAX) with SET_RANGE,
     * then step back one. SET_RANGE may overshoot into the next path
     * range (or miss the DB end entirely); we handle both. */
    uint8_t hikey[INDEX_PATH_MAX + 13];
    size_t  hilen = 0;
    if (build_files_key(hikey, sizeof(hikey), &hilen,
                        path, UINT64_MAX, UINT32_MAX) < 0) {
        mdb_cursor_close(c);
        mdb_txn_abort(txn);
        return -1;
    }

    MDB_val k = { .mv_size = hilen, .mv_data = hikey };
    MDB_val v = {0};
    rc = mdb_cursor_get(c, &k, &v, MDB_SET_RANGE);
    int found = 0;

    if (rc == MDB_NOTFOUND) {
        rc = mdb_cursor_get(c, &k, &v, MDB_LAST);
        if (rc == 0) found = 1;
    } else if (rc == 0) {
        rc = mdb_cursor_get(c, &k, &v, MDB_PREV);
        if (rc == 0) found = 1;
    }

    int result = -1;
    if (found
        && k.mv_size >= plen
        && memcmp(k.mv_data, prefix, plen) == 0
        && v.mv_size == sizeof(hs_file_entry_t))
    {
        const uint8_t *kb = k.mv_data;
        memcpy(&out->entry, v.mv_data, sizeof(out->entry));
        out->captured_ns = get_be64(kb + plen);
        memcpy(out->sha256, out->entry.sha256, HS_SHA_LEN);
        out->num = 0;
        result = 0;
    } else {
        errno = ENOENT;
    }

    mdb_cursor_close(c);
    mdb_txn_abort(txn);
    return result;
}

bool index_has_path_sha(hs_index_t *idx, const char *path,
                        const uint8_t sha[HS_SHA_LEN])
{
    if (idx == NULL || path == NULL || sha == NULL) return false;

    uint8_t prefix[INDEX_PATH_MAX + 1];
    size_t  plen = 0;
    if (build_path_prefix(prefix, sizeof(prefix), &plen, path) < 0) {
        return false;
    }

    MDB_txn *txn = NULL;
    if (mdb_txn_begin(idx->env, NULL, MDB_RDONLY, &txn) != 0) return false;
    MDB_cursor *c = NULL;
    if (mdb_cursor_open(txn, idx->files, &c) != 0) {
        mdb_txn_abort(txn);
        return false;
    }

    MDB_val k = { .mv_size = plen, .mv_data = prefix };
    MDB_val v = {0};
    int rc = mdb_cursor_get(c, &k, &v, MDB_SET_RANGE);
    bool found = false;
    while (rc == 0
           && k.mv_size >= plen
           && memcmp(k.mv_data, prefix, plen) == 0)
    {
        if (v.mv_size == sizeof(hs_file_entry_t)) {
            const hs_file_entry_t *e = v.mv_data;
            if (memcmp(e->sha256, sha, HS_SHA_LEN) == 0) {
                found = true;
                break;
            }
        }
        rc = mdb_cursor_get(c, &k, &v, MDB_NEXT);
    }
    mdb_cursor_close(c);
    mdb_txn_abort(txn);
    return found;
}

hs_index_cursor_t *index_iter_path(hs_index_t *idx, const char *path)
{
    if (idx == NULL || path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    hs_index_cursor_t *cur = calloc(1, sizeof(*cur));
    if (cur == NULL) return NULL;
    cur->idx = idx;

    size_t plen = strlen(path);
    cur->prefix = malloc(plen + 1);
    if (cur->prefix == NULL) {
        free(cur);
        return NULL;
    }
    memcpy(cur->prefix, path, plen);
    cur->prefix[plen] = 0x00;
    cur->prefix_len = plen + 1;

    int rc = mdb_txn_begin(idx->env, NULL, MDB_RDONLY, &cur->txn);
    if (rc != 0) {
        lmdb_fail(rc, "mdb_txn_begin (iter)");
        free(cur->prefix);
        free(cur);
        return NULL;
    }
    rc = mdb_cursor_open(cur->txn, idx->files, &cur->cursor);
    if (rc != 0) {
        lmdb_fail(rc, "mdb_cursor_open (iter)");
        mdb_txn_abort(cur->txn);
        free(cur->prefix);
        free(cur);
        return NULL;
    }
    return cur;
}

int index_cursor_next(hs_index_cursor_t *c, hs_version_t *out)
{
    if (c == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (c->exhausted) return 1;

    MDB_val k, v;
    int rc;
    if (!c->started) {
        k.mv_size = c->prefix_len;
        k.mv_data = c->prefix;
        rc = mdb_cursor_get(c->cursor, &k, &v, MDB_SET_RANGE);
        c->started = true;
    } else {
        rc = mdb_cursor_get(c->cursor, &k, &v, MDB_NEXT);
    }
    if (rc == MDB_NOTFOUND) { c->exhausted = true; return 1; }
    if (rc != 0) {
        c->exhausted = true;
        return lmdb_fail(rc, "mdb_cursor_get (iter next)");
    }
    if (k.mv_size < c->prefix_len
        || memcmp(k.mv_data, c->prefix, c->prefix_len) != 0)
    {
        c->exhausted = true;
        return 1;
    }
    if (v.mv_size != sizeof(hs_file_entry_t)) {
        c->exhausted = true;
        log_error("index: corrupt file_entry size %zu", v.mv_size);
        return -1;
    }
    const uint8_t *kb = k.mv_data;
    memcpy(&out->entry, v.mv_data, sizeof(out->entry));
    out->captured_ns = get_be64(kb + c->prefix_len);
    memcpy(out->sha256, out->entry.sha256, HS_SHA_LEN);
    out->num = 0;
    return 0;
}

void index_cursor_close(hs_index_cursor_t *c)
{
    if (c == NULL) return;
    if (c->cursor) mdb_cursor_close(c->cursor);
    if (c->txn)    mdb_txn_abort(c->txn);
    free(c->prefix);
    free(c);
}

/* ------------------------------------------------------------------ */
/* deletions                                                          */
/* ------------------------------------------------------------------ */

int index_record_deletion(hs_index_t *idx, const char *path,
                          uint64_t deleted_ns, const hs_deletion_t *del)
{
    if (idx == NULL || path == NULL || del == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (idx->readonly) {
        errno = EROFS;
        return -1;
    }
    uint8_t keybuf[INDEX_PATH_MAX + 9];
    size_t  klen = 0;
    if (build_deletions_key(keybuf, sizeof(keybuf), &klen,
                            path, deleted_ns) < 0) {
        return -1;
    }

    MDB_txn *txn = NULL;
    int rc = mdb_txn_begin(idx->env, NULL, 0, &txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_begin (deletion)");

    MDB_val k = { .mv_size = klen,         .mv_data = keybuf };
    MDB_val v = { .mv_size = sizeof(*del), .mv_data = (void *)del };
    rc = mdb_put(txn, idx->deletions, &k, &v, 0);
    if (rc != 0) { mdb_txn_abort(txn); return lmdb_fail(rc, "mdb_put deletions"); }
    rc = mdb_txn_commit(txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_commit (deletion)");
    return 0;
}

/* ------------------------------------------------------------------ */
/* moves: cookie -> { ts_ns, from_path }                              */
/* ------------------------------------------------------------------ */

int index_moves_pending(hs_index_t *idx, uint32_t cookie,
                        const char *from_path)
{
    if (idx == NULL || from_path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (idx->readonly) { errno = EROFS; return -1; }

    uint8_t ckbuf[4];
    put_be32(ckbuf, cookie);

    size_t  plen = strlen(from_path);
    size_t  vlen = 8 + plen + 1;
    uint8_t *vbuf = malloc(vlen);
    if (vbuf == NULL) return -1;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ts_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    put_be64(vbuf, ts_ns);
    memcpy(vbuf + 8, from_path, plen);
    vbuf[8 + plen] = '\0';

    MDB_txn *txn = NULL;
    int rc = mdb_txn_begin(idx->env, NULL, 0, &txn);
    if (rc != 0) { free(vbuf); return lmdb_fail(rc, "mdb_txn_begin (move)"); }

    MDB_val k = { .mv_size = sizeof(ckbuf), .mv_data = ckbuf };
    MDB_val v = { .mv_size = vlen,          .mv_data = vbuf  };
    rc = mdb_put(txn, idx->moves, &k, &v, 0);
    if (rc != 0) { mdb_txn_abort(txn); free(vbuf); return lmdb_fail(rc, "mdb_put moves"); }
    rc = mdb_txn_commit(txn);
    free(vbuf);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_commit (move)");
    return 0;
}

int index_moves_resolve(hs_index_t *idx, uint32_t cookie,
                        const char *to_path, char **out_from)
{
    if (idx == NULL || out_from == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (idx->readonly) { errno = EROFS; return -1; }
    (void)to_path;        /* future: log resolution for audit */

    uint8_t ckbuf[4];
    put_be32(ckbuf, cookie);
    *out_from = NULL;

    MDB_txn *txn = NULL;
    int rc = mdb_txn_begin(idx->env, NULL, 0, &txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_begin (resolve)");

    MDB_val k = { .mv_size = sizeof(ckbuf), .mv_data = ckbuf };
    MDB_val v = {0};
    rc = mdb_get(txn, idx->moves, &k, &v);
    if (rc == MDB_NOTFOUND) {
        mdb_txn_abort(txn);
        return 1;
    }
    if (rc != 0) { mdb_txn_abort(txn); return lmdb_fail(rc, "mdb_get moves"); }
    if (v.mv_size < 9) {
        mdb_txn_abort(txn);
        log_error("index: corrupt move record (size %zu)", v.mv_size);
        return -1;
    }

    const char *from = (const char *)v.mv_data + 8;
    size_t      flen = v.mv_size - 8;
    *out_from = malloc(flen);
    if (*out_from == NULL) { mdb_txn_abort(txn); return -1; }
    memcpy(*out_from, from, flen);
    (*out_from)[flen - 1] = '\0';

    rc = mdb_del(txn, idx->moves, &k, NULL);
    if (rc != 0) {
        mdb_txn_abort(txn);
        free(*out_from); *out_from = NULL;
        return lmdb_fail(rc, "mdb_del moves");
    }
    rc = mdb_txn_commit(txn);
    if (rc != 0) {
        free(*out_from); *out_from = NULL;
        return lmdb_fail(rc, "mdb_txn_commit (resolve)");
    }
    return 0;
}

int index_moves_gc(hs_index_t *idx, uint64_t older_than_ns)
{
    if (idx == NULL) { errno = EINVAL; return -1; }
    if (idx->readonly) { errno = EROFS; return -1; }

    MDB_txn *txn = NULL;
    int rc = mdb_txn_begin(idx->env, NULL, 0, &txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_begin (move gc)");
    MDB_cursor *c = NULL;
    rc = mdb_cursor_open(txn, idx->moves, &c);
    if (rc != 0) { mdb_txn_abort(txn); return lmdb_fail(rc, "mdb_cursor_open (move gc)"); }

    int removed = 0;
    MDB_val k = {0}, v = {0};
    rc = mdb_cursor_get(c, &k, &v, MDB_FIRST);
    while (rc == 0) {
        if (v.mv_size >= 8) {
            uint64_t ts = get_be64(v.mv_data);
            if (ts < older_than_ns) {
                int dr = mdb_cursor_del(c, 0);
                if (dr != 0) { lmdb_fail(dr, "mdb_cursor_del (move gc)"); break; }
                removed++;
            }
        }
        rc = mdb_cursor_get(c, &k, &v, MDB_NEXT);
    }
    mdb_cursor_close(c);
    if (rc != 0 && rc != MDB_NOTFOUND) {
        mdb_txn_abort(txn);
        return lmdb_fail(rc, "mdb_cursor_get (move gc)");
    }
    rc = mdb_txn_commit(txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_commit (move gc)");
    return removed;
}

/* ------------------------------------------------------------------ */
/* prune_by_age                                                       */
/*                                                                    */
/* Walk files, remove entries older than the cutoff, decrement       */
/* by_sha refcount. Returns 0/-1; *out_pruned receives the number of */
/* SHAs whose refcount reached zero — caller deletes those blobs.    */
/* ------------------------------------------------------------------ */

int index_sha_refcount(hs_index_t *idx,
                       const uint8_t sha[HS_SHA_LEN],
                       uint32_t *out)
{
    if (idx == NULL || sha == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    MDB_txn *txn = NULL;
    int rc = mdb_txn_begin(idx->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_begin (refcount)");
    uint32_t v = 0;
    rc = by_sha_get(txn, idx->by_sha, sha, &v);
    mdb_txn_abort(txn);
    if (rc != 0) return lmdb_fail(rc, "by_sha_get (refcount)");
    *out = v;
    return 0;
}

int index_prune_by_age(hs_index_t *idx, uint64_t older_than_ns,
                       size_t *out_pruned)
{
    if (idx == NULL) { errno = EINVAL; return -1; }
    if (idx->readonly) { errno = EROFS; return -1; }
    if (out_pruned) *out_pruned = 0;

    MDB_txn *txn = NULL;
    int rc = mdb_txn_begin(idx->env, NULL, 0, &txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_begin (prune)");
    MDB_cursor *c = NULL;
    rc = mdb_cursor_open(txn, idx->files, &c);
    if (rc != 0) { mdb_txn_abort(txn); return lmdb_fail(rc, "mdb_cursor_open (prune)"); }

    size_t blobs_freed = 0;
    MDB_val k = {0}, v = {0};
    rc = mdb_cursor_get(c, &k, &v, MDB_FIRST);
    while (rc == 0) {
        if (k.mv_size < 13 || v.mv_size != sizeof(hs_file_entry_t)) {
            rc = mdb_cursor_get(c, &k, &v, MDB_NEXT);
            continue;
        }
        /* timestamp is the 8 bytes after the path separator —
         * equivalently, the 12 bytes before the 4-byte seq tail. */
        const uint8_t *kb = k.mv_data;
        uint64_t ts = get_be64(kb + (k.mv_size - 12));
        if (ts >= older_than_ns) {
            rc = mdb_cursor_get(c, &k, &v, MDB_NEXT);
            continue;
        }
        const hs_file_entry_t *e = v.mv_data;
        uint8_t sha[HS_SHA_LEN];
        memcpy(sha, e->sha256, HS_SHA_LEN);

        int dr = mdb_cursor_del(c, 0);
        if (dr != 0) { rc = dr; break; }

        uint32_t refs = 0;
        int gr = by_sha_get(txn, idx->by_sha, sha, &refs);
        if (gr != 0) { rc = gr; break; }
        if (refs > 1) {
            int pr = by_sha_put(txn, idx->by_sha, sha, refs - 1);
            if (pr != 0) { rc = pr; break; }
        } else {
            int dr2 = by_sha_del(txn, idx->by_sha, sha);
            if (dr2 != 0 && dr2 != MDB_NOTFOUND) { rc = dr2; break; }
            blobs_freed++;
        }
        rc = mdb_cursor_get(c, &k, &v, MDB_NEXT);
    }
    mdb_cursor_close(c);
    if (rc != 0 && rc != MDB_NOTFOUND) {
        mdb_txn_abort(txn);
        return lmdb_fail(rc, "prune walk");
    }
    rc = mdb_txn_commit(txn);
    if (rc != 0) return lmdb_fail(rc, "mdb_txn_commit (prune)");
    if (out_pruned) *out_pruned = blobs_freed;
    return 0;
}
