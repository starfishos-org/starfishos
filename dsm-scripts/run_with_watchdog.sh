#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi
#
# Run a test command under the host-side log watchdog.
#
#   ./dsm-scripts/run_with_watchdog.sh -- ./dsm-scripts/tests/leveldb.exp
#   ./dsm-scripts/run_with_watchdog.sh -n 2 -- ./dsm-scripts/tests/phoenix/pca.exp 8
#
# The expect-driven tests boot QEMU themselves and only know about the guest
# output they explicitly wait for, so a kernel panic on any machine leaves them
# spinning until their own `set timeout` expires.  This wrapper starts
# log_watchdog.py on the host for the duration of the command; when the
# watchdog reports a fatal guest signature the command is killed, leftover
# ChCore QEMU processes are reaped, and the wrapper exits 1.
#
# Options:
#   -n N   watch machines 0..N-1 (default 8; logs that never appear are simply
#          not matched, so the default covers every cluster size)
#   -k     keep guest QEMU running after a detected failure (for debugging)
# Set WATCHDOG=0 to run the command without any watching at all.

set -uo pipefail

MACHINES=8
KEEP_QEMU=0

while [ $# -gt 0 ]; do
    case "$1" in
        -n) MACHINES="$2"; shift 2 ;;
        -k) KEEP_QEMU=1; shift ;;
        --) shift; break ;;
        *)  break ;;
    esac
done

if [ $# -eq 0 ]; then
    echo "Usage: $0 [-n N] [-k] -- <command> [args...]" >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=../artifact-evaluation/common.sh
source "$REPO_DIR/artifact-evaluation/common.sh"

if [ "${WATCHDOG:-1}" != "1" ]; then
    exec "$@"
fi

WATCHDOG_DIR="$REPO_DIR/logs/watchdog"
WATCHDOG_FLAG="$WATCHDOG_DIR/$(basename "$1").flag"
declare -a LOG_START_OFFSETS=()
declare -a LOG_START_INODES=()
mkdir -p "$WATCHDOG_DIR" "$AE_MACHINE_LOG_DIR"

rm -f "$WATCHDOG_FLAG"

WATCH_MODE_ARGS=()
if ae_has_chcore_qemu; then
    # A guest is already up (e.g. cfork-restore next to a live cfork-prepare):
    # its log must not be unlinked underneath the running tee, so watch from
    # where the logs stand right now instead.
    echo "[WATCHDOG] guests already running; watching logs from their current end"
    WATCH_MODE_ARGS=(--start-at-end)
    for ((i = 0; i < MACHINES; i++)); do
        log="$AE_MACHINE_LOG_DIR/exec_log${i}.log"
        LOG_START_OFFSETS[i]="$(stat -c%s "$log" 2>/dev/null || echo 0)"
        LOG_START_INODES[i]="$(stat -c%i "$log" 2>/dev/null || true)"
    done
else
    # Serial logs are rewritten by the boot this command is about to do;
    # dropping them keeps a previous run's panic from failing this one.
    for ((i = 0; i < MACHINES; i++)); do
        rm -f "$AE_MACHINE_LOG_DIR/exec_log${i}.log"
        LOG_START_OFFSETS[i]=0
        LOG_START_INODES[i]=""
    done
fi

python3 "$SCRIPT_DIR/log_watchdog.py" \
    --log-dir "$AE_MACHINE_LOG_DIR" \
    --count "$MACHINES" \
    --flag-file "$WATCHDOG_FLAG" \
    --status-log "$WATCHDOG_DIR/$(basename "$1").log" \
    --pattern "$AE_ERROR_PATTERN" \
    --label "$(basename "$1")" \
    "${WATCH_MODE_ARGS[@]+"${WATCH_MODE_ARGS[@]}"}" &
WATCHDOG_PID=$!

stop_watchdog() {
    kill "$WATCHDOG_PID" 2>/dev/null || true
    wait "$WATCHDOG_PID" 2>/dev/null || true
}

watchdog_failure_seen() {
    local machine log fatal start current_size current_inode

    if [ -f "$WATCHDOG_FLAG" ]; then
        return 0
    fi

    # The child can exit between two watchdog polls.  Scan synchronously
    # before accepting its exit status so the final log bytes cannot escape
    # detection merely because the watcher has not written its flag yet.
    for ((machine = 0; machine < MACHINES; machine++)); do
        log="$AE_MACHINE_LOG_DIR/exec_log${machine}.log"
        start="${LOG_START_OFFSETS[machine]:-0}"
        current_size="$(stat -c%s "$log" 2>/dev/null || echo 0)"
        current_inode="$(stat -c%i "$log" 2>/dev/null || true)"
        if [ -z "${LOG_START_INODES[machine]:-}" ] \
           || [ "$current_inode" != "${LOG_START_INODES[machine]}" ] \
           || [ "$current_size" -lt "$start" ]; then
            start=0
        fi
        fatal="$(tail -c "+$((start + 1))" "$log" 2>/dev/null \
            | grep -aE "$AE_ERROR_PATTERN" | head -1 || true)"
        if [ -n "$fatal" ]; then
            echo "[WATCHDOG][FATAL] final scan: machine $machine -> $fatal" >&2
            echo "[WATCHDOG][FATAL] log: $log" >&2
            return 0
        fi
    done
    return 1
}

reap_failed_guests() {
    if [ "$KEEP_QEMU" = "0" ]; then
        ae_reap_leftover_qemu TERM
        ae_wait_qemu_gone 20 || true
    fi
}

# "<&0" is required: a background command in a non-interactive shell otherwise
# gets /dev/null on stdin, which would break the expect scripts that end in
# "interact" and hand the guest console to the user.  Job control is off here,
# so the child stays in this process group and keeps the terminal.
"$@" <&0 &
CHILD=$!

while kill -0 "$CHILD" 2>/dev/null; do
    if [ -f "$WATCHDOG_FLAG" ]; then
        echo "" >&2
        echo "[WATCHDOG] aborting '$*':" >&2
        cat "$WATCHDOG_FLAG" >&2 || true
        kill -TERM "$CHILD" 2>/dev/null || true
        sleep 3
        kill -KILL "$CHILD" 2>/dev/null || true
        wait "$CHILD" 2>/dev/null || true
        # expect spawns QEMU itself, so killing it leaves guests behind.
        reap_failed_guests
        stop_watchdog
        exit 1
    fi
    sleep 1
done

wait "$CHILD"
rc=$?
if watchdog_failure_seen; then
    echo "" >&2
    echo "[WATCHDOG] '$*' exited after a fatal guest signature" >&2
    [ -f "$WATCHDOG_FLAG" ] && cat "$WATCHDOG_FLAG" >&2 || true
    reap_failed_guests
    stop_watchdog
    exit 1
fi
stop_watchdog
exit "$rc"
