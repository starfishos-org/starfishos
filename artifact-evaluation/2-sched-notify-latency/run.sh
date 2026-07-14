#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AE_DIR="$REPO_ROOT/artifact-evaluation/2-sched-notify-latency"
OUT_DIR="${OUT_DIR:-$AE_DIR}"
LOG_DIR="${LOG_DIR:-$OUT_DIR/logs}"
SESSION="${SESSION:-${USER}-sched-notify-ae}"
NUM_MACHINES=2
NRUNS="${NRUNS:-3}"
SAMPLES="${SAMPLES:-8}"
LOCAL_CPU="${LOCAL_CPU:-4}"
REMOTE_CPU="${REMOTE_CPU:-12}"
TIMEOUT="${TIMEOUT:-300}"
SKIP_BUILD="${SKIP_BUILD:-0}"
KEEP_QEMU="${KEEP_QEMU:-0}"
PROJECT_CONFIG="$REPO_ROOT/.config"
COMMAND="${COMMAND:-sched_notify_microbench.bin $SAMPLES $LOCAL_CPU $REMOTE_CPU}"

mkdir -p "$LOG_DIR"
CONFIG_BACKUP="$(mktemp)"
cp "$PROJECT_CONFIG" "$CONFIG_BACKUP"

stop_cluster() {
    if tmux has-session -t "$SESSION" 2>/dev/null; then
        tmux kill-session -t "$SESSION" || true
    fi
}

cleanup() {
    if [ "$KEEP_QEMU" != "1" ]; then
        stop_cluster
    fi
    cp "$CONFIG_BACKUP" "$PROJECT_CONFIG"
    rm -f "$CONFIG_BACKUP"
}
trap cleanup EXIT

enable_microbench_config() {
    sed -i 's/^CHCORE_BUILD_SAMPLE_APPS:BOOL=.*/CHCORE_BUILD_SAMPLE_APPS:BOOL=ON/' \
        "$PROJECT_CONFIG"
    sed -i 's/^CHCORE_BUILD_SAMPLE_APPS_APPS:BOOL=.*/CHCORE_BUILD_SAMPLE_APPS_APPS:BOOL=ON/' \
        "$PROJECT_CONFIG"
    grep -q '^CHCORE_BUILD_SAMPLE_APPS:BOOL=ON$' "$PROJECT_CONFIG"
    grep -q '^CHCORE_BUILD_SAMPLE_APPS_APPS:BOOL=ON$' "$PROJECT_CONFIG"
}

check_global_prepare() {
    local resources=(
        "/dev/shm/ivshmem-$USER"
        "/dev/shm/ivshmem-hostfs-$USER"
        "/tmp/ivshmem-doorbell-$USER"
        "/dev/shm/numa0.0-$USER" "/dev/shm/numa0.1-$USER"
        "/dev/shm/numa1.0-$USER" "/dev/shm/numa1.1-$USER"
        "/dev/shm/numa2.0-$USER" "/dev/shm/numa2.1-$USER"
        "/dev/shm/numa3.0-$USER" "/dev/shm/numa3.1-$USER"
    )
    local resource server_pid_file="/tmp/ivshmem-server-$USER.pid"

    for resource in "${resources[@]}"; do
        if [ ! -e "$resource" ]; then
            echo "Missing global AE resource: $resource" >&2
            echo "Run ./artifact-evaluation/prepare.sh first." >&2
            return 1
        fi
    done
    if [ ! -f "$server_pid_file" ] ||
       ! ps -p "$(<"$server_pid_file")" >/dev/null 2>&1; then
        echo "ivshmem doorbell server is not running." >&2
        echo "Run ./artifact-evaluation/prepare.sh first." >&2
        return 1
    fi
}

build_chcore() {
    echo "=== Building sched/notify microbenchmark ==="
    ./quick-build.sh
    test -x "$REPO_ROOT/user/build/ramdisk/sched_notify_microbench.bin"
}

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

start_cluster() {
    local run_log_dir="$1" label="$2"
    mkdir -p "$run_log_dir"
    stop_cluster
    make clean-dsm-meta
    launch_machine 0 "$run_log_dir"
    wait_for_log "$run_log_dir/machine0.log" 'DSM] machine 0 ' "$label: machine 0 joined"
    launch_machine 1 "$run_log_dir"
    wait_for_log "$run_log_dir/machine1.log" 'DSM] machine 1 ' "$label: machine 1 joined"
    wait_for_log "$run_log_dir/machine0.log" 'Welcome to ChCore shell!' "$label: machine 0 ready"
    wait_for_log "$run_log_dir/machine1.log" 'Welcome to ChCore shell!' "$label: machine 1 ready"
}

run_once() {
    local run="$1" run_log_dir="$LOG_DIR/run${run}"
    local machine0_log first_line

    start_cluster "$run_log_dir" "run $run"
    machine0_log="$run_log_dir/machine0.log"
    first_line=$(($(wc -l < "$machine0_log") + 1))
    echo "=== Run $run/$NRUNS: $COMMAND ==="
    tmux send-keys -t "$SESSION:0" "$COMMAND" Enter
    wait_for_log "$machine0_log" '^\[SCHED_NOTIFY_BENCH\] DONE' \
        "run $run microbenchmark completed" "$first_line"

    if tail -n "+$first_line" "$machine0_log" |
       grep -q '^\[SCHED_NOTIFY_BENCH\] FAILED'; then
        echo "Run $run reported a benchmark failure" >&2
        return 1
    fi
    stop_cluster
}

cd "$REPO_ROOT"
check_global_prepare
enable_microbench_config

echo "=== Configuration: source_cpu=0 local_cpu=$LOCAL_CPU "\
"cross_machine_cpu=$REMOTE_CPU samples=$SAMPLES runs=$NRUNS ==="

if [ "$SKIP_BUILD" = "1" ]; then
    echo "=== Skipping build (SKIP_BUILD=1) ==="
else
    build_chcore
fi

for run in $(seq 1 "$NRUNS"); do
    run_once "$run"
done

echo "=== Parsing samples and drawing figure ==="
python3 "$AE_DIR/plot.py" --log-dir "$LOG_DIR" --out-dir "$OUT_DIR"
echo "Artifact output: $OUT_DIR"
