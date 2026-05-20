#!/usr/bin/env bash
# tests/stress/run_all.sh
#
# Build first if the binaries are missing, then run every stress
# test in this directory. Exits 0 only if every test exited 0.

set -u

cd -- "$(dirname -- "$0")"
REPO_ROOT=$(cd ../.. && pwd)

if [ ! -x "$REPO_ROOT/hypersleepd" ] || [ ! -x "$REPO_ROOT/hypersleep" ]; then
    echo "run_all: building first..."
    (cd "$REPO_ROOT" && make) || {
        echo "run_all: build failed" >&2
        exit 2
    }
fi

passed=0
failed=0
failed_names=""

for t in deep_mkdir.sh atomic_save.sh symlink_no_follow.sh \
         recursive_move.sh parallel_writes.sh; do
    if [ ! -x "$t" ]; then
        echo "skip $t (not executable)"
        continue
    fi
    if ./"$t"; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        failed_names="$failed_names $t"
    fi
    echo
done

echo "---"
echo "summary: $passed passed, $failed failed"
if [ $failed -gt 0 ]; then
    echo "failed:$failed_names"
    exit 1
fi
