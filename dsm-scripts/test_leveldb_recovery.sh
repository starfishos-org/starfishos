#!/bin/bash
# Test leveldb recovery: machine 0 populates DB with fillbatch, crash, recover on machine 1, verify with readrandom
# Usage: ./dsm-scripts/test_leveldb_recovery.sh

session_name=$USER-qemu
num_windows=2
TIMEOUT=120

# Clean up previous session
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

wait_marker() {
    local logfile=$1
    local marker=$2
    local label=$3
    local elapsed=0
    while [ $elapsed -lt $TIMEOUT ]; do
        if grep -q "$marker" "$logfile" 2>/dev/null; then
            echo "$label"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    echo "=== TIMEOUT waiting for: $marker ==="
    tail -20 "$logfile" 2>/dev/null
    return 1
}

make start-ivshmem-server
make clean-dsm-meta

echo "=== LevelDB Recovery Test ==="
echo "Starting 2 machines..."

# Start machine 0 (hosts tmpfs with p-log)
tmux new -d -s $session_name -n 0 "MACHINE_NUM=$num_windows ./build/simulate.sh 0 | tee exec_log0.log"
sleep 1; dsm_ready 0; kernel_ready 0

# Start machine 1 (recovery machine)
tmux new-window -t $session_name -n 1 "MACHINE_NUM=$num_windows ./build/simulate.sh 1 | tee exec_log1.log"
sleep 1; dsm_ready 1; kernel_ready 1

echo "Both machines ready."

# Phase 1: Machine 0 runs leveldb fillbatch to populate database
echo ""
echo "=== Phase 1: Machine 0 starts LevelDB fillbatch ==="
tmux send -t "$session_name:0" "leveldb-dbbench.bin --benchmarks=fillbatch --num=100000 --db=/tmp/leveldb_test --threads=8" ENTER

# Wait briefly for LevelDB to start and write some entries to the p-log
wait_marker exec_log0.log "fillbatch" "Fillbatch started on machine 0" || exit 1
sleep 5

# Phase 2: Kill machine 0 mid-run (simulate crash)
echo ""
echo "=== Phase 2: Killing machine 0 mid-run (simulating crash) ==="
tmux kill-window -t "$session_name:0"
sleep 1
echo "Machine 0 killed."

# Phase 3: Machine 1 starts recovery tmpfs
echo ""
echo "=== Phase 3: Recovery on machine 1 ==="
tmux send -t "$session_name:1" "tmpfs.srv --recover 0" ENTER

wait_marker exec_log1.log "Replay done" "Recovery complete" || exit 1

sleep 2

# Phase 4: Machine 1 client reads back data from recovered DB
echo ""
echo "=== Phase 4: Machine 1 reads from recovered LevelDB with readrandom ==="
tmux send -t "$session_name:1" "leveldb-dbbench.bin --benchmarks=readrandom --num=100000 --db=/tmp/leveldb_test --threads=8" ENTER

if wait_marker exec_log1.log "readrandom" "Readrandom started on machine 1"; then
    if wait_marker exec_log1.log "micros/op" "Readrandom completed on machine 1"; then
        echo "=== RECOVERY TEST PASSED ==="
        grep "\[LEVELDB\]" exec_log1.log | tail -10
    else
        echo "=== RECOVERY TEST FAILED (readrandom timeout) ==="
    fi
else
    echo "=== RECOVERY TEST FAILED (recovery or readrandom startup failed) ==="
fi

echo ""
echo "=== Test complete ==="
echo "Logs: exec_log0.log (machine 0), exec_log1.log (machine 1)"
