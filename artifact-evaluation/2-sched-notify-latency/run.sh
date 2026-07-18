#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AE_DIR="$REPO_ROOT/artifact-evaluation/2-sched-notify-latency"
TS="${TS:-$(date +%Y%m%d_%H%M%S)}"
OUT_DIR="${OUT_DIR:-$AE_DIR/out/$TS}"
LOG_DIR="${LOG_DIR:-$OUT_DIR/logs}"
CSV_DIR="${CSV_DIR:-$OUT_DIR/csv}"
FIG_DIR="${FIG_DIR:-$OUT_DIR/figures}"
SESSION="${SESSION:-${USER}-sched-notify-ae}"
NUM_MACHINES=2
NRUNS="${NRUNS:-3}"
SAMPLES="${SAMPLES:-8}"
GUEST_CPU_NUM="${GUEST_CPU_NUM:-12}"
LOCAL_CPU="${LOCAL_CPU:-4}"
REMOTE_CPU="${REMOTE_CPU:-12}"
TIMEOUT="${TIMEOUT:-300}"
SKIP_BUILD="${SKIP_BUILD:-0}"
KEEP_QEMU="${KEEP_QEMU:-0}"
PROJECT_CONFIG="$REPO_ROOT/.config"

mkdir -p "$LOG_DIR" "$CSV_DIR" "$FIG_DIR"
echo "[AE] Output directory: $OUT_DIR"
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
    local command="cd '$REPO_ROOT' && CPU_NUM=$GUEST_CPU_NUM MACHINE_NUM=$NUM_MACHINES ./build/simulate.sh $machine 2>&1 | tee '$logfile'"

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
source "$REPO_ROOT/artifact-evaluation/common.sh"
ae_ensure_clean_tmux
check_global_prepare
enable_microbench_config

# chcore.ini defaults to 96 vCPUs; microbenchmarks use a smaller guest like
# ipc-cdf to avoid rr_sched budget issues during boot.
ae_prepare_microbench_guest_cpu
GUEST_CPU_NUM="${CPU_NUM:-$GUEST_CPU_NUM}"

if [ "$REMOTE_CPU" -ge "$GUEST_CPU_NUM" ]; then
    echo "[AE] Clamping REMOTE_CPU from $REMOTE_CPU to $((GUEST_CPU_NUM - 1)) "\
"(guest has $GUEST_CPU_NUM CPUs, indices 0..$((GUEST_CPU_NUM - 1)))"
    REMOTE_CPU=$((GUEST_CPU_NUM - 1))
fi
if [ "$LOCAL_CPU" -le 0 ] || [ "$LOCAL_CPU" -ge "$GUEST_CPU_NUM" ]; then
    echo "[AE] LOCAL_CPU=$LOCAL_CPU is invalid for guest CPU count $GUEST_CPU_NUM" >&2
    exit 1
fi
if [ "$LOCAL_CPU" -eq "$REMOTE_CPU" ]; then
    echo "[AE] LOCAL_CPU and REMOTE_CPU must differ" >&2
    exit 1
fi
COMMAND="${COMMAND:-sched_notify_microbench.bin $SAMPLES $LOCAL_CPU $REMOTE_CPU}"

echo "=== Configuration: guest_cpus=$GUEST_CPU_NUM source_cpu=0 "\
"local_cpu=$LOCAL_CPU cross_machine_cpu=$REMOTE_CPU samples=$SAMPLES runs=$NRUNS ==="

if [ "$SKIP_BUILD" = "1" ]; then
    echo "=== Skipping build (SKIP_BUILD=1) ==="
else
    AE_BUILD_POST_RESTORE_HOOK='enable_microbench_config'
    ae_build_with_config_restore \
        "$REPO_ROOT/user/build/ramdisk/sched_notify_microbench.bin"
fi

for run in $(seq 1 "$NRUNS"); do
    run_once "$run"
done

echo "=== Parsing samples and drawing figure ==="
python3 "$AE_DIR/plot.py" --log-dir "$LOG_DIR" --csv-dir "$CSV_DIR" --fig-dir "$FIG_DIR"
echo "Artifact output: $OUT_DIR"

if [ -x "$AE_DIR/run_linux.sh" ]; then
    echo "=== Host Linux sched/notify baseline ==="
    "$AE_DIR/run_linux.sh"
fi
