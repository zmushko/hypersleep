/*
 * retention.c — CAS-side cleanup after an index prune.
 *
 * index_prune_by_age removes by_sha entries whose refcount drops to
 * zero; the blob files themselves stay on disk. This module walks
 * the CAS, checks each blob's refcount in the index, and unlinks
 * the orphans.
 *
 * Race note: if `hypersleep prune` runs while hypersleepd is writing
 * (single-writer LMDB), the writer-lock contention serialises us.
 * A blob added by the daemon between our refcount check and our
 * store_remove call would be erroneously deleted. The window is
 * narrow but real; v0.1.0+ should harden by either pausing the
 * daemon via SIGSTOP for the sweep, or routing prune through the
 * control socket so the daemon does it inline. For now operators
 * are advised to stop hypersleepd before pruning.
 */

#include "retention.h"
#include "index.h"
#include "log.h"
#include "hypersleep.h"
#include "store.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

struct sweep_state {
    hs_index_t *idx;
    hs_store_t *store;
    size_t       removed;
};

static int sweep_cb(const uint8_t sha[HS_SHA_LEN], off_t size, void *user)
{
    (void)size;
    struct sweep_state *s = user;
    uint32_t refs = 0;
    if (index_sha_refcount(s->idx, sha, &refs) < 0) {
        /* Skip this blob — already logged inside index. Keep walking. */
        return 0;
    }
    if (refs == 0) {
        if (store_remove(s->store, sha) == 0) {
            s->removed++;
        }
    }
    return 0;
}

int retention_sweep_orphans(hs_index_t *idx, hs_store_t *store,
                            size_t *out_removed)
{
    if (idx == NULL || store == NULL) {
        errno = EINVAL;
        return -1;
    }
    struct sweep_state st = { idx, store, 0 };
    if (store_iterate(store, sweep_cb, &st) < 0) {
        return -1;
    }
    if (out_removed) *out_removed = st.removed;
    return 0;
}
