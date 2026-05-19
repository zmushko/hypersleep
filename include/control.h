/*
 * control.h — Unix-datagram control socket for the daemon.
 *
 * Wire protocol (one-line request / one-line reply, no framing):
 *
 *   request    = "SNAPSHOT <abs-path>\n"
 *   reply      = "OK <sha-hex>\n"          // captured (or already up to date)
 *              | "ERR <message>\n"          // capture failed
 *
 * The CLI uses this for the pre-snapshot dance before a destructive
 * `restore --force`: it sends SNAPSHOT before opening the file for
 * writing, ensuring the daemon has captured the current on-disk
 * state into the index even if the daemon was lagging on events.
 */

#ifndef RENATUM_CONTROL_H
#define RENATUM_CONTROL_H

#include "watcher.h"

typedef struct rnt_control rnt_control_t;

/* Create the control socket, bind to socket_path, and attach it to
 * the watcher's epoll loop. Returns NULL with errno set on failure
 * (already logged). */
rnt_control_t *control_open(const char *socket_path,
                            rnt_watcher_t *watcher);

void control_close(rnt_control_t *c);

#endif /* RENATUM_CONTROL_H */
