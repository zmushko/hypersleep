/*
 * store.c — content-addressable storage on disk.
 *
 * Layout: <root>/<sha[0:2]>/<sha[2:]>
 *   - the first byte of the SHA-256 (two hex chars) is the shard directory;
 *     256 shards keep any one directory's fanout manageable.
 *   - the remaining 31 bytes (62 hex chars) are the blob filename.
 *
 * store_put pipeline (atomic, fsync-safe):
 *   1. open the source O_RDONLY|O_NOFOLLOW (safety property #1 —
 *      never dereference a symlink at the leaf).
 *   2. mkstemp a "<root>/.rnt-tmp-XXXXXX" file and stream the source
 *      into it while computing SHA-256.
 *   3. mkdir the shard directory under <root> if it does not exist.
 *   4. If <shard>/<rest> already exists in the CAS, unlink the temp
 *      and return — same content was captured earlier; the blob is
 *      content-addressed, so a re-write would be a no-op.
 *   5. fsync(temp_fd), renameat into the shard, fsync(shard_dir_fd).
 *      Power-loss after step 5 leaves either no blob (temp pruned on
 *      next start) or a fully durable one.
 *
 * Compression: deferred to v1.1 per the project brief. store_put
 * always writes raw bytes; out_flags is set to 0. The schema reserves
 * HS_FLAG_COMPRESSED for when zstd-framed blobs land.
 *
 * Concurrency: single writer (the daemon's snapshot pipeline);
 * multiple concurrent readers via store_open_blob / store_has.
 */

#include "store.h"
#include "log.h"
#include "hypersleep.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define COPY_BUFSZ        (64 * 1024)
#define SHA_HEX_LEN       (HS_SHA_LEN * 2)   /* 64 */
#define SHARD_HEX_LEN     2
#define REST_HEX_LEN      (SHA_HEX_LEN - SHARD_HEX_LEN)   /* 62 */

struct hs_store {
    char    *root_path;
    int      root_fd;        /* O_DIRECTORY|O_RDONLY; used for fsync of root */
    unsigned blob_mode;
};

static const char g_hex[] = "0123456789abcdef";

static void sha_to_hex(const uint8_t sha[HS_SHA_LEN], char out[SHA_HEX_LEN + 1])
{
    for (int i = 0; i < HS_SHA_LEN; i++) {
        out[i * 2]     = g_hex[(sha[i] >> 4) & 0xf];
        out[i * 2 + 1] = g_hex[ sha[i]       & 0xf];
    }
    out[SHA_HEX_LEN] = '\0';
}

static int hex_pair_to_byte(char hi, char lo, uint8_t *out)
{
    int h = -1, l = -1;
    if (hi >= '0' && hi <= '9') h = hi - '0';
    else if (hi >= 'a' && hi <= 'f') h = 10 + (hi - 'a');
    if (lo >= '0' && lo <= '9') l = lo - '0';
    else if (lo >= 'a' && lo <= 'f') l = 10 + (lo - 'a');
    if (h < 0 || l < 0) return -1;
    *out = (uint8_t)((h << 4) | l);
    return 0;
}

static int hex_to_sha(const char *hex, uint8_t out[HS_SHA_LEN])
{
    if (strlen(hex) != SHA_HEX_LEN) return -1;
    for (int i = 0; i < HS_SHA_LEN; i++) {
        if (hex_pair_to_byte(hex[i * 2], hex[i * 2 + 1], &out[i]) < 0) {
            return -1;
        }
    }
    return 0;
}

/* Build <root>/<shard>/<rest> into `out` (buffer of at least
 * strlen(root) + 1 + SHARD + 1 + REST + 1). Returns the offset of
 * the shard component for the caller's convenience. */
static size_t blob_path(const hs_store_t *s,
                       const uint8_t sha[HS_SHA_LEN],
                       char *out, size_t cap)
{
    char hex[SHA_HEX_LEN + 1];
    sha_to_hex(sha, hex);
    int n = snprintf(out, cap, "%s/%c%c/%s",
                     s->root_path, hex[0], hex[1], hex + SHARD_HEX_LEN);
    if (n < 0 || (size_t)n >= cap) return 0;
    return strlen(s->root_path) + 1;
}

static size_t shard_path(const hs_store_t *s,
                        const uint8_t sha[HS_SHA_LEN],
                        char *out, size_t cap)
{
    char hex[SHA_HEX_LEN + 1];
    sha_to_hex(sha, hex);
    int n = snprintf(out, cap, "%s/%c%c", s->root_path, hex[0], hex[1]);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

/* ------------------------------------------------------------------ */
/* open / close                                                       */
/* ------------------------------------------------------------------ */

hs_store_t *store_open(const char *path)
{
    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (mkdir(path, 0750) < 0 && errno != EEXIST) {
        log_error("store: cannot create %s: %s", path, strerror(errno));
        return NULL;
    }

    hs_store_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        log_error("store: out of memory");
        return NULL;
    }
    s->root_path = strdup(path);
    if (s->root_path == NULL) {
        free(s);
        return NULL;
    }
    s->root_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (s->root_fd < 0) {
        log_error("store: cannot open %s: %s", path, strerror(errno));
        free(s->root_path);
        free(s);
        return NULL;
    }
    s->blob_mode = 0600;
    return s;
}

void store_close(hs_store_t *s)
{
    if (s == NULL) return;
    if (s->root_fd >= 0) close(s->root_fd);
    free(s->root_path);
    free(s);
}

/* ------------------------------------------------------------------ */
/* store_put                                                          */
/* ------------------------------------------------------------------ */

/* fsync until EINTR is gone or it fails for another reason. */
static int fsync_retry(int fd)
{
    for (;;) {
        if (fsync(fd) == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

int store_put(hs_store_t *s,
              const char *src_path,
              uint8_t out_sha[HS_SHA_LEN],
              uint16_t *out_flags)
{
    if (s == NULL || src_path == NULL || out_sha == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (out_flags) *out_flags = 0;

    int src_fd = open(src_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (src_fd < 0) {
        log_error("store: open %s: %s", src_path, strerror(errno));
        return -1;
    }

    size_t rlen = strlen(s->root_path);
    char *tmp_path = malloc(rlen + 1 + sizeof(".rnt-tmp-XXXXXX"));
    if (tmp_path == NULL) {
        close(src_fd);
        return -1;
    }
    sprintf(tmp_path, "%s/.rnt-tmp-XXXXXX", s->root_path);

    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        log_error("store: mkstemp in %s: %s", s->root_path, strerror(errno));
        free(tmp_path);
        close(src_fd);
        return -1;
    }
    if (fchmod(tmp_fd, (mode_t)s->blob_mode) < 0) {
        log_warn("store: fchmod %s: %s", tmp_path, strerror(errno));
        /* not fatal — the temp may still be usable */
    }

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (md == NULL || EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1) {
        log_error("store: EVP_DigestInit_ex failed");
        if (md) EVP_MD_CTX_free(md);
        close(tmp_fd);
        unlink(tmp_path);
        free(tmp_path);
        close(src_fd);
        return -1;
    }

    /* Stream src -> tmp, hash on the way through. */
    uint8_t buf[COPY_BUFSZ];
    for (;;) {
        ssize_t n = read(src_fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("store: read %s: %s", src_path, strerror(errno));
            goto err;
        }
        if (EVP_DigestUpdate(md, buf, (size_t)n) != 1) {
            log_error("store: EVP_DigestUpdate failed");
            goto err;
        }
        size_t w = 0;
        while (w < (size_t)n) {
            ssize_t wn = write(tmp_fd, buf + w, (size_t)n - w);
            if (wn < 0) {
                if (errno == EINTR) continue;
                log_error("store: write %s: %s", tmp_path, strerror(errno));
                goto err;
            }
            w += (size_t)wn;
        }
    }
    close(src_fd);
    src_fd = -1;

    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(md, out_sha, &digest_len) != 1
        || digest_len != HS_SHA_LEN)
    {
        log_error("store: EVP_DigestFinal_ex produced %u bytes (want %d)",
                  digest_len, HS_SHA_LEN);
        goto err;
    }
    EVP_MD_CTX_free(md);
    md = NULL;

    /* Now we know the SHA. Materialise the target path. */
    size_t pathcap = rlen + 1 + SHARD_HEX_LEN + 1 + REST_HEX_LEN + 1;
    char *shardp = malloc(pathcap);
    char *finalp = malloc(pathcap);
    if (shardp == NULL || finalp == NULL) {
        free(shardp); free(finalp);
        goto err_nounlink;
    }
    if (shard_path(s, out_sha, shardp, pathcap) == 0
        || blob_path(s, out_sha, finalp, pathcap) == 0)
    {
        free(shardp); free(finalp);
        errno = ENAMETOOLONG;
        goto err_nounlink;
    }

    /* Idempotent: if the blob is already there, drop the temp. */
    struct stat st;
    if (lstat(finalp, &st) == 0) {
        unlink(tmp_path);
        close(tmp_fd);
        free(shardp);
        free(finalp);
        free(tmp_path);
        return 0;
    }

    /* Ensure the shard dir exists. */
    if (mkdir(shardp, 0750) < 0 && errno != EEXIST) {
        log_error("store: mkdir %s: %s", shardp, strerror(errno));
        free(shardp); free(finalp);
        goto err_nounlink;
    }

    /* Durability: data, then metadata. */
    if (fsync_retry(tmp_fd) < 0) {
        log_error("store: fsync %s: %s", tmp_path, strerror(errno));
        free(shardp); free(finalp);
        goto err_nounlink;
    }
    close(tmp_fd);
    tmp_fd = -1;

    if (rename(tmp_path, finalp) < 0) {
        log_error("store: rename %s -> %s: %s",
                  tmp_path, finalp, strerror(errno));
        free(shardp); free(finalp);
        goto err_nounlink;
    }

    /* Reopen the shard dir to fsync its directory entry. */
    int shard_fd = open(shardp, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (shard_fd >= 0) {
        fsync_retry(shard_fd);   /* best-effort */
        close(shard_fd);
    } else {
        log_warn("store: cannot reopen shard %s for fsync: %s",
                 shardp, strerror(errno));
    }

    free(shardp);
    free(finalp);
    free(tmp_path);
    return 0;

err:
    if (md) EVP_MD_CTX_free(md);
err_nounlink:
    if (tmp_fd >= 0) close(tmp_fd);
    unlink(tmp_path);
    if (src_fd >= 0) close(src_fd);
    free(tmp_path);
    return -1;
}

/* ------------------------------------------------------------------ */
/* readers                                                            */
/* ------------------------------------------------------------------ */

int store_open_blob(hs_store_t *s, const uint8_t sha[HS_SHA_LEN])
{
    if (s == NULL || sha == NULL) {
        errno = EINVAL;
        return -1;
    }
    char path[PATH_MAX];
    if (blob_path(s, sha, path, sizeof(path)) == 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        log_error("store: open blob %s: %s", path, strerror(errno));
    }
    return fd;
}

bool store_has(hs_store_t *s, const uint8_t sha[HS_SHA_LEN])
{
    if (s == NULL || sha == NULL) return false;
    char path[PATH_MAX];
    if (blob_path(s, sha, path, sizeof(path)) == 0) return false;
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int store_remove(hs_store_t *s, const uint8_t sha[HS_SHA_LEN])
{
    if (s == NULL || sha == NULL) {
        errno = EINVAL;
        return -1;
    }
    char path[PATH_MAX];
    if (blob_path(s, sha, path, sizeof(path)) == 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (unlink(path) < 0) {
        if (errno == ENOENT) return 0;     /* idempotent GC */
        log_error("store: unlink %s: %s", path, strerror(errno));
        return -1;
    }
    /* Shard dir is intentionally left in place even when empty —
     * cheap to keep, costly to race against a concurrent put that
     * just decided it could use this shard. */
    return 0;
}

/* ------------------------------------------------------------------ */
/* iterate / verify                                                   */
/* ------------------------------------------------------------------ */

static bool is_hex_lower(const char *s, size_t want)
{
    if (strlen(s) != want) return false;
    for (size_t i = 0; i < want; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

int store_iterate(hs_store_t *s, hs_store_iter_fn fn, void *user)
{
    if (s == NULL || fn == NULL) {
        errno = EINVAL;
        return -1;
    }
    DIR *root = opendir(s->root_path);
    if (root == NULL) {
        log_error("store: opendir %s: %s", s->root_path, strerror(errno));
        return -1;
    }

    int abort_value = 0;
    struct dirent *e;
    while ((e = readdir(root)) != NULL) {
        if (!is_hex_lower(e->d_name, SHARD_HEX_LEN)) continue;

        size_t need = strlen(s->root_path) + 1 + SHARD_HEX_LEN + 1;
        char *shardp = malloc(need);
        if (shardp == NULL) { abort_value = -1; break; }
        snprintf(shardp, need, "%s/%s", s->root_path, e->d_name);

        DIR *shard = opendir(shardp);
        if (shard == NULL) {
            /* EACCES is the typical "you didn't run me as the user
             * that wrote the store" — happens dozens of times in
             * a row and is not actionable from a status query.
             * Drop it to debug so non-root status calls stay
             * quiet. Other errors still warn. */
            if (errno == EACCES) {
                log_debug("store: opendir %s: %s", shardp, strerror(errno));
            } else {
                log_warn("store: opendir %s: %s", shardp, strerror(errno));
            }
            free(shardp);
            continue;
        }
        struct dirent *be;
        while ((be = readdir(shard)) != NULL) {
            if (!is_hex_lower(be->d_name, REST_HEX_LEN)) continue;

            char full_hex[SHA_HEX_LEN + 1];
            full_hex[0] = e->d_name[0];
            full_hex[1] = e->d_name[1];
            memcpy(full_hex + 2, be->d_name, REST_HEX_LEN);
            full_hex[SHA_HEX_LEN] = '\0';

            uint8_t sha[HS_SHA_LEN];
            if (hex_to_sha(full_hex, sha) < 0) continue;

            char blobp[PATH_MAX];
            int n = snprintf(blobp, sizeof(blobp), "%s/%s", shardp, be->d_name);
            if (n < 0 || (size_t)n >= sizeof(blobp)) continue;

            struct stat st;
            if (lstat(blobp, &st) < 0) continue;
            if (!S_ISREG(st.st_mode)) continue;

            int rc = fn(sha, st.st_size, user);
            if (rc != 0) { abort_value = rc; break; }
        }
        closedir(shard);
        free(shardp);
        if (abort_value != 0) break;
    }
    closedir(root);
    return abort_value;
}

int store_verify_blob(hs_store_t *s, const uint8_t sha[HS_SHA_LEN])
{
    if (s == NULL || sha == NULL) {
        errno = EINVAL;
        return -1;
    }
    int fd = store_open_blob(s, sha);
    if (fd < 0) return -1;

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (md == NULL || EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1) {
        if (md) EVP_MD_CTX_free(md);
        close(fd);
        return -1;
    }
    uint8_t buf[COPY_BUFSZ];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            EVP_MD_CTX_free(md);
            close(fd);
            return -1;
        }
        EVP_DigestUpdate(md, buf, (size_t)n);
    }
    close(fd);

    uint8_t got[HS_SHA_LEN];
    unsigned int got_len = 0;
    if (EVP_DigestFinal_ex(md, got, &got_len) != 1 || got_len != HS_SHA_LEN) {
        EVP_MD_CTX_free(md);
        return -1;
    }
    EVP_MD_CTX_free(md);
    return memcmp(got, sha, HS_SHA_LEN) == 0 ? 0 : 1;
}
