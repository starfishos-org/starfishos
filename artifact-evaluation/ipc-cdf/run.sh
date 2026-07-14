#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AE_DIR="$REPO_ROOT/artifact-evaluation/ipc-cdf"
TS="${TS:-$(date +%Y%m%d_%H%M%S)}"
# NUM_MACHINES=2 reproduces the paper figure; NUM_MACHINES=8 is the
# reviewer-requested variant (same workloads on an 8-machine cluster).
NUM_MACHINES="${NUM_MACHINES:-2}"
OUT_DIR="${OUT_DIR:-$AE_DIR/out/${TS}-m${NUM_MACHINES}}"
LOG_DIR="$OUT_DIR/logs"
SESSION="${SESSION:-${USER}-ipc-ae}"
TIMEOUT="${TIMEOUT:-240}"
KEEP_QEMU="${KEEP_QEMU:-0}"

CLIENT_SRC="$REPO_ROOT/user/system-servers/polling/polling_client_test.c"
SERVER_SRC="$REPO_ROOT/user/system-servers/polling/polling_server.c"
RESP_SRC="$REPO_ROOT/user/system-servers/polling/polling_resp.c"

mkdir -p "$LOG_DIR" "$OUT_DIR/results" "$OUT_DIR/figures"

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
    if [ "$KEEP_QEMU" != "1" ] && tmux has-session -t "$SESSION" 2>/dev/null; then
        tmux kill-session -t "$SESSION" || true
    fi
}
trap cleanup EXIT

set_define() {
    local file="$1"
    local flag="$2"
    local value="$3"
    sed -i "s/^#define ${flag} [01]/#define ${flag} ${value}/" "$file"
}

wait_for_pane_text() {
    local pane="$1"
    local pattern="$2"
    local label="$3"
    local logfile="$LOG_DIR/machine${pane}.log"
    local elapsed=0
    while [ "$elapsed" -lt "$TIMEOUT" ]; do
        if ! tmux has-session -t "$SESSION" 2>/dev/null; then
            echo "tmux session $SESSION exited while waiting for $label" >&2
            tail -120 "$logfile" >&2 || true
            return 1
        fi
        if tmux capture-pane -t "$SESSION:$pane" -pS -3000 | grep -q "$pattern"; then
            echo "$label"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    echo "Timed out waiting for $label" >&2
    tmux capture-pane -t "$SESSION:$pane" -pS -120 >&2 || true
    return 1
}

done_count() {
    local logfile="$1"
    grep -c "polling_client: done" "$logfile" 2>/dev/null || true
}

# Fatal guest-side signatures (same intent as common.sh's AE_ERROR_PATTERN).
IPC_ERROR_PATTERN='General Protection Fault|Kernel panic|kernel panic|panic:|BUG:|BUG_ON|Unhandled .*[Ee]xception|Unhandled .*fault|pool=NULL for va|KERNEL FAULT|Trap No\. '
IPC_ERRORS=()

# run_client never aborts the script: on timeout or guest error it records
# the failure and returns non-zero, but callers use `|| true` so the sweep
# continues with the next mode.
run_client() {
    local machine="$1"
    local mode="$2"
    local command="$3"
    local logfile="$LOG_DIR/machine${machine}.log"
    local before after elapsed=0 err

    before="$(done_count "$logfile")"
    echo "=== Running $mode on machine $machine ==="

    # Wait for the console to quiesce: heavy instrumentation dumps (e.g. the
    # [ST] server-timing flood after each mode) can drop serial input chars,
    # silently eating the next command.
    local prev_sz=-1 cur_sz quiet=0
    while [ "$quiet" -lt 2 ] && [ "$elapsed" -lt "$TIMEOUT" ]; do
        cur_sz=$(stat -c%s "$logfile" 2>/dev/null || echo 0)
        if [ "$cur_sz" = "$prev_sz" ]; then quiet=$((quiet + 1)); else quiet=0; fi
        prev_sz="$cur_sz"
        sleep 2
        elapsed=$((elapsed + 2))
    done

    # Send the command; verify it echoed back, retry a few times if the
    # console dropped it.
    local sent try
    for try in 1 2 3; do
        # flush any pending tty escape-sequence garbage first
        tmux send-keys -t "$SESSION:$machine" "" Enter
        sleep 1
        tmux send-keys -t "$SESSION:$machine" "$command" Enter
        sleep 3
        sent="$(grep -acF "$command" "$logfile" 2>/dev/null || true)"
        if [ "${sent:-0}" -gt 0 ]; then
            break
        fi
        echo "[WARN] command for $mode not echoed (try $try); resending" >&2
        elapsed=$((elapsed + 4))
    done

    while [ "$elapsed" -lt "$TIMEOUT" ]; do
        after="$(done_count "$logfile")"
        if [ "$after" -gt "$before" ]; then
            grep "\\[SUMMARY\\]" "$logfile" | tail -2 || true
            return 0
        fi
        err="$(grep -aE "$IPC_ERROR_PATTERN" "$logfile" 2>/dev/null | head -1)"
        if [ -n "$err" ]; then
            IPC_ERRORS+=("$mode: guest error -> ${err## }")
            echo "[AE][RUN-ERROR] $mode: guest error detected -> ${err## }; skipping to next mode" >&2
            tail -40 "$logfile" >&2 || true
            return 3
        fi
        if ! tmux has-session -t "$SESSION" 2>/dev/null; then
            IPC_ERRORS+=("$mode: tmux session died")
            echo "[AE][RUN-ERROR] $mode: tmux session $SESSION died; skipping to next mode" >&2
            return 3
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done

    IPC_ERRORS+=("$mode: timed out after ${TIMEOUT}s")
    echo "[AE][TIMEOUT-ERROR] $mode timed out after ${TIMEOUT}s; skipping to next mode" >&2
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

    echo "=== Resetting DSM metadata before QEMU boot ==="
    make clean-dsm-meta
}

cd "$REPO_ROOT"

check_global_prepare

echo "=== Enabling IPC instrumentation for this artifact run ==="
set_define "$CLIENT_SRC" ENABLE_BREAKDOWN 1
set_define "$SERVER_SRC" ENABLE_SRV_TIMING 1
set_define "$RESP_SRC" ENABLE_SRV_TIMING 1

echo "=== Building ChCore ==="
./chbuild build

if tmux has-session -t "$SESSION" 2>/dev/null; then
    tmux kill-session -t "$SESSION"
fi

echo "=== Booting $NUM_MACHINES QEMU machines ==="
tmux new-session -d -s "$SESSION" -n 0 "MACHINE_NUM=$NUM_MACHINES ./build/simulate.sh 0 2>&1 | tee '$LOG_DIR/machine0.log'"
wait_for_pane_text 0 "DSM] machine 0 " "DSM machine 0 joined"
wait_for_pane_text 0 "Welcome to ChCore shell!" "Machine 0 shell ready"

for i in $(seq 1 $((NUM_MACHINES - 1))); do
    tmux new-window -t "$SESSION" -n "$i" "MACHINE_NUM=$NUM_MACHINES ./build/simulate.sh $i 2>&1 | tee '$LOG_DIR/machine$i.log'"
    wait_for_pane_text "$i" "DSM] machine $i " "DSM machine $i joined"
done
for i in $(seq 1 $((NUM_MACHINES - 1))); do
    wait_for_pane_text "$i" "Welcome to ChCore shell!" "Machine $i shell ready"
done

# `|| true` so one failing mode does not abort the whole sweep (set -e).
run_client 0 direct_empty "polling_client.bin -d -e -t 1 -m direct_empty" || true
run_client 0 direct "polling_client.bin -d -t 1 -m direct" || true
run_client 1 cross_empty "polling_client.bin -s 0 -e -t 1 -m cross_empty" || true
run_client 1 cross "polling_client.bin -s 0 -t 1 -m cross" || true
run_client 1 cross_empty_4t "polling_client.bin -s 0 -e -t 4 -m cross_empty_4t" || true
run_client 1 cross_4t "polling_client.bin -s 0 -t 4 -m cross_4t" || true

echo "=== Parsing logs and generating figures ==="
python3 "$AE_DIR/parse_and_plot.py" --log-dir "$LOG_DIR" --out-dir "$OUT_DIR"

echo "Artifact output: $OUT_DIR"

if [ "${#IPC_ERRORS[@]}" -gt 0 ]; then
    echo "" >&2
    echo "[AE] ${#IPC_ERRORS[@]} IPC mode(s) FAILED:" >&2
    for e in "${IPC_ERRORS[@]}"; do echo "[AE]   - $e" >&2; done
    exit 2
fi
