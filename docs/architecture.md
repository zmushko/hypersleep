# Renatum Architecture

## High-level dataflow

```
┌────────────────┐    inotify events    ┌────────────────┐
│   Filesystem   │ ───────────────────→ │   librnotify   │
└────────────────┘                       └───────┬────────┘
                                                 │ waitNotify()
                                                 ▼
                                         ┌────────────────┐
                                         │    watcher.c   │
                                         │  (event pump)  │
                                         └───────┬────────┘
                                                 │ raw events
                                                 ▼
                                         ┌────────────────┐
                                         │   debounce.c   │
                                         │  (1-2s window) │
                                         └───────┬────────┘
                                                 │ stable events
                                                 ▼
                                         ┌────────────────┐
                                         │   snapshot.c   │
                                         │ (SHA + capture)│
                                         └─────┬──────┬───┘
                                               │      │
                              ┌────────────────┘      └────────────────┐
                              ▼                                         ▼
                      ┌────────────────┐                         ┌────────────────┐
                      │    store.c     │                         │    index.c     │
                      │   (CAS blobs)  │                         │     (LMDB)     │
                      └────────────────┘                         └────────────────┘
                              ▲                                         ▲
                              │                                         │
                              └──────────────┬──────────────────────────┘
                                             │
                                     ┌───────┴────────┐
                                     │    cli.c       │
                                     │  (renatum CLI) │
                                     └────────────────┘
```

## Processes

Two binaries share one storage:

1. **renatumd** — long-running daemon. Owns the librnotify subscription,
   debouncer, and a writer LMDB txn. Started by systemd.

2. **renatum** — CLI tool. Opens the LMDB environment read-only (or with
   short write txns for restore operations that update metadata).
   Communicates with the daemon only through the filesystem (LMDB and CAS).

There is **no IPC** between CLI and daemon for v1. LMDB's multi-reader /
single-writer model handles concurrent access. The daemon holds an
advisory flock on `/var/lib/renatum/lock` to prevent two daemons running
simultaneously.

## Storage layout on disk

```
/var/lib/renatum/
├── store/                                # CAS blobs
│   ├── a3/
│   │   ├── f291b4...e9c2                 # blob: raw bytes or zstd frame
│   │   └── ...
│   ├── 8c/
│   └── ...
├── index/                                # LMDB environment
│   ├── data.mdb
│   └── lock.mdb
├── lock                                  # daemon flock target
└── log                                   # rotated text log

/etc/renatum/
└── renatum.conf

/var/log/renatum/                         # if rsyslog/journald not used
└── renatumd.log
```

## LMDB schema

LMDB is a flat key-value store; we use named sub-databases.

### `files` sub-DB

Key (variable length, lexicographically sortable):
```
<path-bytes> 0x00 <captured_ts_be_u64> <seq_be_u32>
```

- `path-bytes` — absolute path, no terminator
- `0x00` — separator
- `captured_ts_be_u64` — big-endian timestamp (ns or s), monotonically
  comparable
- `seq_be_u32` — sequence number to break ties within the same timestamp

Value (packed struct):
```c
struct file_entry_v1 {
    uint8_t  sha256[32];
    uint64_t size;
    int64_t  mtime_sec;
    int32_t  mtime_nsec;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t event_mask;        // IN_CLOSE_WRITE, IN_MOVED_TO, ...
    uint16_t flags;             // bits: COMPRESSED, SYNTHETIC, ...
    uint16_t reserved;
};
```

**Why this key design:** ranges like `renatum log /home/andrey/foo.c` map
to `MDB_SET_RANGE` on the path prefix, then sequential `MDB_NEXT` while
the path matches. O(log N) seek + O(k) scan where k is the number of
versions.

### `by_sha` sub-DB

Key: `sha256[32]`
Value: `uint32_t refcount`

Used by garbage collector. Incremented on every new `files` entry that
references this SHA; decremented when entries are pruned. Blob is deleted
from disk when refcount reaches zero.

### `deletions` sub-DB

Key: `<path-bytes> 0x00 <deleted_ts_be_u64>`
Value:
```c
struct deletion_v1 {
    uint32_t uid;               // who deleted (from /proc, best-effort)
    uint8_t  last_sha256[32];   // SHA of last known content
    uint64_t last_size;
};
```

Records `IN_DELETE` / `IN_DELETE_SELF` events. Used by `renatum recover`
to surface deleted files.

### `moves` sub-DB

Key: `<cookie_u32_be>` (transient)
Value:
```c
struct move_v1 {
    uint64_t ts_ns;
    uint8_t  from_path[];       // null-terminated; full record is varlen
};
```

inotify `IN_MOVED_FROM` writes here; `IN_MOVED_TO` reads and consumes (or
times out after a few seconds and produces a "deleted" record).

### `meta` sub-DB

Free-form configuration:
- `schema_version` — currently `1`
- `daemon_started` — timestamp of last daemon start
- `last_overflow_at` — timestamp of last `IN_Q_OVERFLOW` rescan

## Event handling pipeline

### Step 1: raw event reception (watcher.c)

```c
char *path = NULL;
uint32_t mask = 0, cookie = 0;
int rc = waitNotify(ntf, &path, &mask, /*timeout_ms*/ 500, &cookie);
if (rc == 0) {
    debounce_push(deb, path, mask, cookie);
    free(path);
} else if (rc > 0) {
    // timeout — natural flush point for the debouncer
    debounce_tick(deb);
}
```

Watcher does no I/O on the file itself. Hands events off as fast as possible.

### Step 2: debouncing (debounce.c)

A hash table keyed by `path` stores pending events. Each entry tracks:
- last event timestamp
- accumulated mask (ORed over the window)
- last cookie (for move tracking)

A monotonic timer fires every 100 ms; entries older than the debounce
window (default 1500 ms) are flushed to the snapshot stage.

Key behaviour: bursts of `IN_MODIFY` followed by `IN_CLOSE_WRITE` collapse
to a single `IN_CLOSE_WRITE` snapshot. Vim's tmp-then-rename pattern
collapses to a single capture of the final file.

### Step 3: snapshot decision (snapshot.c)

For each flushed event:

```c
void snapshot_handle(const char *path, uint32_t mask, uint32_t cookie) {
    // Filter: deletes go straight to deletions sub-DB
    if (mask & (IN_DELETE | IN_DELETE_SELF)) {
        index_record_deletion(path, ...);
        return;
    }
    if (mask & IN_MOVED_FROM) {
        moves_pending_from(cookie, path);
        return;
    }
    if (mask & IN_MOVED_TO) {
        moves_resolve_to(cookie, path);
        // Fall through — capture the file at the new path
    }
    if (!(mask & (IN_CLOSE_WRITE | IN_MOVED_TO))) return;

    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (!S_ISREG(st.st_mode)) return;

    // Cheap pre-check: same mtime + size as latest version?
    struct file_entry latest;
    if (index_get_latest(path, &latest) == 0
        && latest.size == (uint64_t)st.st_size
        && latest.mtime_sec == st.st_mtim.tv_sec
        && latest.mtime_nsec == st.st_mtim.tv_nsec) {
        return;  // event-duplicate, no real change
    }

    // Compute SHA and snapshot
    uint8_t sha[32];
    if (sha256_file(path, sha) != 0) return;

    // Same SHA as a previous version of this path? (touch without edit)
    if (index_has_path_sha(path, sha)) return;

    // Write blob (no-op if already present at that SHA)
    if (store_put(sha, path) != 0) return;

    // Add index entry
    index_insert(path, sha, &st, mask, /*captured*/ now_ns());
}
```

### Step 4: restore path (restore.c)

```
CLI parses --at / v<N> → look up file_entry in LMDB
                       → fetch blob from CAS by SHA
                       → if --force on existing file:
                            pre-snapshot current state
                       → write to tmpfile in same dir
                       → fsync tmpfile, fsync dir
                       → rename(tmpfile, target)  (atomic)
                       → apply metadata (mode, mtime, uid, gid)
```

## Recovery from overflow / corruption

### `IN_Q_OVERFLOW` rescan

When the inotify queue overflows, an `IN_Q_OVERFLOW` event arrives with
`wd = -1`. The watcher signals snapshot.c to begin a full rescan:

1. For each configured watch path, walk the tree (`nftw(3)` or manual).
2. For each regular file found, perform the same `(mtime, size)` pre-check
   against LMDB's latest entry for that path.
3. If mismatch, compute SHA and capture as if it were a `IN_CLOSE_WRITE`.

This is the **fallback** that ensures Renatum never permanently loses an
event. Rescan logs how many files were captured as a result.

### `renatum verify`

Walks every blob in the CAS and recomputes its SHA. Mismatches indicate
corruption. By default, a corrupted blob is left in place but logged; with
`--repair`, the blob is renamed to `.corrupt-<timestamp>` and removed from
`by_sha`, making the dependent `files` entries dangling (still listed in
`renatum log`, but `restore` will fail with a clear message).

### `renatum gc`

Walks `by_sha` sub-DB. For each SHA with refcount=0, removes the blob from
disk. Optionally runs `lmdb_copy --compact` to shrink the index.

## Concurrency model

- **Single writer to LMDB:** only `renatumd` writes. The CLI opens
  read-only transactions for `log`, `show`, `diff`, `find`.
- **Exception:** `renatum restore --force` performs a pre-snapshot, which
  requires a write transaction. To avoid contention, the CLI sends a
  Unix domain socket message to the daemon ("please snapshot path X now")
  and waits for ack. The daemon snapshots in its writer txn. CLI then
  proceeds with the restore (which itself doesn't need writes — only
  blob reads + filesystem write).

This Unix-socket protocol is **the only IPC** between CLI and daemon, and
it is **only for synchronous pre-snapshot**. Everything else flows through
LMDB.

### Socket protocol (sketch)

`/run/renatum/control.sock` — Unix datagram socket.

Request:
```
SNAPSHOT <path>\n
```

Response:
```
OK <sha-hex>\n     # captured (or already up to date)
SKIP <reason>\n    # not a regular file, excluded, etc.
ERR <message>\n
```

If the daemon isn't running, the CLI continues without pre-snapshot but
logs a warning. The user can force the operation with
`--no-pre-snapshot`.

## Performance targets (Pi 3A+ class)

- Steady-state CPU: < 1% with a 10k-file watched tree, sporadic edits
- Per-snapshot latency: < 50 ms for files up to 1 MB
- LMDB lookup: < 1 ms for `renatum log <path>` on a 100k-entry index
- Memory footprint: < 20 MB RSS for daemon under steady state
- Cold-start scan of 100k files: < 30 seconds

These are targets, not guarantees. Stress tests in `tests/stress/` will
validate them.
