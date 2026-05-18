# Renatum Configuration

Default location: `/etc/renatum/renatum.conf`
Override with `--config <path>` on `renatum` or `renatumd`.

## Format

Line-oriented. `#` starts a comment. Blank lines ignored. Directives are
case-insensitive.

## Directives

### Global

```
storage   /var/lib/renatum/store
index     /var/lib/renatum/index
log       /var/log/renatum/renatumd.log
log-level info                    # debug | info | warn | error
control-socket /run/renatum/control.sock
```

### Watch directives

```
watch <path> [option=value ...]
```

Options:
- `exclude="<regex>"` — POSIX extended regex matched against full path
- `recursive=yes|no` — default yes
- `retention=infinite | <duration>` — auto-prune older versions for this path
- `priority=low|normal|high` — debouncer ordering hint
- `compress=yes|no` — zstd compress blobs from this path (default: no)

Examples:

```
watch /home/andrey/projects exclude="\.git/|node_modules/|target/|\.o$|\.swp$"
watch /etc retention=infinite
watch /home/andrey/.config exclude="Cache/|Trash/"
watch /var/www exclude="\.log$|tmp/"
```

### Inotify tuning

```
inotify-max-watches-warn 524288   # warn if approaching this
debounce-ms 1500                  # event coalescing window
queue-poll-ms 500                 # waitNotify timeout
```

### Storage policy

```
compress-default no
compress-min-size 4096            # bytes; below this, never compress
fsync-mode full                   # full | data | none (none = unsafe)
store-fmode 0600                  # mode of new blobs
```

### Retention defaults

```
retention-default 90d
gc-after-prune yes
auto-prune-schedule "03:00 daily" # if Renatum should self-schedule
```

## Reloading

`renatum config reload` sends SIGHUP to the daemon. Watch directives
diff: new paths get added, removed paths get unwatched, changed
`exclude` patterns are re-applied. Storage and index paths cannot
be changed without restart.

## Permissions

The config file must be readable by the `renatumd` user (typically root,
but see `User=` in the systemd unit for dropping privileges).

`storage` and `index` directories must be writable by `renatumd` and
readable by anyone who should be able to run `renatum log/show/diff`.
A common pattern: `chgrp renatum /var/lib/renatum && chmod 2750
/var/lib/renatum`, then add users to the `renatum` group.

## Validation

`renatum config test` checks:
- Syntax
- Storage and index paths exist and are writable
- All `watch` paths exist
- Regexes compile
- No watch paths overlap each other ambiguously
- Total estimated inotify watches fits in `fs.inotify.max_user_watches`
