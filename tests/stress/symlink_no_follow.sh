#!/usr/bin/env bash
# symlink_no_follow.sh
#
# Hypersleep safety property #1: an attacker with write access to a
# watched directory must not be able to make us snapshot
# /etc/shadow via a planted symlink. The defence is layered:
#
#   * librnotify v3 always sets IN_DONT_FOLLOW on inotify_add_watch
#   * snapshot.c uses lstat() in the recursive descent
#   * nftw in snapshot_rescan uses FTW_PHYS
#
# Plant a symlink inside the watch root pointing at a file outside
# of it, modify the link's target, and assert that nothing under
# the OUTSIDE path appears in the daemon log nor in any captured
# version. The link itself may show up as a CREATE; that's fine,
# but it must not have ISDIR semantics and must not recurse.

. "$(dirname "$0")/lib.sh"

echo "== symlink_no_follow =="
setup_env
trap teardown_env EXIT

OUTSIDE="$TMPROOT/outside"
mkdir "$OUTSIDE"
echo "secret" >"$OUTSIDE/secret.txt"

start_daemon || exit 1

# Plant the link AFTER the daemon starts so it goes through the
# live event path (initial-scan synthetic events are exercised by
# deep_mkdir).
ln -s "$OUTSIDE" "$WATCH/link"

# Touch a real file inside the watched tree as a canary.
echo "alive" >"$WATCH/canary.txt"

# Now modify the symlink target's content. If the watcher were
# following the link, this write would show up under
# $OUTSIDE/secret.txt — but the daemon must not have a watch on
# $OUTSIDE at all.
echo "tampered" >"$OUTSIDE/secret.txt"

settle 2

# Canary must be captured.
assert_log_contains "$WATCH/canary.txt" "canary file captured"

# The link's target must not appear in any captured path. We probe
# via `hypersleep log` rather than grepping the daemon log because
# what we care about is whether a version got committed, not what
# debug messages got logged.
if "$HYPERSLEEP" --config "$CONF" log "$OUTSIDE/secret.txt" \
        >/dev/null 2>&1; then
    echo "  FAIL  symlink target leaked into the index"
    FAILED=$((FAILED + 1))
else
    echo "  PASS  no versions under symlink target"
fi

# The link itself does not need to be captured (it isn't a regular
# file), but we must not have crashed or stopped watching.
assert_log_grep "$WATCH/canary.txt" "daemon still processing events after symlink"

exit $FAILED
