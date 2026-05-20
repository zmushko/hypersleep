# Recovery Procedures

This document covers what to do when Hypersleep itself is in trouble.

## Daemon will not start

### Check the obvious

```bash
sudo systemctl status hypersleepd
sudo journalctl -u hypersleepd -n 200 --no-pager
```

Common causes:
- Config syntax error → `hypersleep config test`
- Storage/index dir missing or unwritable → check permissions
- LMDB lock held by another process → `lsof /var/lib/hypersleep/index/lock.mdb`
- inotify watch limit exceeded → `cat /proc/sys/fs/inotify/max_user_watches`

### Reset the daemon state without losing history

```bash
sudo systemctl stop hypersleepd
# Index is safe; restart should succeed
sudo systemctl start hypersleepd
```

## Index is corrupted

LMDB is crash-safe, but hardware failure or out-of-space conditions can
still corrupt the index file. Symptoms:
- `hypersleepd` crashes on startup with `MDB_CORRUPTED` or similar
- `hypersleep log` returns garbage

### Recover from index corruption

The index can be **rebuilt from the CAS store** (which is content-
addressable and self-verifying). Procedure:

```bash
sudo systemctl stop hypersleepd

# Move the corrupt index aside (do not delete yet)
sudo mv /var/lib/hypersleep/index /var/lib/hypersleep/index.broken-$(date +%F)

# Rebuild
sudo hypersleep rebuild-index

# Verify
sudo hypersleep verify

# Restart
sudo systemctl start hypersleepd
```

`rebuild-index` walks the CAS, hashes every blob to confirm its name,
and reconstructs the `by_sha` refcount table. The `files` sub-DB **cannot**
be fully rebuilt — historical metadata (mtime, mode, paths) is lost
because that data lived only in the corrupted index.

**Implication:** for long-term recovery, periodically backup
`/var/lib/hypersleep/index/` itself (it's a single file plus a lock).
A weekly copy to off-site storage is sufficient.

## Blob is corrupted

`hypersleep verify` reports a blob whose computed SHA does not match its
filename.

```bash
# Inspect
sudo hypersleep verify --path /home/andrey/projects/foo.c

# Quarantine corrupt blobs
sudo hypersleep verify --repair

# Dependent versions will now report "blob missing" on restore.
# The index entry can be removed:
sudo hypersleep purge --path /home/andrey/projects/foo.c --by-sha <corrupt-sha>
```

If the corrupt blob's content is critical, restore from your **off-site
backup of the entire Hypersleep directory** (you have one, right?) and run
`hypersleep verify` again.

## Disk full on storage volume

Hypersleep will log loudly but continue running, queuing new snapshots in
memory until space is freed. Eventually the in-memory queue fills and
new snapshots are dropped (with a logged warning each).

### Make room

```bash
# How much is Hypersleep using?
sudo du -sh /var/lib/hypersleep/store

# Quick win: aggressive prune
sudo hypersleep purge --older-than 7d
sudo hypersleep gc --compact

# If still tight: identify the heaviest paths
sudo hypersleep find --size +10MB --format json \
    | jq -r '.[] | .path' | sort | uniq -c | sort -rn | head
```

### Move the store to a larger volume

```bash
sudo systemctl stop hypersleepd
sudo rsync -aHX --info=progress2 /var/lib/hypersleep/store/ /mnt/big/hypersleep-store/
# Update /etc/hypersleep/hypersleep.conf: storage /mnt/big/hypersleep-store
sudo systemctl start hypersleepd
sudo hypersleep verify
```

## inotify queue overflow

If `IN_Q_OVERFLOW` events appear in the log, the daemon will automatically
trigger a full rescan of all watched paths. During rescan, real-time events
are still consumed; the rescan adds capture entries for any files whose
state on disk differs from the latest indexed version.

To reduce future overflow:

```bash
# Increase queue size (default 16384)
sudo sysctl fs.inotify.max_queued_events=65536

# Make persistent
echo "fs.inotify.max_queued_events = 65536" \
    | sudo tee /etc/sysctl.d/99-hypersleep.conf
```

## Accidental destructive restore

You ran `hypersleep wake --force` on the wrong file.

**The good news:** Hypersleep pre-snapshotted the file before overwriting it
(unless you passed `--no-pre-snapshot`). The previous state is still in
the history as the most recent version.

```bash
# Find the pre-snapshot — it's the newest version that's NOT the one
# you just restored
hypersleep log /path/to/file

# Restore it
hypersleep wake /path/to/file v<latest> --force
```

If you used `--no-pre-snapshot`: the previous state is gone unless the
daemon captured it independently before your restore. Check `hypersleep log`
to confirm.

## Hypersleep is causing too much I/O

If you see Hypersleep chewing through disk on a workload it shouldn't be
watching:

```bash
# What's it snapshotting? Watch live:
sudo journalctl -u hypersleepd -f | grep '\[cap\]'

# Stop watching a noisy path
sudo $EDITOR /etc/hypersleep/hypersleep.conf
# (remove or narrow the offending `watch` line)
sudo hypersleep config reload
```

Common culprits:
- `node_modules/` or `target/` not in `exclude` regex
- A build process running in a watched directory
- A log file being appended to (use `IN_DONT_FOLLOW` for log dirs?)

## Recovery without the daemon

The CAS is just files on disk. The index is LMDB but readable with
standard tools.

### Read a specific blob without `hypersleep`

```bash
# Find the SHA of a captured version
mdb_dump -n -s files /var/lib/hypersleep/index/ \
    | grep -A1 'home/andrey/projects/foo.c' | tail -1

# Extract the SHA (first 32 bytes of the value)
# Use the SHA to locate the blob:
SHA=a3f291b4...e9c2
cat /var/lib/hypersleep/store/${SHA:0:2}/${SHA:2}
```

This means **even if Hypersleep is broken, your data is recoverable** with
nothing more than `cat` and a hex editor. That's by design.

## When all else fails

The most robust recovery is **prevention**. Pair Hypersleep with a periodic
off-site backup tool:

```bash
# /etc/cron.weekly/hypersleep-offsite
#!/bin/bash
set -e
borg create --stats --compression zstd \
    /backups/hypersleep::hypersleep-{now} \
    /var/lib/hypersleep/index \
    /var/lib/hypersleep/store
```

If everything local is destroyed, restore that Borg archive to a new
machine, install Hypersleep, point its config at the restored directories,
and you're back online with full history.
