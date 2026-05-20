# tests/stress/lib.sh
#
# Shared scaffolding for the Hypersleep stress tests. Each test
# sources this file, calls setup_env, runs its scenario, asserts via
# the helpers below, and on exit teardown_env tears the daemon down
# and removes the temp tree.
#
# Tests communicate with the daemon only through:
#   - the filesystem under $WATCH (events flow through the watcher)
#   - the CLI binary (./hypersleep) reading the same store/index
# i.e. exactly the way a real operator does.
#
# Linux only — Hypersleep is Linux-only at runtime (inotify, epoll).
# Bash 4+ assumed.

set -euo pipefail

# Resolve paths relative to the repo root so the suite is callable
# from anywhere.
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
HYPERSLEEPD="$REPO_ROOT/hypersleepd"
HYPERSLEEP="$REPO_ROOT/hypersleep"

if [ ! -x "$HYPERSLEEPD" ] || [ ! -x "$HYPERSLEEP" ]; then
    echo "tests/stress: build first (\`make\` at repo root)" >&2
    exit 2
fi

FAILED=0
TMPROOT=""
DAEMON_PID=""

setup_env() {
    TMPROOT=$(mktemp -d /tmp/hypersleep-stress.XXXXXX)
    export WATCH="$TMPROOT/watch"
    export STORE="$TMPROOT/store"
    export INDEX="$TMPROOT/index"
    export CONF="$TMPROOT/hypersleep.conf"
    export LOG="$TMPROOT/hypersleepd.log"
    export SOCK="$TMPROOT/control.sock"

    mkdir -p "$WATCH" "$STORE" "$INDEX"

    cat >"$CONF" <<EOF
storage         $STORE
index           $INDEX
log             $LOG
log-level       debug
control-socket  $SOCK
debounce-ms     200
queue-poll-ms   100

watch $WATCH
EOF
}

# Start the daemon in foreground and wait for it to be ready
# (heuristic: log file contains "started, watching"). Returns 0 on
# success, non-zero on timeout — caller should fail the test.
start_daemon() {
    "$HYPERSLEEPD" --foreground --config "$CONF" \
        >>"$LOG" 2>&1 &
    DAEMON_PID=$!

    local waited=0
    while [ $waited -lt 50 ]; do
        if grep -q "started, watching" "$LOG" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
            echo "daemon died before READY:" >&2
            sed 's/^/  /' "$LOG" >&2
            return 1
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    echo "daemon timed out becoming READY:" >&2
    sed 's/^/  /' "$LOG" >&2
    return 1
}

stop_daemon() {
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill -TERM "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    DAEMON_PID=""
}

teardown_env() {
    stop_daemon
    if [ -n "$TMPROOT" ] && [ -d "$TMPROOT" ]; then
        rm -rf "$TMPROOT"
    fi
}

# Give the watcher's debouncer time to flush. Tests should call
# settle after producing events and before asserting.
settle() {
    sleep "${1:-1}"
}

assert_log_contains() {
    local path=$1
    local desc=$2
    if "$HYPERSLEEP" --config "$CONF" log "$path" >/dev/null 2>&1; then
        echo "  PASS  $desc"
    else
        echo "  FAIL  $desc"
        echo "        no versions for $path"
        FAILED=$((FAILED + 1))
    fi
}

# Count captured versions of $path.
count_versions() {
    local path=$1
    # log output: header line + N data lines; the header starts with VER
    local n
    n=$("$HYPERSLEEP" --config "$CONF" log "$path" 2>/dev/null \
        | awk 'NR > 1 { c++ } END { print c+0 }')
    echo "$n"
}

assert_version_count() {
    local path=$1
    local want=$2
    local desc=$3
    local got
    got=$(count_versions "$path")
    if [ "$got" -eq "$want" ]; then
        echo "  PASS  $desc ($got)"
    else
        echo "  FAIL  $desc — want $want got $got"
        FAILED=$((FAILED + 1))
    fi
}

# Latest captured SHA prefix for $path (first 8 hex chars).
latest_sha_prefix() {
    local path=$1
    "$HYPERSLEEP" --config "$CONF" log --limit 1 "$path" 2>/dev/null \
        | awk 'NR > 1 { print $4 }'
}

# Capture `hypersleep show <path> v<N>` into a temp file and compare
# byte-for-byte against an expected content string.
assert_show_eq() {
    local path=$1
    local vN=$2
    local want=$3
    local desc=$4
    local got
    got=$("$HYPERSLEEP" --config "$CONF" show "$path" "$vN" 2>/dev/null) || {
        echo "  FAIL  $desc — show failed"
        FAILED=$((FAILED + 1))
        return
    }
    if [ "$got" = "$want" ]; then
        echo "  PASS  $desc"
    else
        echo "  FAIL  $desc"
        echo "        want: $want"
        echo "        got : $got"
        FAILED=$((FAILED + 1))
    fi
}

assert_log_grep() {
    local needle=$1
    local desc=$2
    if grep -Fq "$needle" "$LOG" 2>/dev/null; then
        echo "  PASS  $desc"
    else
        echo "  FAIL  $desc — '$needle' not in daemon log"
        FAILED=$((FAILED + 1))
    fi
}
