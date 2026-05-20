#!/usr/bin/env bash
# tests/stress/deep_mkdir.sh
#
# Stress test the race-free recursive watching of librnotify (via Hypersleep).
# Creates a deep directory structure atomically and verifies that every
# file gets captured into the CAS.

set -euo pipefail

TMPDIR=$(mktemp -d /tmp/hypersleep-stress.XXXXXX)
trap "rm -rf $TMPDIR" EXIT

# TODO: when hypersleepd is implemented, start it pointing at $TMPDIR
# and verify it captures all created files.
echo "deep_mkdir.sh: scaffolding only — fill in after hypersleepd works"

WATCH_DIR="$TMPDIR/watch"
mkdir -p "$WATCH_DIR"

# Atomic deep create with files
( mkdir -p "$WATCH_DIR/L1/L2/L3/L4/L5"
  for L in L1 L1/L2 L1/L2/L3 L1/L2/L3/L4 L1/L2/L3/L4/L5; do
    for i in 1 2 3; do
      echo "content of $L/file_$i" > "$WATCH_DIR/$L/file_$i.txt"
    done
  done
) &
wait

# Expected: 5 directories + 15 files = 20 capture events
echo "Created tree:"
find "$WATCH_DIR" | wc -l

# TODO: query hypersleep log to verify all files captured
echo "Skipping verification until hypersleepd is implemented"
