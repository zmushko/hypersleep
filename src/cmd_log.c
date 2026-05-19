/*
 * cmd_log.c — renatum log <path>
 *
 * Lists captured versions of a path. Output is newest-first the way
 * `git log` shows commits, but the display rank vN counts from the
 * oldest stored version (v1 = oldest, vN = newest). That matches
 * docs/project-brief.md decision #9: ranks renumber after a prune.
 *
 * v0.1.0 scope: positional path + --limit. The fancier selectors
 * (--since, --until, --range, --follow-renames, --format json) are
 * deferred.
 */

#include "config.h"
#include "index.h"
#include "log.h"
#include "renatum.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The event_mask bits we display. Defined here as portable
 * constants rather than via <sys/inotify.h> so the file compiles
 * on non-Linux libcs for syntax-check builds; the values match the
 * Linux uapi. */
#ifndef IN_CREATE
# define IN_CREATE      0x00000100
#endif
#ifndef IN_MOVED_TO
# define IN_MOVED_TO    0x00000080
#endif
#ifndef IN_CLOSE_WRITE
# define IN_CLOSE_WRITE 0x00000008
#endif

static void format_bytes(uint64_t n, char *out, size_t cap)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB" };
    double v = (double)n;
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }
    if (u == 0) snprintf(out, cap, "%" PRIu64 " %s", n, units[u]);
    else        snprintf(out, cap, "%.1f %s", v, units[u]);
}

static void format_ts(uint64_t ns, char *out, size_t cap)
{
    time_t s = (time_t)(ns / 1000000000ULL);
    struct tm tm;
    localtime_r(&s, &tm);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

static const char *event_label(uint32_t mask)
{
    /* Synthetic initial-scan events carry both CREATE and
     * CLOSE_WRITE; check CREATE first. */
    if (mask & IN_CREATE)      return "create";
    if (mask & IN_MOVED_TO)    return "rename";
    if (mask & IN_CLOSE_WRITE) return "modify";
    return "?";
}

int cmd_log(int argc, char **argv, const rnt_config_t *cfg)
{
    long limit = -1;
    static struct option opts[] = {
        { "limit", required_argument, 0, 'n' },
        { "help",  no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "n:h", opts, NULL)) != -1) {
        switch (c) {
        case 'n': limit = strtol(optarg, NULL, 10); break;
        case 'h':
            fprintf(stderr, "Usage: renatum log [--limit N] <path>\n");
            return RNT_EXIT_OK;
        default:
            return RNT_EXIT_USAGE;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "renatum log: missing <path>\n");
        return RNT_EXIT_USAGE;
    }
    const char *path = argv[optind];

    rnt_index_t *idx = index_open(cfg->index_path, RNT_IDX_READ);
    if (idx == NULL) {
        fprintf(stderr, "renatum log: cannot open index\n");
        return RNT_EXIT_ERROR;
    }

    /* Collect versions into a heap-grown array so we can render
     * newest-first while iter walks oldest-first. */
    size_t cap = 16, n = 0;
    rnt_version_t *list = malloc(cap * sizeof(*list));
    if (list == NULL) { index_close(idx); return RNT_EXIT_ERROR; }

    rnt_index_cursor_t *cur = index_iter_path(idx, path);
    if (cur == NULL) {
        fprintf(stderr, "renatum log: cannot iterate %s\n", path);
        free(list);
        index_close(idx);
        return RNT_EXIT_ERROR;
    }

    rnt_version_t v;
    while (index_cursor_next(cur, &v) == 0) {
        if (n == cap) {
            cap *= 2;
            rnt_version_t *nl = realloc(list, cap * sizeof(*list));
            if (nl == NULL) {
                index_cursor_close(cur);
                free(list);
                index_close(idx);
                return RNT_EXIT_ERROR;
            }
            list = nl;
        }
        list[n++] = v;
    }
    index_cursor_close(cur);
    index_close(idx);

    if (n == 0) {
        fprintf(stderr, "renatum log: no versions for %s\n", path);
        free(list);
        return RNT_EXIT_NOTFOUND;
    }

    /* Display ranks: v1 = oldest, vN = newest. */
    for (size_t i = 0; i < n; i++) {
        list[i].num = (int)(i + 1);
    }

    size_t to_show = (limit > 0 && (size_t)limit < n) ? (size_t)limit : n;
    printf("%-6s %-19s %-10s %-9s %s\n",
           "VER", "CAPTURED", "SIZE", "SHA", "EVENT");
    for (size_t i = 0; i < to_show; i++) {
        const rnt_version_t *e = &list[n - 1 - i];
        char ts[24];  format_ts(e->captured_ns, ts, sizeof(ts));
        char sz[16];  format_bytes(e->entry.size, sz, sizeof(sz));
        char sha[9];
        for (int j = 0; j < 4; j++)
            snprintf(sha + j * 2, 3, "%02x", e->sha256[j]);
        printf("v%-5d %-19s %-10s %-9s %s\n",
               e->num, ts, sz, sha, event_label(e->entry.event_mask));
    }

    free(list);
    return RNT_EXIT_OK;
}
