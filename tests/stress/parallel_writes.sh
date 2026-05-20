#!/usr/bin/env bash
# parallel_writes.sh
#
# Hammer the watcher with many concurrent file writes. The point is
# two-fold:
#
#   1. Stress the snapshot pipeline (lstat -> SHA -> store_put ->
#      index_insert) for a few hundred files in flight.
#   2. Probe IN_Q_OVERFLOW recovery — if we generate events faster
#      than the kernel queue can drain, the watcher should call
#      debounce_flush_all and snapshot_rescan; nothing should be
#      permanently lost.
#
# fs.inotify.max_queued_events defaults to 16384 on most distros,
# so saturating it from a shell takes work. We make do with a few
# hundred files which exercises the pipeline well even when no
# overflow fires.

. "$(dirname "$0")/lib.sh"

echo "== parallel_writes =="
setup_env
trap teardown_env EXIT

start_daemon || exit 1
settle 1

N=200
for i in $(seq 1 $N); do
    (printf 'payload_%d\n' "$i" >"$WATCH/file_$i.txt") &
done
wait

# Generous settle — 200 captures can take a few seconds even on a
# warm Pi.
settle 4

# Each file must have been captured exactly once. Sample a handful
# of indices spread across the range.
for i in 1 50 100 150 199 200; do
    assert_version_count "$WATCH/file_$i.txt" 1 \
        "file_$i.txt captured exactly once"
done

# Total entries actually visible via `hypersleep log` is harder to
# count cheaply without a `find`-style CLI; sampling above is good
# enough to flag a systematic loss. If overflow recovery fired, the
# daemon log records it.
if grep -q "IN_Q_OVERFLOW" "$LOG" 2>/dev/null; then
    echo "  INFO  IN_Q_OVERFLOW was observed and recovered"
fi

exit $FAILED
