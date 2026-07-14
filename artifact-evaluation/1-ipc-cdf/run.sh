#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$REPO_ROOT/artifact-evaluation/common.sh"
AE_DIR="$REPO_ROOT/artifact-evaluation/1-ipc-cdf"
# Keep the latest artifact in one predictable location.  Each invocation
# truncates these two logs before booting, rather than accumulating per-run
# copies under out/<timestamp>.
OUT_DIR="${OUT_DIR:-$AE_DIR}"
LOG_DIR="${LOG_DIR:-$AE_DIR/logs}"
SESSION="${SESSION:-${USER}-ipc-ae}"
NUM_MACHINES=2
TIMEOUT="${TIMEOUT:-600}"
INPUT_TIMEOUT="${INPUT_TIMEOUT:-30}"
KEEP_QEMU="${KEEP_QEMU:-0}"
SKIP_BUILD="${SKIP_BUILD:-0}"
PROJECT_CONFIG="$REPO_ROOT/.config"

CLIENT_SRC="$REPO_ROOT/user/system-servers/polling/polling_client_test.c"
SERVER_SRC="$REPO_ROOT/user/system-servers/polling/polling_server.c"
RESP_SRC="$REPO_ROOT/user/system-servers/polling/polling_resp.c"

mkdir -p "$LOG_DIR" "$OUT_DIR"

TMP_DIR="$(mktemp -d)"
cp "$CLIENT_SRC" "$TMP_DIR/polling_client_test.c"
cp "$SERVER_SRC" "$TMP_DIR/polling_server.c"
cp "$RESP_SRC" "$TMP_DIR/polling_resp.c"

restore_sources() {
    cp "$TMP_DIR/polling_client_test.c" "$CLIENT_SRC"
    cp "$TMP_DIR/polling_server.c" "$SERVER_SRC"
    cp "$TMP_DIR/polling_resp.c" "$RESP_SRC"
    rm -rf "$TMP_DIR"
}

cleanup() {
    restore_sources
    if [ "$KEEP_QEMU" != "1" ]; then
        ae_stop_tmux_and_reap "$SESSION" || true
    fi
}
trap cleanup EXIT

set_define() {
    local file="$1"
    local flag="$2"
    local value="$3"
    sed -i "s/^#define ${flag} [01]/#define ${flag} ${value}/" "$file"
}

disable_kernel_tests() {
    if [ ! -f "$PROJECT_CONFIG" ]; then
        echo "Missing ChCore config: $PROJECT_CONFIG" >&2
        return 1
    fi

    sed -i 's/^CHCORE_KERNEL_TEST:BOOL=.*/CHCORE_KERNEL_TEST:BOOL=OFF/' \
        "$PROJECT_CONFIG"
    grep -q '^CHCORE_KERNEL_TEST:BOOL=OFF$' "$PROJECT_CONFIG"
    echo "=== CHCORE_KERNEL_TEST=OFF for IPC artifact ==="
}

wait_for_log_text() {
    local machine="$1"
    local pattern="$2"
    local label="$3"
    local start_line="${4:-1}"
    local logfile="$LOG_DIR/machine${machine}.log"
    local elapsed=0
    while [ "$elapsed" -lt "$TIMEOUT" ]; do
        if ! tmux has-session -t "$SESSION" 2>/dev/null; then
            echo "tmux session $SESSION exited while waiting for $label" >&2
            tail -120 "$logfile" >&2 || true
            return 1
        fi
        # QEMU's serial output can interleave concurrent guest messages, so
        # capture-pane may split a banner across lines and its scrollback is
        # not a reliable readiness source.  The tee'd serial log is complete.
        if tail -n "+$start_line" "$logfile" 2>/dev/null | grep -q "$pattern"; then
            echo "$label"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    echo "Timed out waiting for $label" >&2
    tail -120 "$logfile" >&2 || true
    return 1
}

done_count() {
    local logfile="$1"
    grep -c "polling_client: done" "$logfile" 2>/dev/null || true
}

guest_faulted() {
    local logfile="$1"
    local line_offset="$2"

    tail -n "+$line_offset" "$logfile" 2>/dev/null | grep -Eq \
        'BUG: do_page_fault|CMD: /polling_client\.bin'
}

send_client_command() {
    local machine="$1" command="$2" logfile="$3"
    local prev_sz=-1 cur_sz quiet=0 waited=0 try sent

    # Serial output can still be draining when the shell banner appears.
    while [ "$quiet" -lt 2 ] && [ "$waited" -lt 60 ]; do
        cur_sz=$(stat -c%s "$logfile" 2>/dev/null || echo 0)
        if [ "$cur_sz" = "$prev_sz" ]; then
            quiet=$((quiet + 1))
        else
            quiet=0
        fi
        prev_sz="$cur_sz"
        sleep 2
        waited=$((waited + 2))
    done

    for try in 1 2 3; do
        tmux send-keys -t "$SESSION:$machine" "" Enter
        sleep 1
        tmux send-keys -t "$SESSION:$machine" "$command" Enter
        sleep 3
        sent="$(grep -acF "$command" "$logfile" 2>/dev/null || true)"
        if [ "${sent:-0}" -gt 0 ]; then
            return 0
        fi
        echo "Command not echoed on machine $machine (try $try); resending" >&2
    done
    return 1
}

run_client() {
    local machine="$1"
    local mode="$2"
    local command="$3"
    local logfile="$LOG_DIR/machine${machine}.log"
    local before
    local after
    local elapsed=0
    local first_line
    local command_seen=0

    before="$(done_count "$logfile")"
    first_line=$(($(wc -l < "$logfile") + 1))
    echo "=== Running $mode on machine $machine ==="
    send_client_command "$machine" "$command" "$logfile" || {
        echo "Guest shell did not receive command for $mode" >&2
        tail -80 "$logfile" >&2 || true
        return 1
    }

    while [ "$elapsed" -lt "$TIMEOUT" ]; do
        if [ "$command_seen" = "0" ] &&
           tail -n "+$first_line" "$logfile" 2>/dev/null | grep -Fq "$command"; then
            command_seen=1
        fi
        if [ "$command_seen" = "0" ] && [ "$elapsed" -ge "$INPUT_TIMEOUT" ]; then
            echo "Guest shell did not receive command for $mode" >&2
            tail -80 "$logfile" >&2 || true
            return 1
        fi
        after="$(done_count "$logfile")"
        if [ "$after" -gt "$before" ]; then
            grep "\\[SUMMARY\\]" "$logfile" | tail -2 || true
            return 0
        fi
        if guest_faulted "$logfile" "$first_line"; then
            echo "Guest fault while running $mode" >&2
            tail -120 "$logfile" >&2 || true
            return 1
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done

    echo "Timed out while running $mode" >&2
    tail -80 "$logfile" >&2 || true
    return 1
}

check_global_prepare() {
    local cxl_file="/dev/shm/ivshmem-$USER"
    local hostfs_file="/dev/shm/ivshmem-hostfs-$USER"
    local doorbell_socket="/tmp/ivshmem-doorbell-$USER"
    local server_pid_file="/tmp/ivshmem-server-$USER.pid"
    local numa_files=(
        "/dev/shm/numa0.0-$USER"
        "/dev/shm/numa0.1-$USER"
        "/dev/shm/numa1.0-$USER"
        "/dev/shm/numa1.1-$USER"
        "/dev/shm/numa2.0-$USER"
        "/dev/shm/numa2.1-$USER"
        "/dev/shm/numa3.0-$USER"
        "/dev/shm/numa3.1-$USER"
    )
    local f

    echo "=== Checking global AE environment ==="
    for f in "$cxl_file" "$hostfs_file" "$doorbell_socket" "${numa_files[@]}"; do
        if [ ! -e "$f" ]; then
            echo "Missing global AE resource: $f" >&2
            echo "Run ./artifact-evaluation/prepare.sh once before this test." >&2
            return 1
        fi
    done
    if [ ! -f "$server_pid_file" ] || ! ps -p "$(cat "$server_pid_file")" >/dev/null 2>&1; then
        echo "ivshmem doorbell server is not running." >&2
        echo "Run ./artifact-evaluation/prepare.sh before this test." >&2
        return 1
    fi

}

reset_dsm_metadata() {
    echo "=== Resetting DSM metadata before QEMU boot ==="
    make clean-dsm-meta
}

stop_cluster() {
    ae_stop_tmux_and_reap "$SESSION"
}

start_cluster() {
    local mode="$1"
    local machine0_log="$LOG_DIR/machine0.log"
    local machine1_log="$LOG_DIR/machine1.log"
    local machine0_start
    local machine1_start

    stop_cluster || return 1
    if ae_has_chcore_qemu; then
        echo "Refusing to boot $mode: leftover ChCore QEMU still running" >&2
        return 1
    fi
    reset_dsm_metadata

    echo "=== Booting two QEMU machines for $mode ==="
    machine0_start=$(($(wc -l < "$machine0_log") + 1))
    tmux new-session -d -s "$SESSION" -n 0 \
        "MACHINE_NUM=$NUM_MACHINES ./build/simulate.sh 0 2>&1 | tee -a '$machine0_log'"
    wait_for_log_text 0 "DSM] machine 0 " "DSM machine 0 joined" "$machine0_start"
    # The complete shell banner is sometimes split by concurrent serial
    # writes (for example, "Welcome to \nChCore shell!").  Its stable prefix
    # is emitted only after the polling server and shell are ready.
    wait_for_log_text 0 "Welcome to" "Machine 0 shell ready" "$machine0_start"

    machine1_start=$(($(wc -l < "$machine1_log") + 1))
    tmux new-window -t "$SESSION" -n 1 \
        "MACHINE_NUM=$NUM_MACHINES ./build/simulate.sh 1 2>&1 | tee -a '$machine1_log'"
    wait_for_log_text 1 "DSM] machine 1 " "DSM machine 1 joined" "$machine1_start"
    wait_for_log_text 1 "Welcome to" "Machine 1 shell ready" "$machine1_start"
}

run_mode() {
    local machine="$1"
    local mode="$2"
    local command="$3"

    start_cluster "$mode"
    run_client "$machine" "$mode" "$command"
    stop_cluster
}

cd "$REPO_ROOT"

ae_ensure_clean_tmux

check_global_prepare

: > "$LOG_DIR/machine0.log"
: > "$LOG_DIR/machine1.log"

echo "=== Enabling IPC instrumentation for this artifact run ==="
set_define "$CLIENT_SRC" ENABLE_BREAKDOWN 1
set_define "$SERVER_SRC" ENABLE_SRV_TIMING 1
set_define "$RESP_SRC" ENABLE_SRV_TIMING 1
disable_kernel_tests

if [ "$SKIP_BUILD" = "1" ]; then
    echo "=== Skipping build (SKIP_BUILD=1) ==="
else
    ae_build_with_config_restore
fi

# Continue across modes so partial logs still reach plot.py, but remember
# failures so this script (and run_all.py) exit non-zero.
failed=0
run_mode 0 direct_empty "polling_client.bin -d -e -t 1 -m direct_empty" || failed=1
run_mode 0 direct "polling_client.bin -d -t 1 -m direct" || failed=1
run_mode 1 cross_empty "polling_client.bin -s 0 -e -t 1 -m cross_empty" || failed=1
run_mode 1 cross "polling_client.bin -s 0 -t 1 -m cross" || failed=1
run_mode 1 cross_empty_4t "polling_client.bin -s 0 -e -t 4 -m cross_empty_4t" || failed=1
run_mode 1 cross_4t "polling_client.bin -s 0 -t 4 -m cross_4t" || failed=1

echo "=== Parsing logs and generating figures ==="
python3 "$AE_DIR/plot.py" --log-dir "$LOG_DIR" --out-dir "$OUT_DIR"

echo "Artifact output: $OUT_DIR"
if [ "$failed" -ne 0 ]; then
    echo "One or more IPC modes failed; see $LOG_DIR" >&2
    exit 1
fi
