/*
 * cmd_restore.c — renatum restore <path> v<N> --to <dst>
 *
 * v0.1.0 scope: --to only. --force (destructive in-place restore
 * with pre-snapshot via control socket) is deferred to v0.1.0+.
 * The control-socket protocol exists on the daemon side, but the
 * CLI-side client + interactive prompts + integrity verification
 * loop need their own commit.
 *
 * Selectors: positional vN. Time- and SHA-based selectors come
 * with cmd_show's extension.
 */

#include "config.h"
#include "index.h"
#include "log.h"
#include "renatum.h"
#include "restore.h"
#include "store.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_vn(const char *s, int *out_n)
{
    if (s == NULL || s[0] != 'v') return -1;
    char *end = NULL;
    long n = strtol(s + 1, &end, 10);
    if (end == s + 1 || *end != '\0' || n < 1 || n > INT_MAX) return -1;
    *out_n = (int)n;
    return 0;
}

static int resolve_version(rnt_index_t *idx, const char *path,
                           int want_n, rnt_version_t *out)
{
    rnt_index_cursor_t *cur = index_iter_path(idx, path);
    if (cur == NULL) return -1;
    size_t cap = 16, n = 0;
    rnt_version_t *list = malloc(cap * sizeof(*list));
    if (list == NULL) { index_cursor_close(cur); return -1; }
    rnt_version_t v;
    while (index_cursor_next(cur, &v) == 0) {
        if (n == cap) {
            cap *= 2;
            rnt_version_t *nl = realloc(list, cap * sizeof(*list));
            if (nl == NULL) { free(list); index_cursor_close(cur); return -1; }
            list = nl;
        }
        list[n++] = v;
    }
    index_cursor_close(cur);
    if (n == 0) { free(list); errno = ENOENT; return -1; }
    if (want_n < 1 || (size_t)want_n > n) {
        free(list); errno = ERANGE; return -1;
    }
    *out = list[want_n - 1];
    out->num = want_n;
    free(list);
    return 0;
}

int cmd_restore(int argc, char **argv, const rnt_config_t *cfg)
{
    const char *to_path = NULL;
    bool force         = false;
    bool preserve_mode = true;
    bool dry_run       = false;

    static struct option opts[] = {
        { "to",                 required_argument, 0, 't' },
        { "force",              no_argument,       0, 'f' },
        { "preserve-mode",      no_argument,       0, 'p' },
        { "no-preserve-mode",   no_argument,       0, 'P' },
        { "dry-run",            no_argument,       0, 'd' },
        { "help",               no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "t:fpPdh", opts, NULL)) != -1) {
        switch (c) {
        case 't': to_path = optarg;       break;
        case 'f': force = true;           break;
        case 'p': preserve_mode = true;   break;
        case 'P': preserve_mode = false;  break;
        case 'd': dry_run = true;         break;
        case 'h':
            fprintf(stderr,
                "Usage: renatum restore <path> v<N> --to <dst> "
                "[--preserve-mode|--no-preserve-mode] [--dry-run]\n");
            return RNT_EXIT_OK;
        default:
            return RNT_EXIT_USAGE;
        }
    }
    if (optind + 1 >= argc) {
        fprintf(stderr, "renatum restore: missing <path> v<N>\n");
        return RNT_EXIT_USAGE;
    }
    const char *path = argv[optind];
    int want_n = 0;
    if (parse_vn(argv[optind + 1], &want_n) < 0) {
        fprintf(stderr, "renatum restore: invalid '%s' (expected vN)\n",
                argv[optind + 1]);
        return RNT_EXIT_USAGE;
    }

    if (force) {
        fprintf(stderr,
            "renatum restore: --force is not wired in v0.1.0; "
            "use --to <dst> to write the version to a new path\n");
        return RNT_EXIT_USAGE;
    }
    if (to_path == NULL) {
        fprintf(stderr,
            "renatum restore: --to <dst> required (use --force for "
            "in-place when available)\n");
        return RNT_EXIT_EXISTS;
    }

    rnt_index_t *idx = index_open(cfg->index_path, RNT_IDX_READ);
    if (idx == NULL) {
        fprintf(stderr, "renatum restore: cannot open index\n");
        return RNT_EXIT_ERROR;
    }
    rnt_version_t v;
    if (resolve_version(idx, path, want_n, &v) < 0) {
        index_close(idx);
        if (errno == ENOENT) {
            fprintf(stderr, "renatum restore: no versions for %s\n", path);
            return RNT_EXIT_NOTFOUND;
        }
        if (errno == ERANGE) {
            fprintf(stderr, "renatum restore: v%d out of range\n", want_n);
            return RNT_EXIT_NOTFOUND;
        }
        fprintf(stderr, "renatum restore: lookup failed: %s\n", strerror(errno));
        return RNT_EXIT_ERROR;
    }
    index_close(idx);

    if (dry_run) {
        char hex[9];
        for (int i = 0; i < 4; i++)
            snprintf(hex + i * 2, 3, "%02x", v.sha256[i]);
        printf("would restore v%d (sha=%s..) to %s\n", v.num, hex, to_path);
        return RNT_EXIT_OK;
    }

    rnt_store_t *st = store_open(cfg->store_path);
    if (st == NULL) {
        fprintf(stderr, "renatum restore: cannot open store\n");
        return RNT_EXIT_ERROR;
    }
    int rc = restore_to(st, &v, to_path, preserve_mode);
    store_close(st);
    if (rc < 0) {
        if (errno == EEXIST) {
            fprintf(stderr, "renatum restore: %s already exists\n", to_path);
            return RNT_EXIT_EXISTS;
        }
        fprintf(stderr, "renatum restore: %s\n", strerror(errno));
        return RNT_EXIT_ERROR;
    }
    log_info("restored v%d -> %s", v.num, to_path);
    return RNT_EXIT_OK;
}
