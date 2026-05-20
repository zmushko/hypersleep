/*
 * cmd_diff.c — hypersleep diff <path> v<A> [v<B>]
 *
 * Forms:
 *   diff <path> vA vB      compare two captured versions
 *   diff <path> vA         compare a captured version with the
 *                          current on-disk file
 *
 * v0.1.0 implementation: extract the chosen versions to temp files
 * under TMPDIR and exec /usr/bin/diff -u. Defers --tool, --stat,
 * --unified N, --color, --from/--to time selectors.
 *
 * Exit code follows diff(1):
 *   0  files identical
 *   1  differences found
 *   2  trouble — diff(1) couldn't process the inputs
 *   1  (HS_EXIT_ERROR) from us on any preparation failure
 */

#include "config.h"
#include "index.h"
#include "log.h"
#include "hypersleep.h"
#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int parse_vn(const char *s, int *out_n)
{
    if (s == NULL || s[0] != 'v') return -1;
    char *end = NULL;
    long n = strtol(s + 1, &end, 10);
    if (end == s + 1 || *end != '\0' || n < 1 || n > INT_MAX) return -1;
    *out_n = (int)n;
    return 0;
}

static int resolve_version(hs_index_t *idx, const char *path,
                           int want_n, hs_version_t *out)
{
    hs_index_cursor_t *cur = index_iter_path(idx, path);
    if (cur == NULL) return -1;
    size_t cap = 16, n = 0;
    hs_version_t *list = malloc(cap * sizeof(*list));
    if (list == NULL) { index_cursor_close(cur); return -1; }
    hs_version_t v;
    while (index_cursor_next(cur, &v) == 0) {
        if (n == cap) {
            cap *= 2;
            hs_version_t *nl = realloc(list, cap * sizeof(*list));
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

/* Extract a blob to a freshly mkstemp'd file. On success the path
 * goes into out_path (caller frees). On error returns -1. */
static int extract_blob(hs_store_t *st, const uint8_t sha[HS_SHA_LEN],
                        char **out_path)
{
    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0') tmpdir = "/tmp";

    char template[PATH_MAX];
    int n = snprintf(template, sizeof(template),
                     "%s/hypersleep-diff-XXXXXX", tmpdir);
    if (n < 0 || (size_t)n >= sizeof(template)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int out_fd = mkstemp(template);
    if (out_fd < 0) return -1;

    int blob_fd = store_open_blob(st, sha);
    if (blob_fd < 0) {
        close(out_fd);
        unlink(template);
        return -1;
    }

    char buf[64 * 1024];
    for (;;) {
        ssize_t r = read(blob_fd, buf, sizeof(buf));
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            close(blob_fd); close(out_fd); unlink(template);
            return -1;
        }
        ssize_t w = 0;
        while (w < r) {
            ssize_t wn = write(out_fd, buf + w, (size_t)(r - w));
            if (wn < 0) {
                if (errno == EINTR) continue;
                close(blob_fd); close(out_fd); unlink(template);
                return -1;
            }
            w += wn;
        }
    }
    close(blob_fd);
    close(out_fd);

    *out_path = strdup(template);
    if (*out_path == NULL) { unlink(template); return -1; }
    return 0;
}

static int run_diff(const char *a, const char *b)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "hypersleep diff: fork: %s\n", strerror(errno));
        return HS_EXIT_ERROR;
    }
    if (pid == 0) {
        execlp("diff", "diff", "-u", a, b, (char *)NULL);
        fprintf(stderr, "hypersleep diff: cannot exec diff: %s\n",
                strerror(errno));
        _exit(2);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "hypersleep diff: waitpid: %s\n", strerror(errno));
        return HS_EXIT_ERROR;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return HS_EXIT_ERROR;
}

int cmd_diff(int argc, char **argv, const hs_config_t *cfg)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: hypersleep diff <path> v<A> [v<B>]\n");
        return HS_EXIT_USAGE;
    }
    const char *path = argv[1];
    int vA = 0, vB = 0;
    bool have_B = false;
    if (parse_vn(argv[2], &vA) < 0) {
        fprintf(stderr, "hypersleep diff: invalid '%s' (expected vN)\n", argv[2]);
        return HS_EXIT_USAGE;
    }
    if (argc >= 4) {
        if (parse_vn(argv[3], &vB) < 0) {
            fprintf(stderr, "hypersleep diff: invalid '%s' (expected vN)\n",
                    argv[3]);
            return HS_EXIT_USAGE;
        }
        have_B = true;
    }

    hs_index_t *idx = index_open(cfg->index_path, HS_IDX_READ);
    if (idx == NULL) {
        fprintf(stderr, "hypersleep diff: cannot open index\n");
        return HS_EXIT_ERROR;
    }
    hs_version_t va, vb;
    if (resolve_version(idx, path, vA, &va) < 0) {
        fprintf(stderr, "hypersleep diff: v%d not found\n", vA);
        index_close(idx);
        return HS_EXIT_NOTFOUND;
    }
    if (have_B && resolve_version(idx, path, vB, &vb) < 0) {
        fprintf(stderr, "hypersleep diff: v%d not found\n", vB);
        index_close(idx);
        return HS_EXIT_NOTFOUND;
    }
    index_close(idx);

    hs_store_t *st = store_open(cfg->store_path);
    if (st == NULL) {
        fprintf(stderr, "hypersleep diff: cannot open store\n");
        return HS_EXIT_ERROR;
    }
    char *tmp_a = NULL;
    if (extract_blob(st, va.sha256, &tmp_a) < 0) {
        fprintf(stderr, "hypersleep diff: extract v%d failed: %s\n",
                vA, strerror(errno));
        store_close(st);
        return HS_EXIT_ERROR;
    }

    char *tmp_b = NULL;
    const char *b_path;
    if (have_B) {
        if (extract_blob(st, vb.sha256, &tmp_b) < 0) {
            fprintf(stderr, "hypersleep diff: extract v%d failed: %s\n",
                    vB, strerror(errno));
            unlink(tmp_a); free(tmp_a);
            store_close(st);
            return HS_EXIT_ERROR;
        }
        b_path = tmp_b;
    } else {
        /* v0.1.0: compare against the live on-disk file. */
        b_path = path;
    }
    store_close(st);

    int rc = run_diff(tmp_a, b_path);

    unlink(tmp_a);
    free(tmp_a);
    if (tmp_b) { unlink(tmp_b); free(tmp_b); }

    /* diff(1)'s 0/1 (same/different) is the natural exit for us
     * too; >1 from diff means trouble — surface as HS_EXIT_ERROR. */
    if (rc == 0 || rc == 1) return rc;
    return HS_EXIT_ERROR;
}
