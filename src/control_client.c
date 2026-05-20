/*
 * control_client.c — sends SNAPSHOT requests to the daemon.
 *
 * One request, one reply, one close. The client autobinds an
 * abstract socket address (sun_path[0]='\0') so the daemon's
 * reply has a sender to send to without us touching the filesystem.
 *
 * Wire format mirror of control.c:
 *
 *     request : "SNAPSHOT <abs-path>\n"
 *     reply   : "OK <64-hex>\n"  or  "ERR <message>\n"
 */

#include "control_client.h"
#include "hypersleep.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define REPLY_MAX 4096

/* SOCK_CLOEXEC: Linux extension; portability shim. */
#ifndef SOCK_CLOEXEC
# define SOCK_CLOEXEC 0
#endif

static int hex_byte(char hi, char lo, uint8_t *out)
{
    int h = -1, l = -1;
    if (hi >= '0' && hi <= '9') h = hi - '0';
    else if (hi >= 'a' && hi <= 'f') h = 10 + hi - 'a';
    else if (hi >= 'A' && hi <= 'F') h = 10 + hi - 'A';
    if (lo >= '0' && lo <= '9') l = lo - '0';
    else if (lo >= 'a' && lo <= 'f') l = 10 + lo - 'a';
    else if (lo >= 'A' && lo <= 'F') l = 10 + lo - 'A';
    if (h < 0 || l < 0) return -1;
    *out = (uint8_t)((h << 4) | l);
    return 0;
}

static int parse_ok(const char *body, uint8_t out_sha[HS_SHA_LEN])
{
    /* body is the part after "OK "; expect exactly 64 hex chars. */
    if (strlen(body) < HS_SHA_LEN * 2) return -1;
    for (int i = 0; i < HS_SHA_LEN; i++) {
        if (hex_byte(body[i * 2], body[i * 2 + 1], &out_sha[i]) < 0) {
            return -1;
        }
    }
    return 0;
}

int control_request_snapshot(const char *socket_path,
                             const char *path,
                             uint8_t out_sha[HS_SHA_LEN],
                             char *out_err, size_t err_cap)
{
    if (socket_path == NULL || path == NULL || out_sha == NULL) {
        errno = EINVAL;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    /* Autobind an abstract sockaddr_un so the daemon's sendto reply
     * lands back here. Linux feature; kernel picks a unique address
     * when sun_path[0] == '\0' and addrlen == sizeof(sa_family_t). */
    struct sockaddr_un self;
    memset(&self, 0, sizeof(self));
    self.sun_family = AF_UNIX;
    if (bind(fd, (struct sockaddr *)&self, sizeof(sa_family_t)) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    /* 2 s recv timeout — long enough for the daemon to capture a
     * normal file, short enough not to hang the CLI on a stuck
     * server. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_un peer;
    memset(&peer, 0, sizeof(peer));
    peer.sun_family = AF_UNIX;
    if (strlen(socket_path) >= sizeof(peer.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strncpy(peer.sun_path, socket_path, sizeof(peer.sun_path) - 1);

    char req[2048];
    int rn = snprintf(req, sizeof(req), "SNAPSHOT %s\n", path);
    if (rn < 0 || (size_t)rn >= sizeof(req)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    if (sendto(fd, req, (size_t)rn, 0,
               (struct sockaddr *)&peer, sizeof(peer)) < 0)
    {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    char buf[REPLY_MAX];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    if (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';

    if (n >= 3 && memcmp(buf, "OK ", 3) == 0) {
        if (parse_ok(buf + 3, out_sha) == 0) return 0;
        if (out_err && err_cap > 0) {
            snprintf(out_err, err_cap,
                     "malformed OK reply: %s", buf + 3);
        }
        return -2;
    }
    if (n >= 4 && memcmp(buf, "ERR ", 4) == 0) {
        if (out_err && err_cap > 0) {
            snprintf(out_err, err_cap, "%s", buf + 4);
        }
        return -2;
    }
    if (out_err && err_cap > 0) {
        snprintf(out_err, err_cap, "unrecognised reply: %.200s", buf);
    }
    return -2;
}
