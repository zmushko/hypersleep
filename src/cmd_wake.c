/*
 * cmd_wake.c — hypersleep wake <path> v<N> [--to <dst> | --force]
 *
 * Two modes:
 *
 *   --to <dst>   (default, safe)
 *       Write the chosen version to a new path. dst must not
 *       exist. The watched file at <path> is not touched.
 *
 *   --force      (destructive, in-place)
 *       Atomically replace the contents of <path> with the
 *       chosen version. Before doing so we ask the daemon (via
 *       the control socket) to snapshot the current on-disk
 *       state, so the operator can later wake back to "what was
 *       there a moment ago". --no-pre-snapshot skips that
 *       handshake — only use it when you are certain the current
 *       state has already been captured or you do not care about
 *       losing it.
 *
 * Selectors: positional vN. Time- and SHA-based selectors come
 * with cmd_show's extension.
 */

#include "config.h"
#include "control_client.h"
#include "index.h"
#include "log.h"
#include "hypersleep.h"
#include "restore.h"
#include "store.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Read a single line from stdin, no echo trickery. Returns NULL on
 * EOF or read error. Strips a trailing newline. Caller must free.
 * Returns NULL also when stdin is not a TTY — `--force` must refuse
 * to proceed without an interactive confirmation in that case. */
static char *read_confirm_line(void)
{
    if (!isatty(STDIN_FILENO)) return NULL;
    char buf[64];
    if (fgets(buf, sizeof(buf), stdin) == NULL) return NULL;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    char *r = malloc(n + 1);
    if (r == NULL) return NULL;
    memcpy(r, buf, n + 1);
    return r;
}

static int prompt_confirm(const char *path, int vN)
{
    fprintf(stderr,
        "About to OVERWRITE %s with v%d. "
        "The current on-disk content will be replaced.\n"
        "Type 'yes' to proceed: ", path, vN);
    fflush(stderr);
    char *line = read_confirm_line();
    if (line == NULL) {
        fprintf(stderr, "\nhypersleep wake: refused (no TTY or EOF)\n");
        return -1;
    }
    int ok = (strcmp(line, "yes") == 0);
    free(line);
    return ok ? 0 : -1;
}

/* SHA-verify the file at `path` after a restore. Returns 0 on match,
 * 1 on mismatch, -1 on read/digest error. */
static int verify_after_restore(const char *path,
                                const uint8_t expected[HS_SHA_LEN]);

int cmd_wake(int argc, char **argv, const hs_config_t *cfg)
{
    const char *to_path = NULL;
    bool force            = false;
    bool preserve_mode    = true;
    bool dry_run          = false;
    bool no_pre_snapshot  = false;
    bool yes_no_confirm   = false;
    bool do_verify_set    = false;
    bool do_verify        = false;   /* default depends on mode */

    static struct option opts[] = {
        { "to",                 required_argument, 0, 't' },
        { "force",              no_argument,       0, 'f' },
        { "preserve-mode",      no_argument,       0, 'p' },
        { "no-preserve-mode",   no_argument,       0, 'P' },
        { "dry-run",            no_argument,       0, 'd' },
        { "no-pre-snapshot",    no_argument,       0, 'S' },
        { "yes",                no_argument,       0, 'y' },
        { "verify",             no_argument,       0, 'v' },
        { "no-verify",          no_argument,       0, 'V' },
        { "help",               no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "t:fpPdSyvVh", opts, NULL)) != -1) {
        switch (c) {
        case 't': to_path = optarg;       break;
        case 'f': force = true;           break;
        case 'p': preserve_mode = true;   break;
        case 'P': preserve_mode = false;  break;
        case 'd': dry_run = true;         break;
        case 'S': no_pre_snapshot = true; break;
        case 'y': yes_no_confirm = true;  break;
        case 'v': do_verify_set = true; do_verify = true;  break;
        case 'V': do_verify_set = true; do_verify = false; break;
        case 'h':
            fprintf(stderr,
                "Usage: hypersleep wake <path> v<N>\n"
                "  --to <dst>               write to a new path (default mode)\n"
                "  --force                  overwrite <path> in place\n"
                "  --no-pre-snapshot        skip the safety pre-snapshot\n"
                "  --yes                    skip interactive confirmation\n"
                "  --[no-]preserve-mode     restore mode/uid/gid/mtime (default yes)\n"
                "  --[no-]verify            SHA-check after write (default: yes for --force)\n"
                "  --dry-run                report intent, change nothing\n");
            return HS_EXIT_OK;
        default:
            return HS_EXIT_USAGE;
        }
    }
    if (optind + 1 >= argc) {
        fprintf(stderr, "hypersleep wake: missing <path> v<N>\n");
        return HS_EXIT_USAGE;
    }
    const char *path = argv[optind];
    int want_n = 0;
    if (parse_vn(argv[optind + 1], &want_n) < 0) {
        fprintf(stderr, "hypersleep wake: invalid '%s' (expected vN)\n",
                argv[optind + 1]);
        return HS_EXIT_USAGE;
    }
    if (force && to_path != NULL) {
        fprintf(stderr,
            "hypersleep wake: --force and --to are mutually exclusive\n");
        return HS_EXIT_USAGE;
    }
    if (!force && to_path == NULL) {
        fprintf(stderr,
            "hypersleep wake: --to <dst> or --force required\n");
        return HS_EXIT_USAGE;
    }
    /* Default verify: yes for --force, no for --to. */
    if (!do_verify_set) do_verify = force;

    /* Resolve the requested version. */
    hs_index_t *idx = index_open(cfg->index_path, HS_IDX_READ);
    if (idx == NULL) {
        fprintf(stderr, "hypersleep wake: cannot open index\n");
        return HS_EXIT_ERROR;
    }
    hs_version_t v;
    if (resolve_version(idx, path, want_n, &v) < 0) {
        index_close(idx);
        if (errno == ENOENT) {
            fprintf(stderr, "hypersleep wake: no versions for %s\n", path);
            return HS_EXIT_NOTFOUND;
        }
        if (errno == ERANGE) {
            fprintf(stderr, "hypersleep wake: v%d out of range\n", want_n);
            return HS_EXIT_NOTFOUND;
        }
        fprintf(stderr, "hypersleep wake: lookup failed: %s\n",
                strerror(errno));
        return HS_EXIT_ERROR;
    }
    index_close(idx);

    if (dry_run) {
        char hex[9];
        for (int i = 0; i < 4; i++)
            snprintf(hex + i * 2, 3, "%02x", v.sha256[i]);
        if (force) {
            printf("would overwrite %s with v%d (sha=%s..)\n",
                   path, v.num, hex);
        } else {
            printf("would restore v%d (sha=%s..) to %s\n",
                   v.num, hex, to_path);
        }
        return HS_EXIT_OK;
    }

    /* Destructive path: pre-snapshot + confirmation. */
    if (force) {
        if (!yes_no_confirm && prompt_confirm(path, v.num) < 0) {
            return HS_EXIT_CANCELLED;
        }
        if (!no_pre_snapshot) {
            uint8_t pre_sha[HS_SHA_LEN];
            char err[256] = {0};
            int rc = control_request_snapshot(cfg->control_socket, path,
                                              pre_sha, err, sizeof(err));
            if (rc == 0) {
                char hex[9];
                for (int i = 0; i < 4; i++)
                    snprintf(hex + i * 2, 3, "%02x", pre_sha[i]);
                fprintf(stderr,
                        "wake: pre-snapshot captured (sha=%s..)\n", hex);
            } else if (rc == -1) {
                fprintf(stderr,
                        "wake: daemon unreachable (%s); proceeding without "
                        "pre-snapshot. Pass --no-pre-snapshot to silence "
                        "this warning, or start hypersleepd first.\n",
                        strerror(errno));
            } else {
                /* rc == -2: daemon replied ERR */
                fprintf(stderr,
                        "wake: daemon refused pre-snapshot: %s\n", err);
                fprintf(stderr,
                        "wake: aborting — pass --no-pre-snapshot to override\n");
                return HS_EXIT_ERROR;
            }
        }
    }

    /* Perform the restore. */
    hs_store_t *st = store_open(cfg->store_path);
    if (st == NULL) {
        fprintf(stderr, "hypersleep wake: cannot open store\n");
        return HS_EXIT_ERROR;
    }
    const char *dst = force ? path : to_path;
    int rc = restore_to(st, &v, dst, preserve_mode, force);
    store_close(st);
    if (rc < 0) {
        if (errno == EEXIST) {
            fprintf(stderr, "hypersleep wake: %s already exists\n", dst);
            return HS_EXIT_EXISTS;
        }
        fprintf(stderr, "hypersleep wake: %s\n", strerror(errno));
        return HS_EXIT_ERROR;
    }

    if (do_verify) {
        int vr = verify_after_restore(dst, v.sha256);
        if (vr == 1) {
            fprintf(stderr,
                "hypersleep wake: post-write SHA MISMATCH at %s — "
                "the file on disk differs from the captured version\n",
                dst);
            return HS_EXIT_CORRUPT;
        }
        if (vr < 0) {
            fprintf(stderr,
                "hypersleep wake: post-write verify failed: %s\n",
                strerror(errno));
            return HS_EXIT_ERROR;
        }
    }

    log_info("woke v%d -> %s%s", v.num, dst, force ? " (in place)" : "");
    return HS_EXIT_OK;
}

/* ---- post-write SHA verification ---- */

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/types.h>

static int verify_after_restore(const char *path,
                                const uint8_t expected[HS_SHA_LEN])
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (md == NULL || EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1) {
        if (md) EVP_MD_CTX_free(md);
        close(fd);
        return -1;
    }
    uint8_t buf[64 * 1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            EVP_MD_CTX_free(md); close(fd);
            return -1;
        }
        EVP_DigestUpdate(md, buf, (size_t)n);
    }
    close(fd);
    uint8_t got[HS_SHA_LEN];
    unsigned int got_len = 0;
    if (EVP_DigestFinal_ex(md, got, &got_len) != 1
        || got_len != HS_SHA_LEN)
    {
        EVP_MD_CTX_free(md);
        return -1;
    }
    EVP_MD_CTX_free(md);
    return memcmp(got, expected, HS_SHA_LEN) == 0 ? 0 : 1;
}
