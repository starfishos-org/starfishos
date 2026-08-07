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
SKIP_SHELL_WAIT="${SKIP_SHELL_WAIT:-0}"

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

SESSION="${QEMU_SESSION:-$USER-qemu}"
EXEC_LOG0="${EXEC_LOG0:-exec_log0.log}"
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

# Never prompt for sudo in an automated run. Set AE_DROP_CACHES=0 to skip
# sync/cache dropping on shared hosts.
ae_drop_host_caches

# ---- prepare ----
"$SCRIPT_DIR/start_ivshmem_server.sh"
# config_memdev.sh cxl also refreshes CXLFS when the ramdisk build changed.
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
    tmux new-session -d -s "$SESSION" -n vm0 "$RUN_CMD 0 | tee '$EXEC_LOG0'"
    if [[ "$SKIP_SHELL_WAIT" != "1" ]]; then
        # The colored welcome banner can be byte-interleaved with late polling
        # server output on a busy boot. The actual shell prompt is the stable
        # readiness marker used for command injection.
        wait_for_pattern "$EXEC_LOG0" '[$] ' 120 "machine 0 shell prompt" || {
            echo "FAILED: shell not ready on machine 0"
            tail -10 "$EXEC_LOG0" 2>/dev/null
            exit 1
        }
        for ((i=1; i<N; i++)); do
            sleep 10
            tmux new-window -t "$SESSION" -n "vm${i}" "$RUN_CMD $i | tee exec_log${i}.log"
        done

        # Wait for DSM join
        echo "=== Wait for DSM join ==="
        for ((i=0; i<N; i++)); do
            machine_log="exec_log${i}.log"
            [[ $i -eq 0 ]] && machine_log="$EXEC_LOG0"
            wait_for_pattern "$machine_log" "DSM] machine $i " 180 "machine $i DSM join" || {
                echo "FAILED: machine $i did not join DSM"
                tail -10 "$machine_log" 2>/dev/null
                exit 1
            }
        done

        # Wait for shell ready
        for ((i=0; i<N; i++)); do
            machine_log="exec_log${i}.log"
            [[ $i -eq 0 ]] && machine_log="$EXEC_LOG0"
            wait_for_pattern "$machine_log" '[$] ' 120 "machine $i shell prompt" || {
                echo "FAILED: shell not ready on machine $i"
                exit 1
            }
        done

        # Send command
        echo "=== Send: $CMD ==="
        # SeaBIOS/QEMU may deliver a delayed terminal device-attributes reply
        # (for example "1;2c") after the shell prompt.  Let it arrive, then
        # clear the current input line before injecting the benchmark command.
        sleep 2
        tmux send-keys -t "$SESSION:vm0" C-u
        tmux send-keys -t "$SESSION:vm0" "$CMD" Enter
    else
        echo "=== Kernel autostart mode: skip shell wait and command injection ==="
    fi

    # Wait for expected pattern
    echo -n "=== Waiting for '$EXPECTED'..."
    for ((t=0; t<TIMEOUT; t++)); do
        if grep -q "$EXPECTED" "$EXEC_LOG0" 2>/dev/null; then
            echo " OK!"
            break
        fi
        sleep 1
    done

    echo ""
    echo "========================================="
    rc=0
    if grep -q "$EXPECTED" "$EXEC_LOG0" 2>/dev/null; then
        echo "SUCCESS"
        echo "----- Key output -----"
        grep -E "$EXPECTED" "$EXEC_LOG0" 2>/dev/null || true
        if [[ -n "$LOGNAME" ]]; then
            grep -E "$EXPECTED" "$EXEC_LOG0" 2>/dev/null >> "$LOGNAME" || true
        fi
    else
        echo "FAILED or TIMEOUT"
        echo "----- Last 40 lines of $EXEC_LOG0 -----"
        tail -40 "$EXEC_LOG0" 2>/dev/null
        rc=1
    fi
    echo "========================================="
    echo "Machine 0 log: $EXEC_LOG0"
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
