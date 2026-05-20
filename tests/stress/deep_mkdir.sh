#!/usr/bin/env bash
# deep_mkdir.sh
#
# The killer feature of librnotify is race-free recursive watching:
# when a new directory tree is created in one shell command (mkdir -p
# plus file writes), librnotify's readdir-after-add_watch surfaces
# synthetic IN_CREATE events for entries that existed at the moment
# the parent's watch went in. Hypersleep's snapshot pipeline then
# captures each file into the CAS.
#
# If that race were open, the deep leaves would be silently lost.

. "$(dirname "$0")/lib.sh"

echo "== deep_mkdir =="
setup_env
trap teardown_env EXIT

start_daemon || exit 1

# Create the tree in one go so the deepest entries appear inside
# their parents before our watch on those parents settles.
mkdir -p "$WATCH/a/b/c/d/e"
for i in 1 2 3 4 5; do
    printf 'leaf_%d\n' "$i" >"$WATCH/a/b/c/d/e/file_$i.txt"
done

settle 2

for i in 1 2 3 4 5; do
    assert_log_contains "$WATCH/a/b/c/d/e/file_$i.txt" \
        "file_$i.txt captured under deep tree"
done

# And the contents must round-trip through `show`.
assert_show_eq "$WATCH/a/b/c/d/e/file_3.txt" v1 "leaf_3" \
    "file_3.txt v1 contents round-trip"

exit $FAILED
