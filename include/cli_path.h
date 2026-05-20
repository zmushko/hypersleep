/*
 * cli_path.h — CLI-side path normalisation.
 *
 * The daemon indexes absolute paths (libnotify always reports
 * absolute paths under the watched root). The CLI accepts whatever
 * the user typed, including CWD-relative shorthand like ../foo.txt
 * or symlink chains. Without normalisation every relative
 * invocation misses the index and gets a misleading "no versions"
 * (or worse, "lookup failed" when downstream code stumbles on the
 * unresolved path).
 *
 * cli_resolve_path returns a freshly malloc'd absolute path or
 * NULL on error. NULL preserves errno from realpath(3) so the
 * caller can decide whether to abort or surface a friendlier
 * message.
 *
 * Header-only because the implementation is a one-liner that
 * does not justify another translation unit; defined `inline static`
 * so it stays internal to each .o that needs it.
 */

#ifndef HYPERSLEEP_CLI_PATH_H
#define HYPERSLEEP_CLI_PATH_H

#include <stdlib.h>

static inline char *cli_resolve_path(const char *in)
{
    if (in == NULL) return NULL;
    /* realpath with NULL second arg allocates a buffer of PATH_MAX
     * (glibc / musl POSIX 2008 extension). Caller frees. */
    return realpath(in, NULL);
}

#endif /* HYPERSLEEP_CLI_PATH_H */
