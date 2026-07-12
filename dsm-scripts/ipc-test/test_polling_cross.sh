#!/bin/bash
# Test polling service: direct IPC vs cross-machine polling
# Usage: ./dsm-scripts/ipc-test/test_polling_cross.sh [--breakdown]
#   --breakdown  enable client + server timing breakdown (slower)
#   (default)    CDF only, no breakdown overhead

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLIENT_SRC="$REPO_ROOT/user/system-servers/polling/polling_client_test.c"
SERVER_SRC="$REPO_ROOT/user/system-servers/polling/polling_server.c"
RESP_SRC="$REPO_ROOT/user/system-servers/polling/polling_resp.c"

set_flag() {
    local file=$1 flag=$2 val=$3
    sed -i "s/^#define ${flag} [01]/#define ${flag} ${val}/" "$file"
}

# Parse args
BREAKDOWN=0
for arg in "$@"; do
    [ "$arg" = "--breakdown" ] && BREAKDOWN=1
done

# Configure flags and rebuild
echo "=== Configuring flags (breakdown=$BREAKDOWN) ==="
set_flag "$CLIENT_SRC" ENABLE_BREAKDOWN $BREAKDOWN
set_flag "$SERVER_SRC" ENABLE_SRV_TIMING $BREAKDOWN
set_flag "$RESP_SRC"   ENABLE_SRV_TIMING $BREAKDOWN

cd "$REPO_ROOT"
echo "=== Building ==="
./chbuild build 2>&1 | grep -E "error:|Succeeded|Failed" | head -5

session_name=$USER-qemu
num_windows=2
TIMEOUT=180

if tmux has-session -t $session_name 2>/dev/null; then
    tmux kill-session -t $session_name
    echo "Killed existing tmux session: $session_name"
fi

dsm_ready() {
    tmux select-window -t "$session_name:$1"
    while ! tmux capture-pane -t "$session_name:$1" -pS -1000 | grep -q "DSM] machine $1 "; do sleep 1; done
    echo "DSM machine $1 joined"
}

kernel_ready() {
    while ! tmux capture-pane -t "$session_name:$1" -pS -1000 | grep -q "Welcome to ChCore shell!"; do sleep 1; done
    echo "Kernel $1 ready"
}

wait_new_done() {
    local logfile=$1 label=$2 cnt_before=$3 elapsed=0
    while [ $elapsed -lt $TIMEOUT ]; do
        cnt_after=$(grep -c "polling_client: done" "$logfile" 2>/dev/null || echo 0)
        if [ "$cnt_after" -gt "$cnt_before" ]; then
            echo "=== $label PASSED ==="
            grep "\[SUMMARY\]" "$logfile" | tail -2
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    echo "=== $label TIMEOUT ==="
    tail -20 "$logfile" 2>/dev/null
    return 1
}

make start-ivshmem-server
make clean-dsm-meta

echo "Starting 2 machines..."
tmux new -d -s $session_name -n 0 "MACHINE_NUM=$num_windows ./build/simulate.sh 0 | tee exec_log0.log"
sleep 1; dsm_ready 0; kernel_ready 0

tmux new-window -t $session_name -n 1 "MACHINE_NUM=$num_windows ./build/simulate.sh 1 | tee exec_log1.log"
sleep 1; dsm_ready 1; kernel_ready 1

echo "Both machines ready."

---- [1] Direct empty (t=1) ----
echo ""; echo "=== [1/9] Direct empty IPC (machine 0, t=1) ==="
c0=$(grep -c "polling_client: done" exec_log0.log 2>/dev/null || echo 0)
tmux send -t "$session_name:0" "polling_client.bin -d -e -t 1 -m direct_empty" ENTER
wait_new_done exec_log0.log "direct_empty" $c0; sleep 2

# # ---- [2] Direct read (t=1) ----
# echo ""; echo "=== [2/9] Direct IPC read 4KiB (machine 0, t=1) ==="
# c0=$(grep -c "polling_client: done" exec_log0.log 2>/dev/null || echo 0)
# tmux send -t "$session_name:0" "polling_client.bin -d -t 1 -m direct" ENTER
# wait_new_done exec_log0.log "direct" $c0; sleep 2

# ---- [3] Cross empty (t=1) ----
echo ""; echo "=== [3/9] Cross-machine polling empty (machine 1 -> machine 0, t=1) ==="
c1=$(grep -c "polling_client: done" exec_log1.log 2>/dev/null || echo 0)
tmux send -t "$session_name:1" "polling_client.bin -s 0 -e -t 1 -m cross_empty" ENTER
wait_new_done exec_log1.log "cross_empty" $c1; sleep 2

# # ---- [4] Cross read (t=1) ----
# echo ""; echo "=== [4/9] Cross-machine polling read 4KiB (machine 1 -> machine 0, t=1) ==="
# c1=$(grep -c "polling_client: done" exec_log1.log 2>/dev/null || echo 0)
# tmux send -t "$session_name:1" "polling_client.bin -s 0 -t 1 -m cross" ENTER
# wait_new_done exec_log1.log "cross" $c1; sleep 2

# # ---- [5] Cross empty (t=4) ----
# echo ""; echo "=== [5/9] Cross-machine polling empty (machine 1 -> machine 0, t=4) ==="
# c1=$(grep -c "polling_client: done" exec_log1.log 2>/dev/null || echo 0)
# tmux send -t "$session_name:1" "polling_client.bin -s 0 -e -t 4 -m cross_empty_4t" ENTER
# wait_new_done exec_log1.log "cross_empty_4t" $c1; sleep 2

# # ---- [6] Cross read (t=4) ----
# echo ""; echo "=== [6/9] Cross-machine polling read 4KiB (machine 1 -> machine 0, t=4) ==="
# c1=$(grep -c "polling_client: done" exec_log1.log 2>/dev/null || echo 0)
# tmux send -t "$session_name:1" "polling_client.bin -s 0 -t 4 -m cross_4t" ENTER
# wait_new_done exec_log1.log "cross_4t" $c1; sleep 2

# ---- [1] Direct empty (t=4) ----
echo ""; echo "=== [1/9] Direct empty IPC (machine 0, t=1) ==="
c0=$(grep -c "polling_client: done" exec_log0.log 2>/dev/null || echo 0)
tmux send -t "$session_name:0" "polling_client.bin -d -e -t 4 -m direct_empty" ENTER
wait_new_done exec_log0.log "direct_empty" $c0; sleep 2

# ---- [7] Local write (t=4) ----
echo ""; echo "=== [7/9] Local polling write 4KiB (machine 0, t=4) ==="
c0=$(grep -c "polling_client: done" exec_log0.log 2>/dev/null || echo 0)
tmux send -t "$session_name:0" "polling_client.bin -s 0 -w -t 4 -m local_write_4t" ENTER
wait_new_done exec_log0.log "local_write_4t" $c0; sleep 2

# ---- [8] Cross write (t=1) ----
echo ""; echo "=== [8/9] Cross-machine polling write 4KiB (machine 1 -> machine 0, t=1) ==="
c1=$(grep -c "polling_client: done" exec_log1.log 2>/dev/null || echo 0)
tmux send -t "$session_name:1" "polling_client.bin -s 0 -w -t 1 -m cross_write" ENTER
wait_new_done exec_log1.log "cross_write" $c1; sleep 2

# ---- [9] Cross write (t=4) ----
echo ""; echo "=== [9/9] Cross-machine polling write 4KiB (machine 1 -> machine 0, t=4) ==="
c1=$(grep -c "polling_client: done" exec_log1.log 2>/dev/null || echo 0)
tmux send -t "$session_name:1" "polling_client.bin -s 0 -w -t 4 -m cross_write_4t" ENTER
wait_new_done exec_log1.log "cross_write_4t" $c1

echo ""; echo "=== All tests complete ==="

# Save raw logs with timestamp
TS=$(date +%Y%m%d_%H%M%S)
LOGDIR="$REPO_ROOT/dsm-scripts/ipc-test/logs/${TS}_bd${BREAKDOWN}"
mkdir -p "$LOGDIR"
cp exec_log0.log "$LOGDIR/machine0.log"
cp exec_log1.log "$LOGDIR/machine1.log"
echo "Raw logs saved to $LOGDIR/"

echo ""; echo "Summary:"
grep "\[SUMMARY\].*p50=" exec_log0.log exec_log1.log
