/*
 * restore.h — CAS blob -> filesystem extraction primitives.
 *
 * Shared between `hypersleep restore` (single file) and the upcoming
 * `hypersleep restore-tree` (subtree). The CLI argument plumbing
 * lives in cmd_restore.c / cmd_restore_tree.c; the actual write
 * mechanics live in restore.c so both subcommands share the
 * fsync discipline and metadata-application logic.
 */

#ifndef HYPERSLEEP_RESTORE_H
#define HYPERSLEEP_RESTORE_H

#include "hypersleep.h"
#include "store.h"

#include <stdbool.h>

/* Restore the version `v` into `dst`. The destination must not
 * already exist (use O_CREAT|O_EXCL). On success the file has the
 * mode/uid/gid/mtime from the captured metadata if `preserve_mode`
 * is true, or the process umask + identity otherwise.
 *
 * Atomic: writes to a temp file in the same directory, fsyncs it,
 * then renames into place; fsyncs the parent dir before returning
 * so a power-loss leaves either the old or the new file, never a
 * partial.
 *
 * Returns 0 on success, -1 on error (errno set). On error the
 * temp file is unlinked. */
int restore_to(hs_store_t *store,
               const hs_version_t *v,
               const char *dst,
               bool preserve_mode);

#endif /* HYPERSLEEP_RESTORE_H */
