/*
 * cmd_status.c — hypersleep status
 *
 * v0.1.0 scope: daemon pid (via flock check on the lock file),
 * store size + blob count, watched-path list. The fancier numbers
 * shown in docs/cli-spec.md (overflow timestamps, GC history,
 * queue depth peak) need extra meta-DB entries that the daemon
 * does not write yet; they will be added with the audit
 * pipeline.
 */

#include "config.h"
#include "log.h"
#include "hypersleep.h"
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

struct blob_acc {
    size_t   count;
    uint64_t bytes;
};

static int blob_count_cb(const uint8_t sha[HS_SHA_LEN], off_t size, void *u)
{
    (void)sha;
    struct blob_acc *a = u;
    a->count++;
    a->bytes += (uint64_t)size;
    return 0;
}

static void format_bytes(uint64_t n, char *out, size_t cap)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0) snprintf(out, cap, "%" PRIu64 " %s", n, units[u]);
    else        snprintf(out, cap, "%.1f %s", v, units[u]);
}

/* Probe the daemon's lock file. Three-valued return:
 *    1  the lock is held by someone (i.e. daemon running)
 *    0  the lock is free (daemon not running)
 *   -1  cannot tell (permission denied, file missing, …) — the
 *      lock file is typically 0600 root, so a non-root CLI gets
 *      EACCES even when the daemon is up; we surface that as
 *      "unknown" instead of asserting "not running". */
static int daemon_status(const char *lock_path)
{
    int fd = open(lock_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return (errno == ENOENT) ? 0 : -1;
    }
    if (flock(fd, LOCK_SH | LOCK_NB) == 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

int cmd_status(int argc, char **argv, const hs_config_t *cfg)
{
    (void)argc; (void)argv;
    if (cfg == NULL) return HS_EXIT_ERROR;

    int st = daemon_status(cfg->lock_path);
    if (st > 0) {
        printf("hypersleepd: running\n");
    } else if (st == 0) {
        printf("hypersleepd: not running\n");
    } else {
        printf("hypersleepd: unknown (cannot probe %s: %s — "
               "run as root or join group with read access)\n",
               cfg->lock_path, strerror(errno));
    }
    printf("store:    %s\n", cfg->store_path);
    printf("index:    %s\n", cfg->index_path);

    /* Walk the store to count blobs + bytes. On a fresh install
     * this is fast; on a 100k-blob store it takes a few hundred ms
     * of opendir/lstat, which is acceptable for an operator-typed
     * status command. */
    hs_store_t *s = store_open(cfg->store_path);
    if (s != NULL) {
        struct blob_acc acc = { 0, 0 };
        if (store_iterate(s, blob_count_cb, &acc) == 0) {
            char sz[32];
            format_bytes(acc.bytes, sz, sizeof(sz));
            printf("          %zu blobs, %s\n", acc.count, sz);
        }
        store_close(s);
    }

    printf("\nwatching %zu paths:\n", cfg->n_watches);
    for (size_t i = 0; i < cfg->n_watches; i++) {
        printf("  %s%s%s\n",
               cfg->watches[i].path,
               cfg->watches[i].has_exclude ? "  (exclude=set)" : "",
               cfg->watches[i].compress    ? "  (compress)"    : "");
    }

    return HS_EXIT_OK;
}
