#!/bin/bash
# Unified N-cluster launcher for DSM simulation.
#
# Interactive mode (launch N QEMU instances and attach tmux):
#   ./dsm-scripts/simulate_ncluster.sh <N> [--build]
#
# Automated test mode (launch, run command, wait for result):
#   ./dsm-scripts/simulate_ncluster.sh <N> <logname> "<cmd>" "<expected_pattern>" [--build] [--timeout=SEC]
#
# Examples:
#   ./dsm-scripts/simulate_ncluster.sh 2
#   ./dsm-scripts/simulate_ncluster.sh 2 mm "source run_matrix_multiply.sh" "matrix multiply finished"
#   ./dsm-scripts/simulate_ncluster.sh 2 graph "pagerank /host/twitter-2010.bin 41652230 50 2" "exec_time=" --timeout=600
#
# CPU_NUM env var (default 12) is forwarded to each machine's simulate.sh,
# same as the standalone simulate_Nclusters.sh scripts.

set -e

# ---- parse arguments ----
N=""
LOGNAME=""
CMD=""
EXPECTED=""
BUILD=false
TIMEOUT=120

positional=()
for arg in "$@"; do
    case "$arg" in
        --build)    BUILD=true ;;
        --timeout=*) TIMEOUT="${arg#--timeout=}" ;;
        *)          positional+=("$arg") ;;
    esac
done

N="${positional[0]:-}"
LOGNAME="${positional[1]:-}"
CMD="${positional[2]:-}"
EXPECTED="${positional[3]:-}"

if [[ -z "$N" ]] || ! [[ "$N" =~ ^[1-8]$ ]]; then
    echo "Usage: $0 <N> [logname] [cmd] [expected_pattern] [--build] [--timeout=SEC]"
    echo "  N: number of QEMU instances (1-8)"
    exit 1
fi

SESSION="$USER-qemu"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_DIR"

# ---- helper ----
wait_for_pattern() {
    local file=$1
    local pattern=$2
    local timeout=$3
    local label=$4
    echo -n "  Waiting for $label..."
    for ((t=0; t<timeout; t++)); do
        if grep -q "$pattern" "$file" 2>/dev/null; then
            echo " OK (${t}s)"
            return 0
        fi
        sleep 1
    done
    echo " TIMEOUT (${timeout}s)"
    return 1
}

# ---- optional build ----
if [[ "$BUILD" == "true" ]]; then
    ./scripts/chbuild-with-fallback.sh build
fi

# ---- cleanup ----
# shellcheck source=../artifact-evaluation/common.sh
source "$REPO_DIR/artifact-evaluation/common.sh"
ae_ensure_clean_tmux

# Drop host page cache so residual pages from old QEMU instances do not hurt performance
sync
echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1 || true

# ---- prepare ----
"$SCRIPT_DIR/start_ivshmem_server.sh"
"$SCRIPT_DIR/config_memdev.sh" cxl
sleep 3

# ---- clear old logs ----
for ((i=0; i<N; i++)); do rm -f "exec_log${i}.log"; done
rm -f exec_log.log

RUN_CMD="MACHINE_NUM=$N CPU_NUM=\${CPU_NUM:-12} ./build/simulate.sh"

# ======== Single instance, interactive (no tmux) ========
if [[ $N -eq 1 && -z "$CMD" ]]; then
    MACHINE_NUM=1 CPU_NUM=${CPU_NUM:-12} ./build/simulate.sh 0 | tee exec_log.log
    "$SCRIPT_DIR/kill_ivshmem_server.sh"
    exit 0
fi

# ======== Automated test mode ========
if [[ -n "$CMD" ]]; then
    echo "=== Launch $N QEMU instances (automated) ==="
    tmux new-session -d -s "$SESSION" -n vm0 "$RUN_CMD 0 | tee exec_log0.log"
    wait_for_pattern "exec_log0.log" "Welcome to ChCore shell!" 120 "machine 0 shell" || {
        echo "FAILED: shell not ready on machine 0"
        tail -10 "exec_log0.log" 2>/dev/null
        exit 1
    }
    for ((i=1; i<N; i++)); do
        sleep 10
        tmux new-window -t "$SESSION" -n "vm${i}" "$RUN_CMD $i | tee exec_log${i}.log"
    done

    # Wait for DSM join
    echo "=== Wait for DSM join ==="
    for ((i=0; i<N; i++)); do
        wait_for_pattern "exec_log${i}.log" "DSM] machine $i " 180 "machine $i DSM join" || {
            echo "FAILED: machine $i did not join DSM"
            tail -10 "exec_log${i}.log" 2>/dev/null
            exit 1
        }
    done

    # Wait for shell ready
    for ((i=0; i<N; i++)); do
        wait_for_pattern "exec_log${i}.log" "Welcome to ChCore shell!" 120 "machine $i shell" || {
            echo "FAILED: shell not ready on machine $i"
            exit 1
        }
    done

    # Send command
    echo "=== Send: $CMD ==="
    tmux send-keys -t "$SESSION:vm0" "$CMD" Enter

    # Wait for expected pattern
    echo -n "=== Waiting for '$EXPECTED'..."
    for ((t=0; t<TIMEOUT; t++)); do
        if grep -q "$EXPECTED" exec_log0.log 2>/dev/null; then
            echo " OK!"
            break
        fi
        sleep 1
    done

    echo ""
    echo "========================================="
    rc=0
    if grep -q "$EXPECTED" exec_log0.log 2>/dev/null; then
        echo "SUCCESS"
        echo "----- Key output -----"
        grep -E "$EXPECTED" exec_log0.log 2>/dev/null || true
        if [[ -n "$LOGNAME" ]]; then
            grep -E "$EXPECTED" exec_log0.log 2>/dev/null >> "$LOGNAME" || true
        fi
    else
        echo "FAILED or TIMEOUT"
        echo "----- Last 40 lines of exec_log0.log -----"
        tail -40 exec_log0.log 2>/dev/null
        rc=1
    fi
    echo "========================================="
    echo "Logs: $(for ((i=0; i<N; i++)); do echo -n "exec_log${i}.log "; done)"
    echo "Attach: tmux a -t $SESSION"
    # Leave tmux/QEMU up for inspection (same as before); propagate failure
    # so Makefile targets like run-mm-test do not treat timeouts as success.
    exit "$rc"
fi

# ======== Interactive mode (tmux panes) ========
echo "=== Launch $N QEMU instances (interactive) ==="
tmux new-session -d -s "$SESSION" -n window0 "$RUN_CMD 0 | tee exec_log0.log"
wait_for_pattern "exec_log0.log" "Welcome to ChCore shell!" 120 "machine 0 shell" || {
    echo "FAILED: shell not ready on machine 0"
    tail -10 "exec_log0.log" 2>/dev/null
    exit 1
}
for ((i=1; i<N; i++)); do
    sleep 5
    tmux split-window -t "$SESSION:window0" "$RUN_CMD $i | tee exec_log${i}.log"
    tmux select-layout -t "$SESSION:window0" tiled
done

tmux select-pane -t "$SESSION:window0.0"
tmux attach -t "$SESSION"

"$SCRIPT_DIR/kill_ivshmem_server.sh"
