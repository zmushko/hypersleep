/*
 * hypersleep.h — public types shared between daemon and CLI
 *
 * Apache-2.0
 */

#ifndef HYPERSLEEP_H
#define HYPERSLEEP_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

#define HYPERSLEEP_VERSION_MAJOR 0
#define HYPERSLEEP_VERSION_MINOR 1
#define HYPERSLEEP_VERSION_PATCH 0
#define HYPERSLEEP_SCHEMA_VERSION 1

/* SHA-256 digest size */
#define HS_SHA_LEN 32

/* Exit codes (see docs/cli-spec.md) */
enum hs_exit {
    HS_EXIT_OK        = 0,
    HS_EXIT_ERROR     = 1,
    HS_EXIT_USAGE     = 2,
    HS_EXIT_NOTFOUND  = 3,
    HS_EXIT_EXISTS    = 4,
    HS_EXIT_PERM      = 5,
    HS_EXIT_CORRUPT   = 6,
    HS_EXIT_NOSPACE   = 7,
    HS_EXIT_CANCELLED = 8,
};

/* On-disk packed record for the `files` LMDB sub-DB */
typedef struct __attribute__((packed)) hs_file_entry {
    uint8_t  sha256[HS_SHA_LEN];
    uint64_t size;
    int64_t  mtime_sec;
    int32_t  mtime_nsec;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t event_mask;
    uint16_t flags;
    uint16_t reserved;
} hs_file_entry_t;

/* Flag bits for hs_file_entry.flags */
enum hs_flags {
    HS_FLAG_COMPRESSED = 1 << 0,    /* blob is zstd-framed */
    HS_FLAG_SYNTHETIC  = 1 << 1,    /* captured during overflow rescan */
    HS_FLAG_PRESNAPSHOT = 1 << 2,   /* captured by --force pre-snapshot */
};

/* Deletion marker */
typedef struct __attribute__((packed)) hs_deletion {
    uint32_t uid;
    uint8_t  last_sha256[HS_SHA_LEN];
    uint64_t last_size;
} hs_deletion_t;

/* Move record (transient, indexed by inotify cookie) */
typedef struct hs_move {
    uint64_t ts_ns;
    char     from_path[];  /* null-terminated; record is varlen */
} hs_move_t;

/* Logical version identifier for CLI display.
 * Internally everything is keyed by SHA + capture timestamp; vN is
 * computed at query time as the rank among non-pruned versions for a
 * given path. */
typedef struct hs_version {
    int      num;              /* v1, v2, ... — display only */
    uint64_t captured_ns;
    uint8_t  sha256[HS_SHA_LEN];
    hs_file_entry_t entry;
} hs_version_t;

#endif /* HYPERSLEEP_H */
