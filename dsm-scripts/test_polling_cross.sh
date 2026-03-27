#!/bin/bash
# Test cross-machine polling: machine 1's client talks to machine 0's polling server
# Usage: ./dsm-scripts/test_polling_cross.sh [num_threads]

NUM_THREADS=${1:-3}
session_name=$USER-qemu
num_windows=2

# Clean up previous session
if tmux has-session -t $session_name 2>/dev/null; then
    tmux kill-session -t $session_name
    echo "Killed existing tmux session: $session_name"
fi

dsm_ready() {
  echo "DSM machine $1 is joining..."
  tmux select-window -t "$session_name:$1"
  while true; do
      tmux capture-pane -t "$session_name:$1" -pS -1000 | grep -q "DSM] machine $1 "
      if [ $? -eq 0 ]; then break; fi
      sleep 1
  done
  echo "DSM machine $1 joined the cluster"
}

kernel_ready() {
  echo "Kernel $1 is creating..."
  while true; do
      tmux capture-pane -t "$session_name:$1" -pS -1000 | grep -q "Welcome to ChCore shell!"
      if [ $? -eq 0 ]; then break; fi
      sleep 1
  done
  echo "Kernel $1 is ready"
}

make start-ivshmem-server
make clean-dsm-meta

echo "Starting 2 machines..."

# Machine 0 - tee to exec_log0.log
tmux new -d -s $session_name -n 0 "MACHINE_NUM=$num_windows ./build/simulate.sh 0 | tee exec_log0.log"
sleep 1
dsm_ready 0
kernel_ready 0

# Machine 1 - tee to exec_log1.log
tmux new-window -t $session_name -n 1 "MACHINE_NUM=$num_windows ./build/simulate.sh 1 | tee exec_log1.log"
sleep 1
dsm_ready 1
kernel_ready 1

echo "Both machines ready."
echo "Using $NUM_THREADS worker threads."

# Test 1: Local (machine 0 client → machine 0 server)
echo ""
echo "=== Test 1: Local (machine 0, -s 0 -t $NUM_THREADS) ==="
tmux send -t "$session_name:0" "polling_client.bin -s 0 -t $NUM_THREADS" ENTER

timeout=120
elapsed=0
while [ $elapsed -lt $timeout ]; do
    if grep -q "polling_client: done" exec_log0.log 2>/dev/null; then
        echo "=== Local test PASSED ==="
        grep -E "\[SUMMARY\]|\[thread" exec_log0.log
        break
    fi
    sleep 2
    elapsed=$((elapsed + 2))
done
if [ $elapsed -ge $timeout ]; then
    echo "=== Local test TIMEOUT ==="
    tail -30 exec_log0.log 2>/dev/null
    exit 1
fi

sleep 2

# Test 2: Cross-machine (machine 1 client → machine 0 server)
echo ""
echo "=== Test 2: Cross-machine (machine 1 → machine 0, -s 0 -t $NUM_THREADS) ==="
tmux send -t "$session_name:1" "polling_client.bin -s 0 -t $NUM_THREADS" ENTER

timeout=120
elapsed=0
while [ $elapsed -lt $timeout ]; do
    if grep -q "polling_client: done" exec_log1.log 2>/dev/null; then
        echo "=== Cross-machine test PASSED ==="
        grep -E "\[SUMMARY\]|\[thread" exec_log1.log
        echo ""
        echo "Full CDF data in exec_log0.log and exec_log1.log (grep for [CDF])"
        exit 0
    fi
    sleep 2
    elapsed=$((elapsed + 2))
done

echo "TIMEOUT: Cross-machine test did not finish within ${timeout}s"
tail -30 exec_log1.log 2>/dev/null
exit 1
