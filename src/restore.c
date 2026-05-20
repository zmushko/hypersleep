/*
 * restore.c — CAS blob -> filesystem extraction.
 *
 * Atomic pipeline:
 *   1. open dst's parent directory with O_DIRECTORY (we need it as
 *      an fd for renameat + fsync).
 *   2. mkstemp ".rnt-restore-XXXXXX" inside that directory.
 *   3. fchmod/fchown/futimens the temp BEFORE renaming so the
 *      destination atomically gains its final identity.
 *   4. stream the blob into the temp, fsync the temp.
 *   5. renameat into dst (O_EXCL semantics enforced by the caller's
 *      prior existence check — we cannot use rename+EXCL atomically
 *      without renameat2 RENAME_NOREPLACE which is Linux 3.15+; we
 *      treat that as a v0.1.0+ hardening item).
 *   6. fsync the parent directory.
 */

#include "restore.h"
#include "log.h"
#include "hypersleep.h"
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define COPY_BUFSZ (64 * 1024)

static int fsync_retry(int fd)
{
    for (;;) {
        if (fsync(fd) == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

int restore_to(hs_store_t *store,
               const hs_version_t *v,
               const char *dst,
               bool preserve_mode)
{
    if (store == NULL || v == NULL || dst == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Reject pre-existing target up front. The atomic-rename below
     * cannot enforce O_EXCL across the link; the caller wants a
     * clear refusal rather than a clobber. */
    struct stat dst_st;
    if (lstat(dst, &dst_st) == 0) {
        errno = EEXIST;
        return -1;
    }

    /* dirname() may modify its input. Work on a copy. */
    char *dst_dup = strdup(dst);
    if (dst_dup == NULL) return -1;
    const char *dir = dirname(dst_dup);

    /* O_NOFOLLOW prevents following a symlink at the final
     * component of the parent path. Symlinks earlier in the path
     * are still resolved (no walk-time alternative without
     * openat/O_PATH chains), but this catches the common case of
     * an attacker-planted "use this dir instead" link. */
    int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir_fd < 0) {
        log_error("restore: open dir %s: %s", dir, strerror(errno));
        free(dst_dup);
        return -1;
    }

    /* mkstemp uses the path, not an fd. Build the temp path under
     * the same directory so the rename below stays within one
     * filesystem. */
    size_t dlen = strlen(dir);
    char *tmp_path = malloc(dlen + sizeof("/.rnt-restore-XXXXXX"));
    if (tmp_path == NULL) {
        close(dir_fd);
        free(dst_dup);
        return -1;
    }
    sprintf(tmp_path, "%s/.rnt-restore-XXXXXX", dir);

    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        log_error("restore: mkstemp in %s: %s", dir, strerror(errno));
        free(tmp_path);
        close(dir_fd);
        free(dst_dup);
        return -1;
    }

    int blob_fd = store_open_blob(store, v->sha256);
    if (blob_fd < 0) {
        unlink(tmp_path);
        close(tmp_fd);
        free(tmp_path);
        close(dir_fd);
        free(dst_dup);
        return -1;
    }

    char buf[COPY_BUFSZ];
    for (;;) {
        ssize_t r = read(blob_fd, buf, sizeof(buf));
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            goto err;
        }
        ssize_t w = 0;
        while (w < r) {
            ssize_t wn = write(tmp_fd, buf + w, (size_t)(r - w));
            if (wn < 0) {
                if (errno == EINTR) continue;
                goto err;
            }
            w += wn;
        }
    }
    close(blob_fd);
    blob_fd = -1;

    if (preserve_mode) {
        if (fchmod(tmp_fd, (mode_t)v->entry.mode & 07777) < 0) {
            log_warn("restore: fchmod %s: %s", tmp_path, strerror(errno));
            /* Non-fatal. */
        }
        /* fchown only succeeds for the same uid or for root. We
         * try and accept failure quietly — operators running as
         * the file's owner get the right result; otherwise the
         * file lands with the restore process's identity. */
        if (fchown(tmp_fd, (uid_t)v->entry.uid, (gid_t)v->entry.gid) < 0
            && errno != EPERM)
        {
            log_warn("restore: fchown %s: %s", tmp_path, strerror(errno));
        }
        struct timespec times[2];
        times[0].tv_sec  = v->entry.mtime_sec;       /* atime = mtime */
        times[0].tv_nsec = v->entry.mtime_nsec;
        times[1].tv_sec  = v->entry.mtime_sec;
        times[1].tv_nsec = v->entry.mtime_nsec;
        if (futimens(tmp_fd, times) < 0) {
            log_warn("restore: futimens %s: %s", tmp_path, strerror(errno));
        }
    }

    if (fsync_retry(tmp_fd) < 0) {
        log_error("restore: fsync tmp: %s", strerror(errno));
        goto err;
    }
    close(tmp_fd);
    tmp_fd = -1;

    if (rename(tmp_path, dst) < 0) {
        log_error("restore: rename %s -> %s: %s",
                  tmp_path, dst, strerror(errno));
        unlink(tmp_path);
        free(tmp_path);
        close(dir_fd);
        free(dst_dup);
        return -1;
    }

    if (fsync_retry(dir_fd) < 0) {
        log_warn("restore: fsync dir %s: %s", dir, strerror(errno));
        /* Non-fatal — the rename is durable enough for most use. */
    }

    close(dir_fd);
    free(tmp_path);
    free(dst_dup);
    return 0;

err:
    if (blob_fd >= 0) close(blob_fd);
    if (tmp_fd  >= 0) close(tmp_fd);
    if (tmp_path)     unlink(tmp_path);
    free(tmp_path);
    if (dir_fd  >= 0) close(dir_fd);
    free(dst_dup);
    return -1;
}
