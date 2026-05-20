/*
 * main.c — hypersleepd entry point
 *
 * Apache-2.0
 *
 * Responsibilities:
 *   1. Parse command-line flags (--config, --foreground, --version, etc.)
 *   2. Load and validate config
 *   3. Daemonize unless --foreground
 *   4. Install signal handlers (SIGTERM, SIGINT, SIGHUP)
 *   5. Acquire flock on /var/lib/hypersleep/lock
 *   6. Open store and index
 *   7. Create snapshot pipeline, debouncer, watcher
 *   8. Run the event loop
 *   9. Tear down cleanly on shutdown signal
 */

#include "config.h"
#include "log.h"
#include "watcher.h"
#include "control.h"
#include "snapshot.h"
#include "store.h"
#include "index.h"
#include "hypersleep.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <sys/file.h>
#include <fcntl.h>

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload = 0;

static void on_term(int sig) { (void)sig; g_running = 0; }
static void on_hup(int sig)  { (void)sig; g_reload = 1; }

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [--foreground] [--config FILE] [--version]\n", prog);
}

static int acquire_lock(const char *path) {
    /* O_NOFOLLOW prevents a pre-planted symlink at the lock path
     * from steering us into a victim file when O_CREAT triggers. */
    int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    /* fd intentionally leaked — flock released on process exit */
    return 0;
}

int main(int argc, char **argv) {
    const char *config_path = "/etc/hypersleep/hypersleep.conf";
    bool foreground = false;

    static struct option opts[] = {
        { "config",     required_argument, 0, 'c' },
        { "foreground", no_argument,       0, 'f' },
        { "version",    no_argument,       0, 'V' },
        { "help",       no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:fVh", opts, NULL)) != -1) {
        switch (c) {
        case 'c': config_path = optarg; break;
        case 'f': foreground = true; break;
        case 'V':
            printf("hypersleepd %d.%d.%d\n",
                   HYPERSLEEP_VERSION_MAJOR,
                   HYPERSLEEP_VERSION_MINOR,
                   HYPERSLEEP_VERSION_PATCH);
            return 0;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return HS_EXIT_USAGE;
        }
    }

    hs_config_t *cfg = config_load(config_path);
    if (!cfg) {
        fprintf(stderr, "hypersleepd: failed to load config %s\n", config_path);
        return HS_EXIT_USAGE;
    }
    if (config_validate(cfg) != 0) {
        fprintf(stderr, "hypersleepd: invalid config\n");
        config_free(cfg);
        return HS_EXIT_USAGE;
    }

    log_init(cfg, foreground);

    /* TODO: daemonize() if !foreground */

    if (acquire_lock("/var/lib/hypersleep/lock") != 0) {
        log_error("another hypersleepd is already running");
        config_free(cfg);
        return HS_EXIT_ERROR;
    }

    struct sigaction sa = { 0 };
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sa.sa_handler = on_hup;
    sigaction(SIGHUP,  &sa, NULL);

    hs_store_t *store = store_open(cfg->store_path);
    if (!store) { log_error("store_open failed"); goto err_cfg; }

    hs_index_t *index = index_open(cfg->index_path, HS_IDX_WRITE);
    if (!index) { log_error("index_open failed"); goto err_store; }

    if (index_schema_check(index) < 0) {
        log_error("incompatible index schema version");
        goto err_index;
    }

    hs_snapshot_t *snapshot = snapshot_create(store, index);
    if (!snapshot) { log_error("snapshot_create failed"); goto err_index; }

    hs_watcher_t *watcher = watcher_create(cfg, snapshot, &g_running);
    if (!watcher) { log_error("watcher_create failed"); goto err_snap; }

    /* Control socket is optional — if /run/hypersleep/ is unwritable
     * (no permissions, no systemd-managed runtime dir), log and
     * continue without the pre-snapshot rendezvous. The CLI falls
     * back to its --no-pre-snapshot behaviour with a warning. */
    hs_control_t *control = control_open(cfg->control_socket, watcher);
    if (!control) {
        log_warn("hypersleepd: control socket disabled (%s)", strerror(errno));
    }

    log_info("hypersleepd %d.%d.%d started, watching %zu paths",
             HYPERSLEEP_VERSION_MAJOR, HYPERSLEEP_VERSION_MINOR,
             HYPERSLEEP_VERSION_PATCH, cfg->n_watches);

    int rc = watcher_run(watcher);

    log_info("hypersleepd shutting down");

    if (control) control_close(control);
    watcher_destroy(watcher);
    snapshot_destroy(snapshot);
    index_close(index);
    store_close(store);
    config_free(cfg);
    log_close();
    return rc;

err_snap:  snapshot_destroy(snapshot);
err_index: index_close(index);
err_store: store_close(store);
err_cfg:   config_free(cfg);
    return HS_EXIT_ERROR;
}
