/*
 * main.c — renatumd entry point
 *
 * Apache-2.0
 *
 * Responsibilities:
 *   1. Parse command-line flags (--config, --foreground, --version, etc.)
 *   2. Load and validate config
 *   3. Daemonize unless --foreground
 *   4. Install signal handlers (SIGTERM, SIGINT, SIGHUP)
 *   5. Acquire flock on /var/lib/renatum/lock
 *   6. Open store and index
 *   7. Create snapshot pipeline, debouncer, watcher
 *   8. Run the event loop
 *   9. Tear down cleanly on shutdown signal
 */

#include "config.h"
#include "log.h"
#include "watcher.h"
#include "snapshot.h"
#include "store.h"
#include "index.h"
#include "renatum.h"

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
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    /* fd intentionally leaked — flock released on process exit */
    return 0;
}

int main(int argc, char **argv) {
    const char *config_path = "/etc/renatum/renatum.conf";
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
            printf("renatumd %d.%d.%d\n",
                   RENATUM_VERSION_MAJOR,
                   RENATUM_VERSION_MINOR,
                   RENATUM_VERSION_PATCH);
            return 0;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return RNT_EXIT_USAGE;
        }
    }

    rnt_config_t *cfg = config_load(config_path);
    if (!cfg) {
        fprintf(stderr, "renatumd: failed to load config %s\n", config_path);
        return RNT_EXIT_USAGE;
    }
    if (config_validate(cfg) != 0) {
        fprintf(stderr, "renatumd: invalid config\n");
        config_free(cfg);
        return RNT_EXIT_USAGE;
    }

    log_init(cfg, foreground);

    /* TODO: daemonize() if !foreground */

    if (acquire_lock("/var/lib/renatum/lock") != 0) {
        log_error("another renatumd is already running");
        config_free(cfg);
        return RNT_EXIT_ERROR;
    }

    struct sigaction sa = { 0 };
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sa.sa_handler = on_hup;
    sigaction(SIGHUP,  &sa, NULL);

    rnt_store_t *store = store_open(cfg->store_path);
    if (!store) { log_error("store_open failed"); goto err_cfg; }

    rnt_index_t *index = index_open(cfg->index_path, RNT_IDX_WRITE);
    if (!index) { log_error("index_open failed"); goto err_store; }

    if (index_schema_check(index) < 0) {
        log_error("incompatible index schema version");
        goto err_index;
    }

    rnt_watcher_t *watcher = watcher_create(cfg);
    if (!watcher) { log_error("watcher_create failed"); goto err_index; }

    log_info("renatumd %d.%d.%d started, watching %zu paths",
             RENATUM_VERSION_MAJOR, RENATUM_VERSION_MINOR,
             RENATUM_VERSION_PATCH, cfg->n_watches);

    int rc = watcher_run(watcher);

    log_info("renatumd shutting down");

    watcher_destroy(watcher);
    index_close(index);
    store_close(store);
    config_free(cfg);
    log_close();
    return rc;

err_index: index_close(index);
err_store: store_close(store);
err_cfg:   config_free(cfg);
    return RNT_EXIT_ERROR;
}
