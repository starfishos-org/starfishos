#!/bin/bash
# Test polling service: direct IPC vs local polling vs cross-machine polling
# Usage: ./dsm-scripts/test_polling_cross.sh [num_threads]

NUM_THREADS=${1:-3}
session_name=$USER-qemu
num_windows=2
TIMEOUT=180

# Clean up
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

wait_done() {
    local logfile=$1
    local label=$2
    local elapsed=0
    while [ $elapsed -lt $TIMEOUT ]; do
        if grep -q "polling_client: done" "$logfile" 2>/dev/null; then
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

echo "Starting 2 machines, $NUM_THREADS threads per test..."

tmux new -d -s $session_name -n 0 "MACHINE_NUM=$num_windows ./build/simulate.sh 0 | tee exec_log0.log"
sleep 1; dsm_ready 0; kernel_ready 0

tmux new-window -t $session_name -n 1 "MACHINE_NUM=$num_windows ./build/simulate.sh 1 | tee exec_log1.log"
sleep 1; dsm_ready 1; kernel_ready 1

echo "Both machines ready."

# ---- Test 1: Direct IPC (machine 0, no polling queue) ----
echo ""
echo "=== Test 1: Direct IPC on machine 0 ==="
# Truncate marker in log so we detect the NEW "done"
tmux send -t "$session_name:0" "polling_client.bin -d -t $NUM_THREADS -m direct" ENTER
wait_done exec_log0.log "Direct IPC"

sleep 2

# ---- Test 2: Local polling (machine 0 client -> machine 0 server) ----
echo ""
echo "=== Test 2: Local Polling (machine 0 -> machine 0) ==="
# We need a fresh log search - use a unique marker
tmux send -t "$session_name:0" "polling_client.bin -s 0 -t $NUM_THREADS -m local" ENTER
# Wait by checking for second "done"
elapsed=0
while [ $elapsed -lt $TIMEOUT ]; do
    cnt=$(grep -c "polling_client: done" exec_log0.log 2>/dev/null)
    if [ "$cnt" -ge 2 ]; then
        echo "=== Local Polling PASSED ==="
        grep "\[SUMMARY\]" exec_log0.log | tail -2
        break
    fi
    sleep 2
    elapsed=$((elapsed + 2))
done
if [ $elapsed -ge $TIMEOUT ]; then
    echo "=== Local Polling TIMEOUT ==="
    tail -20 exec_log0.log
fi

sleep 2

# ---- Test 3: Cross-machine polling (machine 1 client -> machine 0 server) ----
echo ""
echo "=== Test 3: Cross-machine Polling (machine 1 -> machine 0) ==="
tmux send -t "$session_name:1" "polling_client.bin -s 0 -t $NUM_THREADS -m cross" ENTER
wait_done exec_log1.log "Cross-machine Polling"

sleep 2

# ---- Test 4: Direct empty IPC (machine 0, no file I/O) ----
echo ""
echo "=== Test 4: Direct Empty IPC (machine 0) ==="
tmux send -t "$session_name:0" "polling_client.bin -d -e -t $NUM_THREADS -m direct_empty" ENTER
elapsed=0
cnt_before=$(grep -c "polling_client: done" exec_log0.log 2>/dev/null || echo 0)
while [ $elapsed -lt $TIMEOUT ]; do
    cnt_after=$(grep -c "polling_client: done" exec_log0.log 2>/dev/null || echo 0)
    if [ "$cnt_after" -gt "$cnt_before" ]; then
        echo "=== Direct Empty IPC PASSED ==="
        grep "\[SUMMARY\]" exec_log0.log | tail -1
        break
    fi
    sleep 2
    elapsed=$((elapsed + 2))
done
if [ $elapsed -ge $TIMEOUT ]; then
    echo "=== Direct Empty IPC TIMEOUT ==="
    tail -20 exec_log0.log
fi

echo ""
echo "=== All tests complete ==="
echo "Logs: exec_log0.log (machine 0), exec_log1.log (machine 1)"
echo "Run: python3 dsm-scripts/ipc-test/analyze_polling.py exec_log0.log exec_log1.log"
echo "Compare: direct vs direct_empty vs local_polling vs cross_polling"
