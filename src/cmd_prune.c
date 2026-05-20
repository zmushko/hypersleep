/*
 * cmd_prune.c — hypersleep prune --older-than <duration>
 *
 * v0.1.0 scope: --older-than only. The borg-style --keep-N-* flags,
 * --path prefix filter, and --dry-run are deferred.
 *
 * Pipeline:
 *   1. open the index for WRITE — this contends with hypersleepd if
 *      it is running (single-writer LMDB). Operators should stop
 *      the daemon before running prune in v0.1.0; v0.1.0+ will
 *      route prune through the control socket.
 *   2. index_prune_by_age — deletes file entries with captured_ts
 *      older than the cutoff and decrements/removes by_sha
 *      refcounts in the same transaction.
 *   3. retention_sweep_orphans — walks the CAS and unlinks the
 *      blobs whose by_sha refcount is now zero.
 */

#include "config.h"
#include "index.h"
#include "log.h"
#include "hypersleep.h"
#include "retention.h"
#include "store.h"
#include "timeparse.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int cmd_prune(int argc, char **argv, const hs_config_t *cfg)
{
    const char *older_than = NULL;
    bool no_gc = false;

    static struct option opts[] = {
        { "older-than", required_argument, 0, 't' },
        { "no-gc",      no_argument,       0, 'G' },
        { "help",       no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "t:Gh", opts, NULL)) != -1) {
        switch (c) {
        case 't': older_than = optarg; break;
        case 'G': no_gc = true;        break;
        case 'h':
            fprintf(stderr,
                "Usage: hypersleep prune --older-than <duration> "
                "[--no-gc]\n");
            return HS_EXIT_OK;
        default:
            return HS_EXIT_USAGE;
        }
    }
    if (older_than == NULL) {
        fprintf(stderr, "hypersleep prune: --older-than required\n");
        return HS_EXIT_USAGE;
    }

    uint64_t window_ns = 0;
    if (parse_duration_ns(older_than, &window_ns) < 0) {
        fprintf(stderr,
                "hypersleep prune: invalid duration '%s'\n", older_than);
        return HS_EXIT_USAGE;
    }
    if (window_ns == 0) {
        fprintf(stderr,
                "hypersleep prune: 'infinite' makes no sense for --older-than\n");
        return HS_EXIT_USAGE;
    }
    uint64_t cutoff = now_ns();
    if (cutoff < window_ns) {
        fprintf(stderr,
                "hypersleep prune: cutoff would go negative; clock skew?\n");
        return HS_EXIT_ERROR;
    }
    cutoff -= window_ns;

    hs_index_t *idx = index_open(cfg->index_path, HS_IDX_WRITE);
    if (idx == NULL) {
        fprintf(stderr,
                "hypersleep prune: cannot open index for write "
                "(daemon running?): %s\n", strerror(errno));
        return HS_EXIT_ERROR;
    }

    size_t blobs_orphaned = 0;
    if (index_prune_by_age(idx, cutoff, &blobs_orphaned) < 0) {
        fprintf(stderr, "hypersleep prune: index_prune_by_age failed\n");
        index_close(idx);
        return HS_EXIT_ERROR;
    }
    printf("prune: %zu blob ref%s dropped to zero\n",
           blobs_orphaned, blobs_orphaned == 1 ? "" : "s");

    if (!no_gc && blobs_orphaned > 0) {
        hs_store_t *st = store_open(cfg->store_path);
        if (st == NULL) {
            fprintf(stderr, "hypersleep prune: cannot open store for GC\n");
            index_close(idx);
            return HS_EXIT_ERROR;
        }
        size_t removed = 0;
        if (retention_sweep_orphans(idx, st, &removed) < 0) {
            fprintf(stderr, "hypersleep prune: GC sweep failed\n");
            store_close(st);
            index_close(idx);
            return HS_EXIT_ERROR;
        }
        store_close(st);
        printf("gc: %zu orphan blob%s removed\n",
               removed, removed == 1 ? "" : "s");
    }

    index_close(idx);
    return HS_EXIT_OK;
}
