/*
 * timeparse.h — parse human-friendly time expressions into machine units.
 *
 * Currently covers duration strings used by the retention config and
 * the prune CLI. Absolute timestamp parsing for `renatum show --at`
 * will live here too when that command lands.
 */

#ifndef RENATUM_TIMEPARSE_H
#define RENATUM_TIMEPARSE_H

#include <stdint.h>

/*
 * Parse a duration like "30d", "12h", "90d", "5m", "1y" into
 * nanoseconds. Suffixes:
 *
 *   s  seconds
 *   m  minutes
 *   h  hours
 *   d  days        (24 h)
 *   w  weeks       (7 d)
 *   y  years       (365 d — no leap-year handling; this is a
 *                  retention horizon, not a calendar function)
 *
 * The strings "infinite", "never", and "forever" map to *out = 0,
 * which Renatum-wide means "do not auto-prune".
 *
 * Returns 0 on success and stores the result in *out. Returns -1 on
 * an unparseable input or numeric overflow (errno set to EINVAL or
 * ERANGE). *out is left untouched on error.
 */
int parse_duration_ns(const char *str, uint64_t *out);

#endif /* RENATUM_TIMEPARSE_H */
