/*
 * cmd_verify.c — renatum verify
 *
 * Walks the CAS, re-hashes each blob, and reports mismatches.
 *
 * v0.1.0 scope: full sweep only. --quick (sample-based), --repair
 * (quarantine) and --path (single path's history) are deferred.
 * The exit code follows docs/cli-spec.md: 0 on clean, RNT_EXIT_CORRUPT
 * if anything failed verification.
 */

#include "config.h"
#include "log.h"
#include "renatum.h"
#include "store.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

struct verify_acc {
    rnt_store_t *store;
    size_t       checked;
    size_t       mismatches;
    size_t       errors;
};

static int verify_cb(const uint8_t sha[RNT_SHA_LEN], off_t size, void *user)
{
    (void)size;
    struct verify_acc *a = user;
    int rc = store_verify_blob(a->store, sha);
    a->checked++;
    if (rc == 1) {
        a->mismatches++;
        char hex[RNT_SHA_LEN * 2 + 1];
        static const char tab[] = "0123456789abcdef";
        for (int i = 0; i < RNT_SHA_LEN; i++) {
            hex[i * 2]     = tab[(sha[i] >> 4) & 0xf];
            hex[i * 2 + 1] = tab[ sha[i]       & 0xf];
        }
        hex[RNT_SHA_LEN * 2] = '\0';
        fprintf(stderr, "verify: MISMATCH %s\n", hex);
    } else if (rc < 0) {
        a->errors++;
    }
    /* keep iterating regardless */
    return 0;
}

int cmd_verify(int argc, char **argv, const rnt_config_t *cfg)
{
    (void)argc; (void)argv;
    if (cfg == NULL) return RNT_EXIT_ERROR;

    rnt_store_t *s = store_open(cfg->store_path);
    if (s == NULL) {
        fprintf(stderr, "renatum verify: cannot open store\n");
        return RNT_EXIT_ERROR;
    }

    struct verify_acc acc = { s, 0, 0, 0 };
    if (store_iterate(s, verify_cb, &acc) < 0) {
        fprintf(stderr, "renatum verify: iterate failed\n");
        store_close(s);
        return RNT_EXIT_ERROR;
    }
    store_close(s);

    printf("verify: %zu blobs checked, %zu mismatches, %zu errors\n",
           acc.checked, acc.mismatches, acc.errors);
    if (acc.mismatches > 0) return RNT_EXIT_CORRUPT;
    if (acc.errors > 0)     return RNT_EXIT_ERROR;
    return RNT_EXIT_OK;
}
