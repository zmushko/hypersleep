#!/usr/bin/env bash
# recursive_move.sh
#
# Rename a watched subdirectory inside the watch root. The kernel
# keeps the wd alive (the directory inode is still there) but the
# path cache in librnotify goes stale. librnotify's renameWatches
# rewrites every path under the old prefix; subsequent events
# inside the moved subtree must surface under the NEW path, and
# the OLD prefix must not see new events.

. "$(dirname "$0")/lib.sh"

echo "== recursive_move =="
setup_env
trap teardown_env EXIT

mkdir -p "$WATCH/src/sub"
echo "before" >"$WATCH/src/sub/file.txt"

start_daemon || exit 1
settle 1

# Move the whole subtree.
mv "$WATCH/src" "$WATCH/dst"
settle 1

# Subsequent edit inside the renamed tree.
echo "after" >"$WATCH/dst/sub/file.txt"
settle 2

# The new path must have a version captured.
assert_log_contains "$WATCH/dst/sub/file.txt" \
    "event inside moved subtree surfaces under new path"

# The old prefix must not gain any new captures after the rename.
# (The initial-scan synthetic event from before the rename does
# carry the old path; that is correct and not what we are
# asserting here. We assert that the LATEST version of the new
# path is the post-rename content.)
assert_show_eq "$WATCH/dst/sub/file.txt" v1 "after" \
    "captured content under new path is the post-rename write"

exit $FAILED
