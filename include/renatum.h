/*
 * renatum.h — public types shared between daemon and CLI
 *
 * Apache-2.0
 */

#ifndef RENATUM_H
#define RENATUM_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

#define RENATUM_VERSION_MAJOR 0
#define RENATUM_VERSION_MINOR 1
#define RENATUM_VERSION_PATCH 0
#define RENATUM_SCHEMA_VERSION 1

/* SHA-256 digest size */
#define RNT_SHA_LEN 32

/* Exit codes (see docs/cli-spec.md) */
enum rnt_exit {
    RNT_EXIT_OK        = 0,
    RNT_EXIT_ERROR     = 1,
    RNT_EXIT_USAGE     = 2,
    RNT_EXIT_NOTFOUND  = 3,
    RNT_EXIT_EXISTS    = 4,
    RNT_EXIT_PERM      = 5,
    RNT_EXIT_CORRUPT   = 6,
    RNT_EXIT_NOSPACE   = 7,
    RNT_EXIT_CANCELLED = 8,
};

/* On-disk packed record for the `files` LMDB sub-DB */
typedef struct __attribute__((packed)) rnt_file_entry {
    uint8_t  sha256[RNT_SHA_LEN];
    uint64_t size;
    int64_t  mtime_sec;
    int32_t  mtime_nsec;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t event_mask;
    uint16_t flags;
    uint16_t reserved;
} rnt_file_entry_t;

/* Flag bits for rnt_file_entry.flags */
enum rnt_flags {
    RNT_FLAG_COMPRESSED = 1 << 0,    /* blob is zstd-framed */
    RNT_FLAG_SYNTHETIC  = 1 << 1,    /* captured during overflow rescan */
    RNT_FLAG_PRESNAPSHOT = 1 << 2,   /* captured by --force pre-snapshot */
};

/* Deletion marker */
typedef struct __attribute__((packed)) rnt_deletion {
    uint32_t uid;
    uint8_t  last_sha256[RNT_SHA_LEN];
    uint64_t last_size;
} rnt_deletion_t;

/* Move record (transient, indexed by inotify cookie) */
typedef struct rnt_move {
    uint64_t ts_ns;
    char     from_path[];  /* null-terminated; record is varlen */
} rnt_move_t;

/* Logical version identifier for CLI display.
 * Internally everything is keyed by SHA + capture timestamp; vN is
 * computed at query time as the rank among non-pruned versions for a
 * given path. */
typedef struct rnt_version {
    int      num;              /* v1, v2, ... — display only */
    uint64_t captured_ns;
    uint8_t  sha256[RNT_SHA_LEN];
    rnt_file_entry_t entry;
} rnt_version_t;

#endif /* RENATUM_H */
