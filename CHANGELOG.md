# Changelog

All notable changes to Hypersleep will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- **Project renamed from Renatum to Hypersleep.** Binaries
  (`hypersleepd`, `hypersleep`), config (`/etc/hypersleep/hypersleep.conf`),
  state (`/var/lib/hypersleep/`), logs (`/var/log/hypersleep/`),
  systemd unit (`hypersleepd.service`), public header
  (`include/hypersleep.h`), and identifier prefixes (`hs_`, `HS_`)
  all updated. The old `renatum` / `RNT_` names are gone; users
  upgrading from an installed Renatum will need to move state
  directories and reconfigure systemd.
- CLI verb `restore` renamed to `wake`; `prune` renamed to `purge`.
  Other verbs (`status`, `log`, `show`, `diff`, `find`, `verify`,
  `config`) are unchanged.

### TODO before v0.1.0 → v0.2.0

- Stress tests in `tests/stress/` (deep_mkdir, atomic_save,
  IN_Q_OVERFLOW recovery, symlink_no_follow, recursive_move —
  mirroring the librnotify suite at one level up).
- `hypersleep wake --force` CLI side: control-socket client,
  interactive confirmation, post-write integrity verify loop.
- `hypersleep find` — needs an index iterator over a path prefix
  (or the whole files sub-DB) before it can serve `--name`,
  `--grep`, `--deleted`, etc.
- `hypersleep wake-tree` and `hypersleep recover` (interactive).
- `hypersleep gc` as a standalone command.
- `hypersleep config reload` once main.c starts persisting the pid
  into the lock file.
- Compression: optional zstd-framed blobs (HS_FLAG_COMPRESSED
  already reserved in the schema).
- Time-based selectors for `show`/`diff`/`wake` (`--at`,
  `--before`, `--by-sha`).
- Audit pipeline: queue depth peak, last overflow ts, last GC
  written into meta and surfaced by `hypersleep status`.

## [0.1.0] — 2026-05-19

First implementable cut. The daemon captures content into a
content-addressable store with an LMDB index; the CLI lets an
operator inspect history, view a past version, diff between
versions, and restore one to a new path.

### Added

- `log.c` — three-backend logging (stderr / file / syslog) with
  UTC millisecond timestamps and a zero-init bootstrap mode so
  log calls work before log_init.
- `timeparse.c` — duration parser (`30d`, `12h`, `infinite`, …)
  with overflow guards.
- `config.c` — full hypersleep.conf parser with line continuation,
  quoted-value tokens, post-pass resolution of retention-default
  and compress-default (directive order no longer matters).
- `index.c` — LMDB-backed index with five sub-DBs (files, by_sha,
  deletions, moves, meta), big-endian timestamp keys for
  chronological cursor walks, schema versioning, exponential
  doubling for the path table.
- `store.c` — content-addressable storage on disk
  (`<root>/<sha[0:2]>/<sha[2:]>`); atomic put via mkstemp +
  rename + dir fsync; idempotent re-puts; OpenSSL EVP_sha256.
- `snapshot.c` — the file → CAS pipeline: lstat, (mtime, size)
  pre-check, store_put, (path, sha) idempotency, index_insert.
  Handles deletes, move-from/to pairing, IN_Q_OVERFLOW rescans
  via nftw(FTW_PHYS|FTW_MOUNT).
- `debounce.c` — per-path event coalescing on a monotonic timer.
- `watcher.c` — librnotify v3 multiplexer through one epoll;
  one Notify per cfg watch directive; full-path exclude regex
  applied post-receive (librnotify's own regex matches only
  the entry name and docs/config.md promises full-path).
- `control.c` — Unix-datagram control socket carrying
  `SNAPSHOT <path>` → `OK <hex>` / `ERR <msg>` for the
  pre-snapshot handshake.
- `restore.c` — atomic CAS-to-filesystem extraction with
  mkstemp + fchmod/fchown/futimens + rename + fsync(parent).
- `retention.c` — orphan-blob sweep after prune.
- CLI: `hypersleep status`, `hypersleep log`, `hypersleep show`,
  `hypersleep diff`, `hypersleep wake --to`, `hypersleep verify`,
  `hypersleep purge --older-than`, `hypersleep config show/test/paths`.
- IN_Q_OVERFLOW rescan in the watcher + snapshot pipeline.
- AT_NOFOLLOW audit pass — O_NOFOLLOW on every user- or
  config-supplied final path component (cmd_show --out, log
  destination, main.c lock file, restore.c parent directory).
- Third-party librnotify upgraded to v3.0.0 (single-path
  initNotify, notifyFd accessor, always-on IN_DONT_FOLLOW;
  20+ bugfix commits landed in the underlying repo).

### Known limitations

- `hypersleep wake --force` is parsed but refuses with a clear
  message; the CLI side of the control-socket dance is in the
  v0.2.0 plan.
- `hypersleep find` returns "not implemented" until the index
  grows a prefix iterator.
- `hypersleep config reload` does not actually send SIGHUP yet —
  main.c does not persist its pid into the lock file.
- Compression is deferred to v1.1 per the project brief; all
  blobs are stored raw.
- No stress test suite yet — librnotify's tests/ cover the
  inotify layer, Hypersleep's own scenarios (atomic save, deep
  mkdir, queue overflow) are still on the v0.2.0 list.

## [0.0.0]

### Added

- Initial project skeleton
- Architecture documentation
- CLI specification
- Configuration format
- Recovery procedures documentation
- librnotify integration via git submodule (third_party/librnotify)
- LMDB-based index schema design
- Content-addressable storage layout
- systemd unit file with hardening
- Apache-2.0 license
