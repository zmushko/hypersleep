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

# After the move there are TWO versions under the new path:
#   v1  the move-induced capture documenting "what was at the new
#       path the moment it appeared" — content is the pre-rename
#       file ("before");
#   v2  the post-rename write of "after".
# Both are intentional. Assert v1 == before, v2 == after.
assert_show_eq "$WATCH/dst/sub/file.txt" v1 "before" \
    "v1 under new path snapshots the pre-rename content"
assert_show_eq "$WATCH/dst/sub/file.txt" v2 "after" \
    "v2 under new path is the post-rename write"

exit $FAILED
