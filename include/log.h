/*
 * log.h — minimal logging shim
 *
 * Backends:
 *   - stderr (default for foreground / CLI)
 *   - syslog (for daemon when stderr is closed)
 *   - file (when configured)
 */

#ifndef HYPERSLEEP_LOG_H
#define HYPERSLEEP_LOG_H

#include "config.h"
#include <stdarg.h>

void log_init(const hs_config_t *cfg, bool foreground);
void log_close(void);

/* Override the minimum log level set during log_init. The CLI uses
 * this to lift the floor from INFO (daemon-friendly) to WARN so
 * successful operator commands do not decorate stdout/stderr with
 * timestamped daemon-style chatter. */
void log_set_level(enum hs_log_level lvl);

void log_msg(enum hs_log_level lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define log_debug(...) log_msg(HS_LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_msg(HS_LOG_INFO,  __VA_ARGS__)
#define log_warn(...)  log_msg(HS_LOG_WARN,  __VA_ARGS__)
#define log_error(...) log_msg(HS_LOG_ERROR, __VA_ARGS__)

#endif /* HYPERSLEEP_LOG_H */
