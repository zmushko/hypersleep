/*
 * timeparse.c — duration parser.
 *
 * Grammar (informal):
 *
 *     duration := "infinite" | "never" | "forever"
 *               | <unsigned-decimal> <suffix>
 *
 *     suffix   := "s" | "m" | "h" | "d" | "w" | "y"
 *
 * No fractional values. No compound expressions (no "1d12h"). The
 * config grammar doesn't need them and accepting them silently
 * would invite mistakes.
 */

#include "timeparse.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NS_PER_SECOND  1000000000ULL

static int unit_seconds(char suffix, uint64_t *out_secs)
{
    switch (suffix) {
        case 's': *out_secs = 1;                   return 0;
        case 'm': *out_secs = 60;                  return 0;
        case 'h': *out_secs = 60 * 60;             return 0;
        case 'd': *out_secs = 24 * 60 * 60;        return 0;
        case 'w': *out_secs = 7 * 24 * 60 * 60;    return 0;
        case 'y': *out_secs = 365 * 24 * 60 * 60;  return 0;
    }
    return -1;
}

int parse_duration_ns(const char *str, uint64_t *out)
{
    if (str == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (strcasecmp(str, "infinite") == 0
        || strcasecmp(str, "never") == 0
        || strcasecmp(str, "forever") == 0) {
        *out = 0;
        return 0;
    }

    /* strtoull accepts both leading whitespace and a leading minus
     * (it wraps the result around unsigned, optionally with ERANGE).
     * Reject both upfront so the error is reported as EINVAL — the
     * actual problem is malformed input, not a numeric overflow. */
    if (*str == '\0' || *str == ' ' || *str == '\t' || *str == '-') {
        errno = EINVAL;
        return -1;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long n = strtoull(str, &end, 10);
    if (errno == ERANGE) {
        return -1;
    }
    if (end == str || end[0] == '\0' || end[1] != '\0') {
        /* no digits, or no suffix, or trailing garbage after suffix */
        errno = EINVAL;
        return -1;
    }

    uint64_t unit_s = 0;
    if (unit_seconds(end[0], &unit_s) < 0) {
        errno = EINVAL;
        return -1;
    }

    /* Guard the two multiplications: n * unit_s * NS_PER_SECOND. */
    if (n > UINT64_MAX / unit_s) {
        errno = ERANGE;
        return -1;
    }
    uint64_t secs = (uint64_t)n * unit_s;
    if (secs > UINT64_MAX / NS_PER_SECOND) {
        errno = ERANGE;
        return -1;
    }
    *out = secs * NS_PER_SECOND;
    return 0;
}
