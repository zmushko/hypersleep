#!/usr/bin/env bash
# atomic_save.sh
#
# Editors do atomic saves: write a temp file, fsync it, rename it
# over the target. The watcher sees IN_CREATE / IN_CLOSE_WRITE for
# the temp and IN_MOVED_FROM / IN_MOVED_TO for the rename — many
# events for what is logically one save. The debouncer should
# collapse this into a single captured version.
#
# Three rounds, three versions expected (one per save, not 4+ per
# save).

. "$(dirname "$0")/lib.sh"

echo "== atomic_save =="
setup_env
trap teardown_env EXIT

start_daemon || exit 1

TARGET="$WATCH/target.txt"

for round in 1 2 3; do
    TMP="$WATCH/.target.$$.tmp"
    printf 'content_%d\n' "$round" >"$TMP"
    sync
    mv "$TMP" "$TARGET"
    settle 1
done

assert_log_contains "$TARGET" "target captured"
assert_version_count "$TARGET" 3 "three saves -> three versions"
assert_show_eq "$TARGET" v3 "content_3" "latest version has round-3 content"
assert_show_eq "$TARGET" v1 "content_1" "oldest version has round-1 content"

exit $FAILED
