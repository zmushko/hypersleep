/*
 * control.c — Unix-datagram control socket.
 *
 * The socket is SOCK_DGRAM so each request/response is a single
 * recvfrom/sendto pair; no persistent connections, no framing,
 * no half-open states. The client autobinds an abstract address
 * before sending so the daemon's sendto reply lands back at the
 * same client.
 *
 * The socket fd is registered with the watcher's epoll loop via
 * watcher_attach_fd, so dispatch happens in the same single
 * thread that runs the rest of the daemon — no synchronisation
 * with snapshot / index is needed.
 *
 * Wire format (line-oriented, no framing beyond the datagram):
 *
 *   request   : "SNAPSHOT <abs-path>\n"
 *   reply ok  : "OK <64-hex-chars>\n"
 *   reply err : "ERR <human-readable>\n"
 *
 * On unknown verbs we reply ERR rather than ignoring — the CLI
 * gets a clear signal that it's talking to a daemon that doesn't
 * speak its dialect.
 */

#include "control.h"
#include "log.h"
#include "renatum.h"
#include "watcher.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* SOCK_CLOEXEC and SOCK_NONBLOCK are Linux extensions exposed via
 * <sys/socket.h> when _GNU_SOURCE is defined. On other libcs (e.g.
 * macOS for local compile-checks) they are absent; fall back to
 * fcntl() after socket creation. The runtime target is Linux so
 * the macros are always available in production builds. */
#ifndef SOCK_CLOEXEC
# define SOCK_CLOEXEC 0
#endif
#ifndef SOCK_NONBLOCK
# define SOCK_NONBLOCK 0
#endif

#define CONTROL_MAXMSG 4096

struct rnt_control {
    int             fd;
    char           *socket_path;
    rnt_watcher_t  *watcher;
};

static void send_reply(int fd, const struct sockaddr *peer, socklen_t plen,
                       const char *line)
{
    size_t len = strlen(line);
    ssize_t n = sendto(fd, line, len, 0, peer, plen);
    if (n < 0) {
        log_warn("control: sendto: %s", strerror(errno));
    } else if ((size_t)n != len) {
        log_warn("control: short sendto (%zd of %zu)", n, len);
    }
}

static void send_err(int fd, const struct sockaddr *peer, socklen_t plen,
                     const char *msg)
{
    char buf[CONTROL_MAXMSG];
    int n = snprintf(buf, sizeof(buf), "ERR %s\n", msg);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        send_reply(fd, peer, plen, "ERR internal\n");
        return;
    }
    send_reply(fd, peer, plen, buf);
}

static void handle_snapshot(struct rnt_control *c,
                            const struct sockaddr *peer, socklen_t plen,
                            const char *path)
{
    /* Reject obvious malformed input cheaply. */
    if (path[0] != '/') {
        send_err(c->fd, peer, plen, "path must be absolute");
        return;
    }

    uint8_t sha[RNT_SHA_LEN];
    if (watcher_force_snapshot(c->watcher, path, sha) < 0) {
        char err[256];
        snprintf(err, sizeof(err),
                 "snapshot failed: %s", strerror(errno));
        send_err(c->fd, peer, plen, err);
        return;
    }

    char reply[3 + 1 + RNT_SHA_LEN * 2 + 2];
    char *p = reply;
    memcpy(p, "OK ", 3); p += 3;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < RNT_SHA_LEN; i++) {
        *p++ = hex[(sha[i] >> 4) & 0xf];
        *p++ = hex[ sha[i]       & 0xf];
    }
    *p++ = '\n';
    *p   = '\0';
    send_reply(c->fd, peer, plen, reply);
}

static void dispatch(void *user)
{
    struct rnt_control *c = user;

    char buf[CONTROL_MAXMSG];
    struct sockaddr_un peer;
    socklen_t plen = sizeof(peer);

    ssize_t n = recvfrom(c->fd, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&peer, &plen);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            log_warn("control: recvfrom: %s", strerror(errno));
        }
        return;
    }
    buf[n] = '\0';

    /* Trim trailing newline for parsing. */
    if (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';
    if (n == 0) {
        send_err(c->fd, (struct sockaddr *)&peer, plen, "empty request");
        return;
    }

    const char *verb_end = strchr(buf, ' ');
    if (verb_end == NULL) {
        send_err(c->fd, (struct sockaddr *)&peer, plen,
                 "expected '<VERB> <arg>'");
        return;
    }
    size_t verb_len = (size_t)(verb_end - buf);
    const char *arg = verb_end + 1;

    if (verb_len == 8 && memcmp(buf, "SNAPSHOT", 8) == 0) {
        handle_snapshot(c, (struct sockaddr *)&peer, plen, arg);
    } else {
        send_err(c->fd, (struct sockaddr *)&peer, plen,
                 "unknown verb");
    }
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

/* Best-effort mkdir of the parent directory of socket_path. The
 * systemd unit usually pre-creates /run/renatum, but development
 * runs benefit from a fallback. */
static void ensure_parent_dir(const char *socket_path)
{
    char *dup = strdup(socket_path);
    if (dup == NULL) return;
    char *d = dirname(dup);
    if (d != NULL) {
        if (mkdir(d, 0750) < 0 && errno != EEXIST) {
            log_warn("control: cannot create %s: %s", d, strerror(errno));
        }
    }
    free(dup);
}

rnt_control_t *control_open(const char *socket_path, rnt_watcher_t *watcher)
{
    if (socket_path == NULL || watcher == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        log_error("control: socket path too long: %s", socket_path);
        errno = ENAMETOOLONG;
        return NULL;
    }

    rnt_control_t *c = calloc(1, sizeof(*c));
    if (c == NULL) return NULL;
    c->fd = -1;
    c->watcher = watcher;
    c->socket_path = strdup(socket_path);
    if (c->socket_path == NULL) {
        free(c);
        return NULL;
    }

    ensure_parent_dir(socket_path);

    /* Remove a stale socket file from a previous crashed daemon. */
    if (unlink(socket_path) < 0 && errno != ENOENT) {
        log_warn("control: unlink stale %s: %s",
                 socket_path, strerror(errno));
    }

    c->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (c->fd < 0) {
        log_error("control: socket: %s", strerror(errno));
        goto err;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(c->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("control: bind(%s): %s", socket_path, strerror(errno));
        goto err;
    }
    /* The socket needs to be reachable by the renatum CLI run by
     * users in the renatum group. The systemd unit can override
     * with a tighter mode; this is the sensible default. */
    if (chmod(socket_path, 0660) < 0) {
        log_warn("control: chmod(%s): %s", socket_path, strerror(errno));
    }

    if (watcher_attach_fd(watcher, c->fd, dispatch, c) < 0) {
        log_error("control: watcher_attach_fd: %s", strerror(errno));
        goto err;
    }

    log_info("control: listening on %s", socket_path);
    return c;

err:
    if (c->fd >= 0) close(c->fd);
    if (c->socket_path) unlink(c->socket_path);
    free(c->socket_path);
    free(c);
    return NULL;
}

void control_close(rnt_control_t *c)
{
    if (c == NULL) return;
    if (c->fd >= 0) close(c->fd);
    if (c->socket_path) {
        if (unlink(c->socket_path) < 0 && errno != ENOENT) {
            log_warn("control: unlink %s: %s",
                     c->socket_path, strerror(errno));
        }
        free(c->socket_path);
    }
    free(c);
}
