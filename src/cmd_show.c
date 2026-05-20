/*
 * cmd_show.c — hypersleep show <path> v<N>
 *
 * Prints the contents of a captured version to stdout (or to a file
 * via --out). Never touches the original watched path — that is
 * what `hypersleep wake` is for.
 *
 * v0.1.0 scope: positional vN selector only. The --at/--before/
 * --by-sha selectors and --no-decompress flag are deferred.
 */

#include "config.h"
#include "index.h"
#include "log.h"
#include "hypersleep.h"
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Find the hs_file_entry for the requested version number. Walks
 * all versions of `path` in the index so we can assign display
 * ranks (v1 = oldest, vN = newest) consistently with cmd_log. */
static int resolve_version(hs_index_t *idx, const char *path,
                           int want_n, hs_version_t *out)
{
    hs_index_cursor_t *cur = index_iter_path(idx, path);
    if (cur == NULL) return -1;

    /* Buffer all entries first, then pick the want_n-th by rank. */
    size_t cap = 16, n = 0;
    hs_version_t *list = malloc(cap * sizeof(*list));
    if (list == NULL) {
        index_cursor_close(cur);
        return -1;
    }
    hs_version_t v;
    while (index_cursor_next(cur, &v) == 0) {
        if (n == cap) {
            cap *= 2;
            hs_version_t *nl = realloc(list, cap * sizeof(*list));
            if (nl == NULL) {
                free(list);
                index_cursor_close(cur);
                return -1;
            }
            list = nl;
        }
        list[n++] = v;
    }
    index_cursor_close(cur);

    if (n == 0) {
        free(list);
        errno = ENOENT;
        return -1;
    }
    if (want_n < 1 || (size_t)want_n > n) {
        free(list);
        errno = ERANGE;
        return -1;
    }
    *out = list[want_n - 1];
    out->num = want_n;
    free(list);
    return 0;
}

static int parse_vn(const char *s, int *out_n)
{
    if (s == NULL || s[0] != 'v') return -1;
    char *end = NULL;
    long n = strtol(s + 1, &end, 10);
    if (end == s + 1 || *end != '\0' || n < 1 || n > INT_MAX) return -1;
    *out_n = (int)n;
    return 0;
}

static int copy_fd_to_fd(int in, int out)
{
    char buf[64 * 1024];
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        ssize_t w = 0;
        while (w < r) {
            ssize_t wn = write(out, buf + w, (size_t)(r - w));
            if (wn < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            w += wn;
        }
    }
}

int cmd_show(int argc, char **argv, const hs_config_t *cfg)
{
    const char *out_path = NULL;
    static struct option opts[] = {
        { "out",  required_argument, 0, 'o' },
        { "help", no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "o:h", opts, NULL)) != -1) {
        switch (c) {
        case 'o': out_path = optarg; break;
        case 'h':
            fprintf(stderr,
                "Usage: hypersleep show [--out FILE] <path> v<N>\n");
            return HS_EXIT_OK;
        default:
            return HS_EXIT_USAGE;
        }
    }
    if (optind + 1 >= argc) {
        fprintf(stderr, "hypersleep show: missing <path> v<N>\n");
        return HS_EXIT_USAGE;
    }
    const char *path = argv[optind];
    int want_n = 0;
    if (parse_vn(argv[optind + 1], &want_n) < 0) {
        fprintf(stderr,
                "hypersleep show: invalid version selector '%s' (expected vN)\n",
                argv[optind + 1]);
        return HS_EXIT_USAGE;
    }

    hs_index_t *idx = index_open(cfg->index_path, HS_IDX_READ);
    if (idx == NULL) {
        fprintf(stderr, "hypersleep show: cannot open index\n");
        return HS_EXIT_ERROR;
    }
    hs_version_t v;
    if (resolve_version(idx, path, want_n, &v) < 0) {
        if (errno == ENOENT) {
            fprintf(stderr, "hypersleep show: no versions for %s\n", path);
            index_close(idx);
            return HS_EXIT_NOTFOUND;
        }
        if (errno == ERANGE) {
            fprintf(stderr, "hypersleep show: v%d out of range\n", want_n);
            index_close(idx);
            return HS_EXIT_NOTFOUND;
        }
        fprintf(stderr, "hypersleep show: lookup failed: %s\n", strerror(errno));
        index_close(idx);
        return HS_EXIT_ERROR;
    }
    index_close(idx);

    hs_store_t *st = store_open(cfg->store_path);
    if (st == NULL) {
        fprintf(stderr, "hypersleep show: cannot open store\n");
        return HS_EXIT_ERROR;
    }
    int blob_fd = store_open_blob(st, v.sha256);
    if (blob_fd < 0) {
        fprintf(stderr, "hypersleep show: blob missing in CAS (corruption?)\n");
        store_close(st);
        return HS_EXIT_CORRUPT;
    }

    int out_fd = STDOUT_FILENO;
    if (out_path) {
        /* O_NOFOLLOW so the user cannot have a pre-planted symlink
         * at --out steer the bytes into a victim file. O_EXCL
         * already forbids overwriting, but a symlink could point
         * at a not-yet-existing path. */
        out_fd = open(out_path,
                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                      0600);
        if (out_fd < 0) {
            fprintf(stderr, "hypersleep show: cannot create %s: %s\n",
                    out_path, strerror(errno));
            close(blob_fd);
            store_close(st);
            return errno == EEXIST ? HS_EXIT_EXISTS : HS_EXIT_ERROR;
        }
    }

    int rc = copy_fd_to_fd(blob_fd, out_fd);
    close(blob_fd);
    if (out_fd != STDOUT_FILENO) close(out_fd);
    store_close(st);
    if (rc < 0) {
        fprintf(stderr, "hypersleep show: copy failed: %s\n", strerror(errno));
        return HS_EXIT_ERROR;
    }
    return HS_EXIT_OK;
}
