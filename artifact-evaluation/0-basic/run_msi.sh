#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AE_DIR="$REPO_ROOT/artifact-evaluation/0-basic"
OUT_DIR="${OUT_DIR:-$AE_DIR}"
LOG_DIR="${LOG_DIR:-$OUT_DIR/msi-logs}"
SESSION="${SESSION:-${USER}-msi-basic-ae}"
NRUNS="${NRUNS:-1}"
SAMPLES="${SAMPLES:-100}"
TARGET_MACHINE="${TARGET_MACHINE:-1}"
TARGET_CPU="${TARGET_CPU:-4}"
TIMEOUT="${TIMEOUT:-300}"
SKIP_BUILD="${SKIP_BUILD:-0}"
NUM_MACHINES=2

mkdir -p "$LOG_DIR"

stop_cluster() {
    if tmux has-session -t "$SESSION" 2>/dev/null; then
        tmux kill-session -t "$SESSION" || true
    fi
}
trap stop_cluster EXIT

wait_for_log() {
    local logfile="$1" pattern="$2" label="$3" start_line="${4:-1}"
    local elapsed=0

    while [ "$elapsed" -lt "$TIMEOUT" ]; do
        if ! tmux has-session -t "$SESSION" 2>/dev/null; then
            echo "tmux session $SESSION exited while waiting for $label" >&2
            tail -120 "$logfile" >&2 || true
            return 1
        fi
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

launch_machine() {
    local machine="$1" run_log_dir="$2"
    local logfile="$run_log_dir/machine${machine}.log"
    local command="MACHINE_NUM=$NUM_MACHINES ./build/simulate.sh $machine 2>&1 | tee '$logfile'"

    if [ "$machine" -eq 0 ]; then
        tmux new-session -d -s "$SESSION" -n 0 "$command"
    else
        tmux new-window -t "$SESSION" -n "$machine" "$command"
    fi
}

check_prepare() {
    local server_pid_file="/tmp/ivshmem-server-$USER.pid"

    if [ ! -e "/tmp/ivshmem-doorbell-$USER" ] ||
       [ ! -f "$server_pid_file" ] ||
       ! ps -p "$(<"$server_pid_file")" >/dev/null 2>&1; then
        echo "Run ./artifact-evaluation/prepare.sh first." >&2
        return 1
    fi
}

cd "$REPO_ROOT"
source "$REPO_ROOT/artifact-evaluation/common.sh"
ae_ensure_clean_tmux
check_prepare
if [ "$SKIP_BUILD" = "1" ]; then
    echo "=== Skipping build (SKIP_BUILD=1) ==="
else
    echo "=== Building MSI microbenchmark ==="
    ./scripts/chbuild-with-fallback.sh build
fi
test -x "$REPO_ROOT/user/build/ramdisk/msi_latency_microbench.bin"

for run in $(seq 1 "$NRUNS"); do
    run_log_dir="$LOG_DIR/run${run}"
    machine0_log="$run_log_dir/machine0.log"
    mkdir -p "$run_log_dir"
    stop_cluster
    make clean-dsm-meta
    launch_machine 0 "$run_log_dir"
    wait_for_log "$machine0_log" 'DSM] machine 0 ' "run $run: machine 0 joined"
    launch_machine 1 "$run_log_dir"
    wait_for_log "$run_log_dir/machine1.log" 'DSM] machine 1 ' \
        "run $run: machine 1 joined"
    wait_for_log "$machine0_log" 'Welcome to ChCore shell!' \
        "run $run: machine 0 ready"
    wait_for_log "$run_log_dir/machine1.log" 'Welcome to ChCore shell!' \
        "run $run: machine 1 ready"

    first_line=$(($(wc -l < "$machine0_log") + 1))
    command="msi_latency_microbench.bin $SAMPLES $TARGET_MACHINE $TARGET_CPU"
    echo "=== Run $run/$NRUNS: $command ==="
    tmux send-keys -t "$SESSION:0" "$command" Enter
    wait_for_log "$machine0_log" 'MSI_LATENCY_BENCH] DONE' \
        "run $run MSI benchmark completed" "$first_line"
    if tail -n "+$first_line" "$machine0_log" |
       grep -q 'MSI_LATENCY_BENCH] FAILED'; then
        echo "Run $run reported an MSI benchmark failure" >&2
        exit 1
    fi
    stop_cluster
done

python3 "$AE_DIR/parse_msi.py" --log-dir "$LOG_DIR" --out-dir "$OUT_DIR"
echo "Artifact output: $OUT_DIR"
