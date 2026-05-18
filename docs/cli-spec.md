# Renatum CLI Specification

> All commands return:
> - exit 0 on success
> - exit 2 on invalid arguments
> - exit 3 when nothing matches (e.g. no versions for path)
> - exit 4 when target exists and `--force` was not given
> - exit 5 on permission denied
> - exit 6 on storage corruption
> - exit 7 on insufficient space
> - exit 8 on user-cancelled interactive operation
> - exit 1 on any other error

All commands accept:
- `--quiet` — suppress all output except exit code
- `--verbose` — detailed operational log to stderr
- `--format {human|json}` — output format (default: human)
- `--config <path>` — alternative config file
- `--help` — usage

## `renatum status`

```
$ renatum status

renatumd: running (pid 4231, uptime 3d 14h)
config:   /etc/renatum/renatum.conf
store:    /var/lib/renatum/store  (1.2 GB, 18,432 blobs)
index:    /var/lib/renatum/index  (84 MB, 47,213 entries)

watching 4 paths:
  /home/andrey/projects     12,847 files, 8,231 dirs    [active]
  /etc                         412 files,   78 dirs    [active]
  /home/andrey/.config       3,891 files,  221 dirs    [active]
  /var/www                  20,124 files, 1,932 dirs   [active]

last overflow:  never
last GC:        2026-05-15 03:00:12  (freed 142 MB)
queue depth:    0  (peak 1h: 23)
```

Flags:
- `--json` — structured output for automation

## `renatum log`

List versions of a path.

```
$ renatum log /home/andrey/projects/foo.c

VERSION  CAPTURED              SIZE      SHA       EVENT
v7       2026-05-18 11:23:04   2.4 KB    a3f291b4  modify
v6       2026-05-18 09:15:11   2.3 KB    8c7e2110  modify
v5       2026-05-17 18:42:09   2.1 KB    1de44ba9  modify
v4       2026-05-17 14:30:22   1.9 KB    7fab1c20  modify
v3       2026-05-17 10:11:55   1.5 KB    3e9d8a01  create
v2       2026-05-16 22:00:18    ---      (deleted) delete
v1       2026-05-15 08:30:00   1.2 KB    5fb22e8c  create
```

Flags:
- `--limit N` — show only the most recent N versions
- `--since <time>` — versions captured after `<time>`
- `--until <time>` — versions captured before `<time>`
- `--range vA..vB` — versions in a range
- `--oneline` — one line per version, machine-friendly
- `--format json` — JSON array of entries
- `--follow-renames` — trace history across `mv` operations (via cookies)
- `--show-deleted` — include deletion markers (default: yes)

## `renatum show`

Print the contents of a version to stdout. Never writes to the original path.

```
$ renatum show /home/andrey/projects/foo.c v5 | less
$ renatum show /home/andrey/projects/foo.c --at "yesterday 14:00"
$ renatum show /home/andrey/projects/foo.c --at 1715949000  # unix epoch
$ renatum show /home/andrey/projects/foo.c --before "today 09:00"
```

Selectors (mutually exclusive, exactly one required):
- `<vN>` — positional, e.g. `v5`
- `--at <time>` — exact match or nearest preceding version
- `--before <time>` — last version captured strictly before `<time>`
- `--by-sha <hex>` — by content hash

Flags:
- `--out <path>` — write to file instead of stdout (path MUST NOT equal
  the original watched path; use `restore` for that)
- `--no-decompress` — for compressed blobs, emit the raw compressed frame

For binary files, `show` writes raw bytes. The user is responsible for
piping to appropriate tools.

## `renatum diff`

Textual diff between versions, or between a version and the working copy.

```
$ renatum diff /home/andrey/projects/foo.c v3 v5
$ renatum diff /home/andrey/projects/foo.c v5           # v5 vs disk
$ renatum diff /home/andrey/projects/foo.c \
      --from "yesterday 09:00" --to "today 11:00"
```

Flags:
- `--stat` — summary only (lines added/removed)
- `--tool <name>` — invoke external diff (vimdiff, meld, kdiff3, etc.)
- `--unified N` (alias `-u N`) — context lines (default 3)
- `--color {auto|always|never}` — default `auto`
- `--no-pager` — disable pager (default: pipe to `less -R` if stdout is tty)

For binary files: prints `Binary files differ: vA=<size>, vB=<size>` and
nothing else.

## `renatum restore`

Destructive operation. Multiple safety checks.

```
# Safe: write to a new location
$ renatum restore /home/andrey/projects/foo.c v5 --to /tmp/foo.recovered.c

# Destructive: overwrite original (requires --force)
$ renatum restore /home/andrey/projects/foo.c v5 --force

# By time
$ renatum restore /home/andrey/projects/foo.c \
      --at "yesterday 14:00" --to /tmp/foo.old.c

# Dry run
$ renatum restore /home/andrey/projects/foo.c v5 --force --dry-run
```

Selectors: same as `show` (`<vN>`, `--at`, `--before`, `--by-sha`).

Flags:
- `--to <path>` — destination path. If omitted and `--force` not set,
  exits with code 4.
- `--force` — allow overwrite of existing target. Triggers a pre-snapshot
  of the current state of the target (if it has not been captured yet)
  before writing.
- `--no-pre-snapshot` — skip the safety pre-snapshot. Use only when you
  know the current state is captured or you don't care about losing it.
- `--preserve-mode` — restore mode, uid, gid, mtime from the captured
  metadata (default: yes)
- `--no-preserve-mode` — restore content only; new file inherits process
  umask and identity
- `--verify` — after write, re-read the file and verify SHA matches the
  captured version (default: yes for `--force`, no for `--to`)
- `--dry-run` — print what would be done, change nothing

## `renatum restore-tree`

Restore a subtree to its state at a given time.

```
$ renatum restore-tree /home/andrey/projects/ \
      --at "yesterday 14:00" --to /tmp/yesterday-snapshot/

$ renatum restore-tree /home/andrey/projects/ \
      --at "yesterday 14:00" --in-place

$ renatum restore-tree /home/andrey/projects/ \
      --at "yesterday" --only-missing --to /tmp/recovered/

$ renatum restore-tree /home/andrey/projects/ --at "yesterday" --dry-run
```

Required: one of `--to <dir>` OR `--in-place`.

Flags:
- `--at <time>` — required; the historical timestamp to materialize
- `--to <dir>` — restore into a new directory (created if missing)
- `--in-place` — restore over the original location. **Requires `--force`
  AND interactive `y` confirmation.** Without a TTY, additionally requires
  `--yes-i-really-mean-it`.
- `--only-missing` — only restore files that no longer exist on disk
- `--include <pattern>` / `--exclude <pattern>` — glob filters
- `--dry-run` — list operations without performing them

Before any `--in-place` operation, Renatum captures the current state of
the entire affected subtree as a series of snapshots, so the action is
reversible.

## `renatum recover`

Interactive recovery mode for stressful situations.

```
$ renatum recover /home/andrey/projects/foo.c

Found 7 versions of foo.c:

  v7  2026-05-18 11:23  2.4 KB  modify   (matches current disk state)
  v6  2026-05-18 09:15  2.3 KB  modify
  v5  2026-05-17 18:42  2.1 KB  modify
  v4  2026-05-17 14:30  1.9 KB  modify
  v3  2026-05-17 10:11  1.5 KB  create
  v2  2026-05-16 22:00  ----    delete
  v1  2026-05-15 08:30  1.2 KB  create

[s] show version  [d] diff with current  [r] restore  [q] quit
> 
```

Designed to be usable over plain SSH (no full TTY required, no ncurses).
Always confirms before destructive actions.

## `renatum find`

Search across the history.

```
$ renatum find --name "*.conf"
$ renatum find --name "nginx*" --since "last week"
$ renatum find --grep "DATABASE_PASSWORD" --paths "/etc/**"
$ renatum find --deleted --since "last month"
$ renatum find --size +10MB --until "yesterday"
$ renatum find --by-sha 5fb22e8c     # partial SHA prefix
```

Flags:
- `--name <glob>` — match basename
- `--path <glob>` — match full path
- `--grep <pattern>` — search inside file contents (slow; warns on
  large result set)
- `--deleted` — only paths with a deletion marker
- `--size <expr>` — size filter (`+1MB`, `-100KB`, `=4096`)
- `--since` / `--until` — time range
- `--by-sha <prefix>` — find by content hash prefix
- `--format json` — machine output

## `renatum prune`

Remove old versions according to retention policy.

```
$ renatum prune --older-than 30d
$ renatum prune --keep-daily 7 --keep-weekly 4 --keep-monthly 6
$ renatum prune --path /tmp/builds --older-than 1d  # path-specific
$ renatum prune --dry-run
```

After pruning index entries, `prune` invokes the GC to delete unreferenced
blobs.

Flags:
- `--older-than <duration>` — simple cutoff
- `--keep-N-{daily,weekly,monthly,yearly}` — borg/restic-style retention
- `--path <prefix>` — limit to a path prefix (default: all watched paths)
- `--dry-run` — report what would be pruned
- `--no-gc` — don't run blob GC after pruning (useful in scripts)

## `renatum gc`

Garbage-collect unreferenced blobs from the CAS.

```
$ renatum gc
$ renatum gc --compact   # also compact the LMDB file
$ renatum gc --dry-run
```

Run automatically after `prune` unless `--no-gc` was given. Safe to
interrupt; partial GC just leaves more blobs for the next pass.

## `renatum verify`

Check integrity of the store and index.

```
$ renatum verify                  # check everything
$ renatum verify --quick          # sample-based, faster
$ renatum verify --repair         # quarantine corrupt blobs
$ renatum verify --path foo.c     # verify a specific path's history
```

For each blob: re-compute SHA, compare to filename. For each index entry:
verify the referenced blob exists. Reports a final tally and exits 6 if
anything failed (unless `--repair`).

## `renatum config`

Inspect or test the configuration.

```
$ renatum config show          # dump effective config
$ renatum config test          # validate config syntax + permissions
$ renatum config paths         # list configured watch paths
$ renatum config reload        # signal daemon to reload (SIGHUP)
```

## Time expression syntax

All time arguments accept:

**Absolute:**
- `YYYY-MM-DD` (start of day)
- `YYYY-MM-DD HH:MM`
- `YYYY-MM-DD HH:MM:SS`
- ISO 8601: `2026-05-17T14:30:00+03:00`
- Unix epoch (seconds): `1715949000`

**Relative:**
- `N {seconds|minutes|hours|days|weeks|months|years} ago`
- `yesterday`, `today`, `tomorrow` (with optional `HH:MM`)
- `last {monday|tuesday|...}`
- `last {week|month|year}`

**Special:**
- `latest` — most recent version
- `oldest` — earliest version
- `current` — current state on disk
