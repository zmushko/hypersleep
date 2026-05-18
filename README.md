# Renatum

**Continuous filesystem versioning daemon for Linux.**

Watches your directories through inotify and captures every change the moment
it happens — not every hour, not every day, but every save. When you need to
recover, every past state of every file is one command away.

> *renatum* (Latin) — "reborn". Every restore is a rebirth of a file's state.

## Why Renatum?

### The gap in Linux backup tooling

| Approach | Continuous | Versioned history | Pure C | Embedded-ready |
|---|---|---|---|---|
| `cron + rsync` | ❌ scheduled | partial | ❌ shell | ✓ |
| `inotifywait + rsync` scripts | ✓ | ❌ replication only | ❌ shell | partial |
| `lsyncd` | ✓ | ❌ replication only | ❌ Lua | partial |
| Btrfs/ZFS snapshots | partial (manual) | ✓ | n/a | requires FS |
| `restic` / `borg` / `kopia` | ❌ scheduled (cron) | ✓ | ❌ Go / Python | heavy |
| Apple Time Machine | partial (hourly) | ✓ | n/a | macOS only |
| Enterprise CDP (Veeam etc.) | ✓ | ✓ | ❌ | ❌ |
| **Renatum** | **✓** | **✓** | **✓** | **✓** |

### What Renatum does

- **Captures every save.** A daemon subscribes to inotify events through
  [librnotify](third_party/librnotify) and snapshots files into a
  content-addressable store the moment `IN_CLOSE_WRITE` fires.
- **Race-free recursive watching.** When a new subdirectory is created,
  librnotify closes the inotify race condition by performing a `readdir()`
  after `inotify_add_watch()` and synthesizing events for files that
  appeared in between. No lost events. No missed files.
- **Content-addressable storage.** Files are stored by SHA-256. Duplicates
  cost one hash and no write. Identical files across paths share a single
  blob.
- **LMDB-backed index.** Sub-millisecond lookups, crash-safe, zero-copy
  reads. No SQL, no schema migrations, no fsync hell.
- **One small native daemon.** Pure C. Minimal dependencies. Runs on a
  Raspberry Pi 3A+ without breaking a sweat.
- **Git-style CLI.** `renatum log`, `renatum show`, `renatum diff`,
  `renatum restore` — familiar verbs, predictable behaviour.

### What Renatum is NOT

- Not a replacement for `restic` or `borg`. Those do encrypted, deduplicated,
  off-site backups on a schedule. Renatum is the layer **between** those
  scheduled jobs — protecting you from the file you deleted at 14:32 when
  the next `borg create` runs at 18:00.
- Not a sync tool (use `syncthing` or `lsyncd`).
- Not a VCS (use `git`).
- Not a snapshot tool tied to a specific filesystem (use Btrfs/ZFS).

Run Renatum **alongside** these. It complements; it does not compete.

## Quick start

```bash
# Build
git submodule update --init --recursive
make

# Install
sudo make install

# Configure
sudo cp /etc/renatum/renatum.conf.example /etc/renatum/renatum.conf
sudo $EDITOR /etc/renatum/renatum.conf

# Start
sudo systemctl enable --now renatumd

# Use
renatum status
renatum log /home/andrey/projects/foo.c
renatum show /home/andrey/projects/foo.c v5 | less
renatum diff /home/andrey/projects/foo.c v3 v5
renatum restore /home/andrey/projects/foo.c v5 --to /tmp/foo.c.recovered
```

## Documentation

- [Architecture](docs/architecture.md) — how Renatum works internally
- [CLI specification](docs/cli-spec.md) — full command reference
- [Project brief](docs/project-brief.md) — design decisions and rationale
- [Configuration](docs/config.md) — `/etc/renatum/renatum.conf` format
- [Recovery](docs/recovery.md) — how to recover from corruption / disasters

## Status

**Pre-alpha.** This is a personal tool under active development. Not yet
suitable for protecting irreplaceable data. Use alongside (not instead of) a
proper periodic backup tool.

## License

Apache-2.0. See [LICENSE](LICENSE).

## Author

Andrey Zmushko ([@zmushko](https://github.com/zmushko))

Built on [librnotify](https://github.com/zmushko/librnotify) (same author).
