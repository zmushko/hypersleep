# Hypersleep — Project Brief

> **Read this first.** This document captures the design decisions and
> rationale for Hypersleep. If you (or an AI assistant such as Claude Code)
> are picking up this project, read this before touching code. It will
> save hours of re-deriving conclusions.

## Origin story

Hypersleep was born from two starting points:

1. **librnotify** — a recursive inotify wrapper in pure C, written by the
   same author for a NETGEAR NAS commercial project around 2013–2014.
   Production-tested on large file trees with concurrent client access.
   Its killer feature: race-free recursive watching, which most public
   inotify wrappers (including `inotifywait -r`) get wrong.

2. **A personal frustration** — every time the author needed to recover a
   lost file (about 5 times in 30+ years of programming), the existing
   options were painful: digging through `cron`+`rsync` scripts, hoping
   the periodic backup ran recently enough, or grepping shell history.
   The thought of writing yet another `inotifywait | rsync` script "the
   right way" was itself off-putting.

Hypersleep is the answer: a small, native daemon that continuously captures
file changes the moment they happen, with a clean CLI for finding and
restoring past versions.

## What problem Hypersleep solves

Linux has many backup tools. None of them does exactly this combination:

```
   continuous (event-driven)
            ∩
        Linux native
            ∩
       pure C (lightweight, no Python/Go/Rust runtime)
            ∩
   embedded-ready (Pi 3A+, OpenWrt, small NAS)
            ∩
   versioned history (not just replication)
            ∩
   content-addressable (free deduplication)
            ∩
   race-free recursive inotify
            ∩
         FOSS, not enterprise
```

Each property exists in some tool. The intersection does not — until Hypersleep.

## Positioning vs neighbours

Hypersleep is **not a competitor** to `restic`, `borg`, `kopia`, or
`duplicati`. They do scheduled, encrypted, deduplicated, off-site backups.
Hypersleep protects the **gap between** their runs — the file you deleted at
14:32 when the next `borg create` is scheduled at 18:00.

Recommended deployment pattern:

- **Hypersleep** — continuous local versioning, single machine
- **Borg/Restic** — periodic encrypted off-site (S3, remote SSH)
- **Btrfs/ZFS snapshots** — atomic mount-point snapshots for system rollback
- **Syncthing** — cross-machine file synchronization (not backup)

These layers are complementary, not redundant.

## Architectural decisions and rationale

### Decision 1: pure C

**Rationale:** the author's primary language; embedded-friendly (Pi, OpenWrt);
no runtime to install; smallest possible binary; predictable memory use;
fastest startup.

**Trade-off:** more code to write than a Go or Rust implementation. Worth
it for embedded deployment.

### Decision 2: librnotify as the inotify layer

**Rationale:** already exists, production-tested in NETGEAR NAS, solves the
recursive race condition that other wrappers ignore. Linking it via
git submodule keeps Hypersleep's repo light while allowing coordinated
development.

**Key feature being leveraged:** when a new directory is created during
runtime, librnotify performs `inotify_add_watch()` + `readdir()` + emits
synthetic events for files that appeared between the `mkdir(2)` and our
watch installation. This closes a race that `inotifywait -r` and most
language bindings (older fsnotify, naive pyinotify) leave open.

**Known trade-off:** the readdir-after-add-watch approach can produce
**duplicate `IN_CREATE` events** when both the directory and its children
were already in the inotify queue. Hypersleep handles this on the consumer
side (see Decision 5).

### Decision 3: LMDB for the index

**Rationale:**
- Sub-millisecond lookups via mmap, zero-copy reads
- Crash-safe out of the box (no journal corruption like with SQLite WAL gotchas)
- No SQL, no schema migrations, no query planner surprises
- Single file + lock file on disk — simple to back up the index itself
- Mature, BSD-licensed, used by OpenLDAP and many others
- Small (~30KB compiled), embedded-friendly

**Alternative considered:** SQLite. Rejected because:
- Index workload is pure key-value (no joins, no ad-hoc queries)
- Indexing schema simpler in LMDB (just compose keys yourself)
- LMDB's concurrent readers + single writer model fits the daemon+CLI access pattern perfectly

**Alternative considered:** append-only journal. Rejected for v1 because
range queries ("all versions of path X between time A and B") become
expensive without an index. Could be revisited if LMDB proves a maintenance
burden.

**Schema design (LMDB sub-databases):**

```
DB: files          key: path | be_u64(captured_ts) | be_u32(seq)
                   value: { sha256[32], size, mtime, mode, uid, gid, event_mask }

DB: by_sha         key: sha256[32]
                   value: refcount (u32)
                   # for garbage collection of unreferenced blobs

DB: deletions      key: path | be_u64(deleted_ts)
                   value: { uid_of_deleter, last_known_sha256 }

DB: moves          key: be_u32(cookie)
                   value: { from_path, to_path, ts }

DB: meta           key: arbitrary config keys
                   value: arbitrary (schema version, etc.)
```

Path is stored as a variable-length prefix in the key, allowing efficient
range scans (`hypersleep log /some/path` becomes `mdb_cursor_get(MDB_SET_RANGE)`
followed by sequential reads until the path prefix changes).

### Decision 4: content-addressable storage on the filesystem

**Rationale:** simplest possible storage layer. Every captured blob lives at
`/var/lib/hypersleep/store/<sha[0:2]>/<sha[2:]>` (raw bytes or zstd-compressed).
Free deduplication: writing the same content twice is a no-op (already
exists). Easy to verify (recompute SHA, compare to path). Easy to GC
(walk LMDB by_sha refcounts, delete unreferenced).

**Metadata** (mode, mtime, uid, gid, xattrs) stays in LMDB, not next to the
blob. Two files with identical content but different permissions share
one blob, each with its own metadata entry.

**Not used:**
- Restic-style content-defined chunking. Adds complexity; for the
  expected workload (mostly small text files + occasional images/binaries),
  whole-file storage with optional zstd is sufficient. Can be added in v2.
- Encryption at rest. Defer to filesystem-level (`cryptsetup`/LUKS) or
  add in v2.

### Decision 5: idempotency via SHA, not via event deduplication

**Rationale:** librnotify can deliver duplicate events for legitimate
reasons (race-condition closing) or for noise reasons (atomic-save patterns
like vim's `swap → rename`). Trying to deduplicate at the event layer is
fragile and policy-heavy.

Instead, Hypersleep makes the **storage layer idempotent**:

1. On any candidate event (`IN_CLOSE_WRITE`, `IN_MOVED_TO`), compute the
   target file's SHA-256.
2. Look up `(path, sha256)` in LMDB. If present, no-op.
3. If absent, insert the index entry and write the blob to CAS.

A cheap pre-check (compare `(mtime, size)` to the latest known version)
skips the SHA computation for obvious duplicate events without risking
correctness for real changes.

This is the same pattern git uses. It just works.

### Decision 6: event debouncing

**Rationale:** atomic-save patterns (vim: write tmp → rename) and bulk
operations (rsync, cp -r) generate bursts of events on the same file or
directory. Snapshotting every intermediate state wastes I/O and pollutes
history with garbage versions.

**Solution:** a per-path debouncer with a small window (1–2 seconds). When
an event arrives, the path is queued with a timer. New events on the same
path reset the timer. When the timer fires, the *current* state of the
file is snapshotted once.

This means Hypersleep captures **stable states**, not transient ones.

### Decision 7: pre-snapshot before destructive restore

**Rationale:** `hypersleep wake --force` overwrites a file in place. If the
current on-disk state has not yet been captured (e.g., daemon was stopped),
that state would be lost forever.

**Solution:** before any `--force` restore, Hypersleep first captures the
current state synchronously (if it differs from the latest known version),
then performs the restore. The user's "previous current state" is always
recoverable as the most recent version in history.

Flag `--no-pre-snapshot` is available for users who know what they're doing.

### Decision 8: stdout-by-default for `show`

**Rationale:** `hypersleep show <path> v5` writes to **stdout**, not back to
the original path. This is the same convention as `git show`, `cat`, and
all read-side Unix tools. It enables:

```bash
hypersleep show foo.c v5 | less
hypersleep show foo.c v5 | diff foo.c -
hypersleep show foo.c v5 > /tmp/foo.c.bak
```

To actually restore in place, the user types `hypersleep wake` — a
different, explicitly destructive verb.

### Decision 9: human-readable version IDs (v1, v2, ...) externally

**Rationale:** SHAs are correct but unfriendly. `v3` is friendly. Internally
everything is SHA-keyed; the CLI translates `v3` → captured-timestamp-rank
on lookup.

**Stability concern:** if version `v3` is pruned by GC, do remaining versions
renumber? Decision: **yes, version numbers are display-only and always
counted from the oldest currently-stored version.** Users referring to a
specific snapshot for the long term should use timestamps (`--at "2026-05-17
14:30"`) or `--by-sha`.

## Critical safety properties

These must be preserved by all code:

1. **AT_NOFOLLOW everywhere.** Use `lstat`, `openat` with `O_NOFOLLOW`,
   `unlinkat` with `AT_SYMLINK_NOFOLLOW`. An attacker with write access to
   a watched directory must not be able to make Hypersleep snapshot
   `/etc/shadow` via a planted symlink.

2. **No silent data loss on storage exhaustion.** If `/var/lib/hypersleep/`
   fills up, the daemon must **log loudly** and continue accepting events
   (queue them, drop synthetic snapshots gracefully) — not crash. A
   backup daemon that dies when the disk fills is worse than no daemon.

3. **fsync discipline.** New blobs: `fsync(blob_fd)` + `fsync(dir_fd)`
   before considering the write durable. LMDB's `MDB_NOSYNC` is forbidden
   for the index. Power-loss must leave the store in a valid state.

4. **`IN_Q_OVERFLOW` must trigger a rescan.** If the inotify queue
   overflows, events are lost. Hypersleep responds by walking each watched
   subtree and synthesizing events for any files whose `(path, mtime,
   size)` does not match the latest LMDB entry. Log the incident.

5. **`max_user_watches` graceful degradation.** On `inotify_add_watch()`
   returning `ENOSPC`, log a clear warning recommending
   `sysctl fs.inotify.max_user_watches=524288` (or higher) and continue
   with watches successfully installed so far. Do not crash.

## Out of scope (for now)

- Encryption at rest (use LUKS or add in v2)
- Remote storage (use Borg/Restic for off-site; Hypersleep is local)
- Content-defined chunking (whole-file is fine for v1)
- Cross-machine sync (use Syncthing)
- GUI (Unix-way; third parties can write wrappers)
- Compression beyond optional zstd-per-blob in v1.1
- Plugin system (premature)

## CLI summary

Full spec in [cli-spec.md](cli-spec.md). One-line summaries:

| Command | Effect |
|---|---|
| `hypersleep status` | daemon health, watched paths, store size |
| `hypersleep log <path>` | list versions of a file |
| `hypersleep show <path> v<N>` | print version contents to stdout |
| `hypersleep diff <path> v<A> v<B>` | textual diff between versions |
| `hypersleep wake <path> v<N> --to <new>` | safe restore to a new location |
| `hypersleep wake <path> v<N> --force` | destructive in-place restore (with pre-snapshot) |
| `hypersleep wake-tree <dir> --at <time>` | restore a whole subtree as it was |
| `hypersleep recover <path>` | interactive recovery mode |
| `hypersleep find --name PATTERN` | search the history |
| `hypersleep purge --older-than 30d` | garbage-collect old versions |
| `hypersleep verify` | re-hash all blobs, check store integrity |

## Repository layout

```
hypersleep/
├── README.md
├── LICENSE                       (Apache-2.0)
├── Makefile
├── CHANGELOG.md
│
├── docs/
│   ├── project-brief.md          (this file)
│   ├── architecture.md
│   ├── cli-spec.md
│   ├── config.md
│   └── recovery.md
│
├── include/
│   ├── hypersleep.h
│   ├── store.h
│   ├── index.h
│   ├── watcher.h
│   ├── snapshot.h
│   ├── debounce.h
│   ├── config.h
│   └── log.h
│
├── src/
│   ├── main.c                    (hypersleepd entry)
│   ├── cli.c                     (hypersleep CLI entry)
│   ├── watcher.c                 (librnotify subscription)
│   ├── debounce.c
│   ├── snapshot.c                (file → CAS pipeline)
│   ├── restore.c                 (CAS → file pipeline)
│   ├── store.c                   (CAS storage on disk)
│   ├── index.c                   (LMDB operations)
│   ├── retention.c               (prune policies)
│   ├── config.c                  (parser for hypersleep.conf)
│   └── log.c
│
├── tests/
│   ├── test_store.c
│   ├── test_index.c
│   ├── test_e2e.sh
│   └── stress/
│       ├── deep_mkdir.sh
│       ├── atomic_drop.sh
│       └── parallel_writes.sh
│
├── etc/
│   └── hypersleep.conf.example
│
├── systemd/
│   ├── hypersleepd.service
│   └── hypersleep-prune.timer
│
└── third_party/
    └── librnotify/               (git submodule)
```

## Build dependencies

Minimum:
- C11 compiler (gcc 7+ or clang 6+)
- `libcrypto` (OpenSSL or LibreSSL) — for SHA-256
- `liblmdb` (Debian/Ubuntu: `liblmdb-dev`)
- `make`, `pkg-config`

Optional:
- `libzstd-dev` — for blob compression (build with `make WITH_ZSTD=1`)

Runtime: Linux ≥ 4.11 (for `statx`-style birth-time, optional); kernel
inotify support (any modern kernel).

## Notes for future Claude (or future Andrey)

The author lives in Turkey (Manavgat area), is Belarusian by origin,
prefers vi over nano, writes in C, deploys on Raspberry Pi class
hardware, and values directness over flattery in technical discussion.

When proposing further changes, prefer:
- depth over breadth (one solid feature beats five half-done ones)
- production hardening over surface API expansion
- documentation that explains *why*, not just *how*

The author is willing to push back on AI suggestions. This is a feature,
not a bug. Respond substantively, not defensively.
