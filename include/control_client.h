/*
 * control_client.h — CLI side of the daemon control socket.
 *
 * The daemon listens at cfg->control_socket and serves the line
 * protocol defined in control.h:
 *
 *     SNAPSHOT <abs-path>\n   ->  OK <sha-hex>\n
 *                                 ERR <msg>\n
 *
 * Each call here opens a fresh AF_UNIX/SOCK_DGRAM socket,
 * autobinds an abstract address, sends, recvs once, closes.
 * No persistent connection state.
 */

#ifndef HYPERSLEEP_CONTROL_CLIENT_H
#define HYPERSLEEP_CONTROL_CLIENT_H

#include "hypersleep.h"

#include <stdbool.h>

/* Ask the daemon to capture the current on-disk state of `path`.
 *
 * Return values:
 *    0  daemon replied OK; *out_sha holds the captured SHA-256.
 *   -1  the daemon could not be reached (errno set to ECONNREFUSED,
 *       ENOENT, ETIMEDOUT, …) — the caller may proceed without a
 *       pre-snapshot if it has been told to. The caller is
 *       responsible for warning the user.
 *   -2  the daemon was reached but replied ERR (e.g. "path must be
 *       absolute", "snapshot failed: ENOENT"). *out_err receives the
 *       human-readable reason if non-NULL (NUL-terminated, max
 *       err_cap bytes).
 *
 * The receive uses a SO_RCVTIMEO of ~2 seconds so a hung daemon
 * does not block the CLI indefinitely. */
int control_request_snapshot(const char *socket_path,
                             const char *path,
                             uint8_t out_sha[HS_SHA_LEN],
                             char *out_err, size_t err_cap);

#endif /* HYPERSLEEP_CONTROL_CLIENT_H */
