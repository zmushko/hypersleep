/*
 * log.c — process-wide logging shim with three backends.
 *
 *   stderr   — foreground (CLI tools, `hypersleepd -f`); the default
 *              when log_init has not been called yet.
 *   file     — when the daemon has `log <path>` in hypersleep.conf.
 *   syslog   — daemon fallback when no log path is configured (or
 *              the configured file cannot be opened).
 *
 * Format for stderr/file:
 *
 *     2026-05-18T14:32:01.234Z [INFO ] message
 *
 * Timestamps are UTC to avoid surprises across timezone changes.
 * syslog gets the raw formatted message; the daemon facility and
 * priority encode the level for journald/rsyslog filtering.
 *
 * Thread-safety: the daemon is single-threaded for v0.1.0; this
 * module is not protected by a mutex. log_msg is *not* async-signal
 * safe — stdio fprintf and syslog both touch locks.
 */

#include "log.h"
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

enum log_backend {
    LOG_BACKEND_STDERR,
    LOG_BACKEND_FILE,
    LOG_BACKEND_SYSLOG,
};

/* Zero-initialised: backend=STDERR, level=DEBUG, file=NULL,
 * syslog_open=false. So log_msg works before log_init is called —
 * it falls back to stderr at the most verbose level, which is the
 * right behaviour for the bootstrap window (argv parsing, config
 * load) where we have nothing better. */
static struct {
    enum log_backend     backend;
    enum hs_log_level   level;
    FILE                *file;
    bool                 syslog_open;
} g;

static const char *level_name(enum hs_log_level lvl)
{
    switch (lvl) {
        case HS_LOG_DEBUG: return "DEBUG";
        case HS_LOG_INFO:  return "INFO ";
        case HS_LOG_WARN:  return "WARN ";
        case HS_LOG_ERROR: return "ERROR";
    }
    return "?    ";
}

static int level_to_syslog(enum hs_log_level lvl)
{
    switch (lvl) {
        case HS_LOG_DEBUG: return LOG_DEBUG;
        case HS_LOG_INFO:  return LOG_INFO;
        case HS_LOG_WARN:  return LOG_WARNING;
        case HS_LOG_ERROR: return LOG_ERR;
    }
    return LOG_INFO;
}

void log_init(const hs_config_t *cfg, bool foreground)
{
    g.level = cfg ? cfg->log_level : HS_LOG_INFO;

    if (foreground) {
        g.backend = LOG_BACKEND_STDERR;
        g.file = stderr;
        return;
    }

    /* Daemon mode. Prefer the configured file; fall back to syslog
     * if there's no path or we can't open it. We do NOT silently
     * drop logs — syslog is always reachable. */
    if (cfg && cfg->log_path) {
        /* O_NOFOLLOW so a symlink at the configured log path (e.g.
         * a misconfiguration or a deliberate plant) does not steer
         * append-writes into a victim file like /etc/passwd. */
        int fd = open(cfg->log_path,
                      O_WRONLY | O_CREAT | O_APPEND
                      | O_NOFOLLOW | O_CLOEXEC,
                      0640);
        FILE *f = (fd >= 0) ? fdopen(fd, "a") : NULL;
        if (f == NULL && fd >= 0) close(fd);
        if (f) {
            setvbuf(f, NULL, _IOLBF, 0);
            g.backend = LOG_BACKEND_FILE;
            g.file = f;
            return;
        }
        /* fall through to syslog so the file-open failure itself is
         * still recorded somewhere reachable */
    }

    openlog("hypersleepd", LOG_PID | LOG_CONS, LOG_DAEMON);
    g.backend = LOG_BACKEND_SYSLOG;
    g.syslog_open = true;

    if (cfg && cfg->log_path) {
        /* re-emit the file failure once the syslog backend is up */
        syslog(LOG_WARNING,
               "could not open log file %s: %s; logging to syslog instead",
               cfg->log_path, strerror(errno));
    }
}

void log_close(void)
{
    if (g.backend == LOG_BACKEND_FILE && g.file && g.file != stderr) {
        fclose(g.file);
    }
    g.file = NULL;

    if (g.syslog_open) {
        closelog();
        g.syslog_open = false;
    }

    /* Reset to bootstrap defaults so any further log_msg call (e.g.
     * during atexit handlers) still has stderr to write to. */
    g.backend = LOG_BACKEND_STDERR;
}

void log_msg(enum hs_log_level lvl, const char *fmt, ...)
{
    if (lvl < g.level) {
        return;
    }

    int saved_errno = errno;

    va_list ap;
    va_start(ap, fmt);

    if (g.backend == LOG_BACKEND_SYSLOG) {
        vsyslog(level_to_syslog(lvl), fmt, ap);
        va_end(ap);
        errno = saved_errno;
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmval;
    gmtime_r(&ts.tv_sec, &tmval);
    char tsbuf[24];
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S", &tmval);

    FILE *out = g.file ? g.file : stderr;
    flockfile(out);
    fprintf(out, "%s.%03ldZ [%s] ",
            tsbuf, ts.tv_nsec / 1000000, level_name(lvl));
    vfprintf(out, fmt, ap);
    fputc('\n', out);
    funlockfile(out);

    va_end(ap);
    errno = saved_errno;
}
