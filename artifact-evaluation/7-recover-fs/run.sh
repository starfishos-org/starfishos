#!/usr/bin/env bash
# Recover a LevelDB instance after actually killing machine 0's QEMU process.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AE_DIR="$REPO_ROOT/artifact-evaluation/7-recover-fs"
OUT_DIR="${OUT_DIR:-$AE_DIR}"
LOG_DIR="${LOG_DIR:-$AE_DIR/logs}"
SESSION="${SESSION:-${USER}-recover-fs-ae}"
NUM_MACHINES=2
TIMEOUT="${TIMEOUT:-600}"
SKIP_BUILD="${SKIP_BUILD:-0}"
KEEP_QEMU="${KEEP_QEMU:-0}"
DB_PATH="${DB_PATH:-/tmp/leveldb_recovery}"
# Each LevelDB value is logged by tmpfs.  The p-log checkpoints CXL-resident
# tmpfs state and truncates automatically at its 4 MiB capacity.
FILL_NUM="${FILL_NUM:-128}"
READ_NUM="${READ_NUM:-1000}"
THREADS="${THREADS:-8}"
CRASH_DELAY="${CRASH_DELAY:-3}"
DETECTOR_INTERVAL="${DETECTOR_INTERVAL:-0.01}"
LOG_POLL_INTERVAL="${LOG_POLL_INTERVAL:-0.05}"
# Pass this explicitly to each tmux pane.  The current recovery image is built
# with device-backed DRAM enabled, so its valid default is 1.
USE_DEV_AS_DRAM="${USE_DEV_AS_DRAM:-1}"

mkdir -p "$OUT_DIR" "$LOG_DIR"
# Logs use stable names and are truncated on every run. They are diagnostic
# state, not timestamped experiment artifacts.
: > "$LOG_DIR/machine0.log"
: > "$LOG_DIR/machine1.log"
: > "$LOG_DIR/machine0-detector.log"

now_ns() { date +%s%N; }
elapsed_ms() { awk -v start="$1" -v end="$2" 'BEGIN { printf "%.3f", (end - start) / 1000000 }'; }

qemu_pid_for_machine() {
    local machine="$1" pane_pid qemu_pid
    pane_pid="$(tmux display-message -p -t "$SESSION:$machine" '#{pane_pid}' 2>/dev/null || true)"
    [ -n "$pane_pid" ] || return 0

    find_qemu_descendant() {
        local pid="$1" child found args
        args="$(ps -p "$pid" -o args= 2>/dev/null || true)"
        if [[ "$args" == *qemu-6.2-system-x86_64* ]]; then
            printf '%s\n' "$pid"
            return 0
        fi
        for child in $(pgrep -P "$pid" 2>/dev/null || true); do
            found="$(find_qemu_descendant "$child")"
            if [ -n "$found" ]; then
                printf '%s\n' "$found"
                return 0
            fi
        done
        return 0
    }

    qemu_pid="$(find_qemu_descendant "$pane_pid")"
    if [ -n "$qemu_pid" ]; then
        printf '%s\n' "$qemu_pid"
    fi
    return 0
}

start_machine_detector() {
    local qemu_pid="$1" detector_log="$LOG_DIR/machine0-detector.log"
    : > "$detector_log"
    (
        printf 'watching_qemu_pid=%s started_ns=%s\n' "$qemu_pid" "$(now_ns)" > "$detector_log"
        while kill -0 "$qemu_pid" 2>/dev/null; do
            sleep "$DETECTOR_INTERVAL"
        done
        printf 'machine0_qemu_exited detected_ns=%s\n' "$(now_ns)" >> "$detector_log"
    ) &
    DETECTOR_PID=$!
}

wait_for_machine_detector() {
    local elapsed=0 max_checks=$((TIMEOUT * 100)) detector_log="$LOG_DIR/machine0-detector.log"
    while [ "$elapsed" -lt "$max_checks" ]; do
        if grep -q '^machine0_qemu_exited detected_ns=' "$detector_log" 2>/dev/null; then
            wait "$DETECTOR_PID" || true
            return 0
        fi
        sleep "$DETECTOR_INTERVAL"
        elapsed=$((elapsed + 1))
    done
    echo 'Timed out waiting for the machine-0 QEMU detector' >&2
    return 1
}

stop_cluster() {
    if tmux has-session -t "$SESSION" 2>/dev/null; then
        local machine pid qemu_pids=""
        for machine in $(tmux list-windows -t "$SESSION" -F '#{window_index}' 2>/dev/null); do
            pid="$(qemu_pid_for_machine "$machine")"
            if [ -n "$pid" ]; then
                qemu_pids="$qemu_pids $pid"
            fi
        done
        tmux kill-session -t "$SESSION" || true
        for pid in $qemu_pids; do
            kill "$pid" 2>/dev/null || true
        done
    fi
}

cleanup() {
    if [ "$KEEP_QEMU" != "1" ]; then
        stop_cluster
    fi
}
trap cleanup EXIT

check_global_prepare() {
    local resources=(
        "/dev/shm/ivshmem-$USER" "/dev/shm/ivshmem-hostfs-$USER"
        "/tmp/ivshmem-doorbell-$USER"
        "/dev/shm/numa0.0-$USER" "/dev/shm/numa0.1-$USER"
        "/dev/shm/numa1.0-$USER" "/dev/shm/numa1.1-$USER"
    )
    local resource
    for resource in "${resources[@]}"; do
        if [ ! -e "$resource" ]; then
            echo "Missing global AE resource: $resource" >&2
            echo "Run ./artifact-evaluation/prepare.sh first." >&2
            return 1
        fi
    done
}

wait_for_log() {
    local logfile="$1" pattern="$2" label="$3" start_line="${4:-1}"
    local checks=0 max_checks
    max_checks="$(awk -v timeout="$TIMEOUT" -v interval="$LOG_POLL_INTERVAL" \
        'BEGIN { printf "%d", timeout / interval }')"
    while [ "$checks" -lt "$max_checks" ]; do
        if ! tmux has-session -t "$SESSION" 2>/dev/null; then
            echo "tmux session $SESSION exited while waiting for $label" >&2
            return 1
        fi
        if tail -n "+$start_line" "$logfile" 2>/dev/null | grep -qE \
            'General Protection Fault|#GP during early boot|Kernel panic|BUG: do_page_fault|Assertion failed:|invalid user access, killing process|Not a valid dynamic program'; then
            echo "Guest fault while waiting for $label" >&2
            tail -120 "$logfile" >&2 || true
            return 1
        fi
        if tail -n "+$start_line" "$logfile" 2>/dev/null | grep -q '\[plog\] P-log full'; then
            echo "Persistent log filled while waiting for $label; reduce FILL_NUM." >&2
            return 1
        fi
        if tail -n "+$start_line" "$logfile" 2>/dev/null | grep -qE "$pattern"; then
            echo "$label"
            return 0
        fi
        sleep "$LOG_POLL_INTERVAL"
        checks=$((checks + 1))
    done
    echo "Timed out waiting for $label ($pattern)" >&2
    tail -120 "$logfile" >&2 || true
    return 1
}

launch_machine() {
    local machine="$1" logfile
    logfile="$LOG_DIR/machine${machine}.log"
    local command="cd '$REPO_ROOT' && USE_DEV_AS_DRAM=$USE_DEV_AS_DRAM MACHINE_NUM=$NUM_MACHINES ./build/simulate.sh $machine 2>&1 | tee '$logfile'"
    if [ "$machine" -eq 0 ]; then
        tmux new-session -d -s "$SESSION" -n 0 "$command"
    else
        tmux new-window -t "$SESSION" -n "$machine" "$command"
    fi
}

start_cluster() {
    stop_cluster
    # QEMU's peer ID is allocated by ivshmem-server.  Restart it for this
    # self-contained evaluation so stale dead connections cannot hand a new
    # guest an out-of-range peer ID and corrupt DSM state during boot.
    make start-ivshmem-server
    make clean-dsm-meta
    : > "$LOG_DIR/machine0.log"
    : > "$LOG_DIR/machine1.log"
    launch_machine 0
    wait_for_log "$LOG_DIR/machine0.log" 'DSM] machine 0 ' 'machine 0 joined'
    # The first guest initializes the shared CXL allocator.  Do not let the
    # second guest attach midway through that initialization.
    wait_for_log "$LOG_DIR/machine0.log" '^\$ ' 'machine 0 ready'
    launch_machine 1
    wait_for_log "$LOG_DIR/machine1.log" 'DSM] machine 1 ' 'machine 1 joined'
    wait_for_log "$LOG_DIR/machine1.log" '^\$ ' 'machine 1 ready'
    MACHINE0_QEMU_PID="$(qemu_pid_for_machine 0)"
    if [ -z "$MACHINE0_QEMU_PID" ]; then
        echo 'Could not locate machine 0 QEMU for the external detector.' >&2
        return 1
    fi
    echo "[AE] External detector will watch machine 0 QEMU PID $MACHINE0_QEMU_PID"
}

build_chcore() {
    local config_snapshot
    config_snapshot="$(mktemp)"
    cp "$REPO_ROOT/.config" "$config_snapshot"

    echo '=== Building LevelDB recovery artifact ==='
    if ./chbuild build; then
        rm -f "$config_snapshot"
        return 0
    fi
    echo '=== chbuild failed; retrying with scripts/quick-build.sh ===' >&2
    if ! ./scripts/quick-build.sh; then
        rm -f "$config_snapshot"
        return 1
    fi
    cp "$config_snapshot" "$REPO_ROOT/.config"
    rm -f "$config_snapshot"
    ./chbuild build
    test -x "$REPO_ROOT/user/build/ramdisk/leveldb-dbbench.bin"
}

benchmark_rate_ops() {
    # db_bench prints: "<benchmark> : X micros/op". Select the requested
    # benchmark from this invocation and convert it to ops/s.
    local logfile="$1" benchmark="$2" start_line="${3:-1}"
    tail -n "+$start_line" "$logfile" |
        sed -nE "/${benchmark}[[:space:]]*:/ s/.*${benchmark}[[:space:]]*: *([0-9.]+) micros\/op.*/\1/p" |
        tail -1 |
        awk '{ if ($1 > 0) printf "%.3f", 1000000 / $1; }'
}

require_rate() {
    local name="$1" value="$2"
    if [ -z "$value" ]; then
        echo "Could not extract the $name db_bench throughput." >&2
        exit 1
    fi
}

timeline_ms() {
    # Keep the real pre-crash observation interval on the paper-style x axis.
    awk -v lead="$CRASH_DELAY" -v elapsed="$1" \
        'BEGIN { printf "%.3f", lead * 1000 + elapsed }'
}

cd "$REPO_ROOT"
check_global_prepare
if [ "$SKIP_BUILD" != "1" ]; then
    build_chcore
fi

echo "[AE] Result directory: $OUT_DIR"
echo "[AE] Log directory: $LOG_DIR"
echo "[AE] DB: $DB_PATH; fill entries: $FILL_NUM; read entries: $READ_NUM"
echo "[AE] USE_DEV_AS_DRAM: $USE_DEV_AS_DRAM"
start_cluster

# Build the database and measure both write and read throughput before the
# crash.  readrandom uses FILL_NUM as its key space and READ_NUM as its sample
# count, so it does not probe keys outside the populated database.
m0_log="$LOG_DIR/machine0.log"
pre_fill_line=$(($(wc -l < "$m0_log") + 1))
tmux send-keys -t "$SESSION:0" "leveldb-dbbench.bin --benchmarks=fillbatch,readrandom --num=$FILL_NUM --reads=$READ_NUM --db=$DB_PATH --threads=$THREADS" Enter
wait_for_log "$m0_log" 'fillbatch.*micros/op' 'machine 0 database populated' "$pre_fill_line"
pre_fill_ops="$(benchmark_rate_ops "$m0_log" fillbatch "$pre_fill_line")"
require_rate 'pre-crash fill' "$pre_fill_ops"

wait_for_log "$m0_log" 'readrandom.*micros/op' 'machine 0 read baseline completed' "$pre_fill_line"
pre_read_ops="$(benchmark_rate_ops "$m0_log" readrandom "$pre_fill_line")"
require_rate 'pre-crash read' "$pre_read_ops"

sleep "$CRASH_DELAY"
start_machine_detector "$MACHINE0_QEMU_PID"
crash_start_ns="$(now_ns)"
echo '=== Killing machine 0 QEMU (simulated machine failure) ==='
# kill-window terminates the QEMU process, rather than merely stopping the
# guest workload.  machine 1 remains alive in the same DSM cluster.
tmux kill-window -t "$SESSION:0"
crash_end_ns="$(now_ns)"
wait_for_machine_detector
detect_end_ns="$(sed -nE 's/^machine0_qemu_exited detected_ns=([0-9]+)$/\1/p' \
    "$LOG_DIR/machine0-detector.log" | tail -1)"
if [ -z "$detect_end_ns" ]; then
    echo 'External detector did not record a machine-0 QEMU exit timestamp.' >&2
    exit 1
fi
echo "[AE] External detector confirmed machine 0 QEMU exit in $(elapsed_ms "$crash_start_ns" "$detect_end_ns") ms"

m1_log="$LOG_DIR/machine1.log"
fs_line=$(($(wc -l < "$m1_log") + 1))
fs_start_ns="$(now_ns)"
tmux send-keys -t "$SESSION:1" 'cxlfs.srv --recover 0 &' Enter
wait_for_log "$m1_log" '^\[TIMING\] fs_recovery_total:' 'filesystem recovery complete' "$fs_line"
fs_end_ns="$(now_ns)"
fs_total_ms="$(sed -nE 's/.*\[TIMING\] fs_recovery_total: ([0-9]+) ms.*/\1/p' "$m1_log" | tail -1)"
plog_ms="$(sed -nE 's/.*\[TIMING\] plog_recovery: ([0-9]+) ms.*/\1/p' "$m1_log" | tail -1)"

leveldb_line=$(($(wc -l < "$m1_log") + 1))
leveldb_start_ns="$(now_ns)"
tmux send-keys -t "$SESSION:1" "leveldb-dbbench.bin --benchmarks=readrandom,overwrite --use_existing_db=1 --num=$FILL_NUM --reads=$READ_NUM --db=$DB_PATH --threads=$THREADS" Enter
wait_for_log "$m1_log" '\[TIMING\] leveldb_restart \(open existing db\):' 'LevelDB reopened on machine 1' "$leveldb_line"
leveldb_ready_ns="$(now_ns)"
wait_for_log "$m1_log" 'readrandom.*micros/op' 'machine 1 recovery read completed' "$leveldb_line"
post_read_end_ns="$(now_ns)"
leveldb_open_ms="$(sed -nE 's/.*\[TIMING\] leveldb_restart \(open existing db\): ([0-9]+) ms.*/\1/p' "$m1_log" | tail -1)"
post_read_ops="$(benchmark_rate_ops "$m1_log" readrandom "$leveldb_line")"
require_rate 'post-recovery read' "$post_read_ops"

# fillbatch intentionally refuses --use_existing_db.  The second benchmark in
# this same recovered process is overwrite, which exercises the LevelDB write
# path without deleting the database.  The CSV/figure label it "fill".
wait_for_log "$m1_log" 'overwrite.*micros/op' 'machine 1 recovery fill completed' "$leveldb_line"
post_fill_end_ns="$(now_ns)"
post_fill_ops="$(benchmark_rate_ops "$m1_log" overwrite "$leveldb_line")"
require_rate 'post-recovery fill' "$post_fill_ops"

kill_elapsed_ms="$(elapsed_ms "$crash_start_ns" "$crash_end_ns")"
detect_elapsed_ms="$(elapsed_ms "$crash_start_ns" "$detect_end_ns")"
fs_elapsed_ms="$(elapsed_ms "$crash_start_ns" "$fs_end_ns")"
leveldb_elapsed_ms="$(elapsed_ms "$crash_start_ns" "$leveldb_ready_ns")"
read_elapsed_ms="$(elapsed_ms "$crash_start_ns" "$post_read_end_ns")"
fill_elapsed_ms="$(elapsed_ms "$crash_start_ns" "$post_fill_end_ns")"

cat > "$OUT_DIR/recovery_detail.csv" <<CSV
stage,measured_ms,guest_reported_ms,description
kill_machine0,$kill_elapsed_ms,,Host issued tmux kill-window for machine 0 QEMU
detect_machine0,$detect_elapsed_ms,,External host detector observed machine 0 QEMU exit
restart_fs,$(elapsed_ms "$fs_start_ns" "$fs_end_ns"),${fs_total_ms:-},Machine 1 ran cxlfs.srv --recover 0 as a shell background service
plog_replay,,${plog_ms:-},P-log replay reported by tmpfs
restart_leveldb,$(elapsed_ms "$leveldb_start_ns" "$leveldb_ready_ns"),${leveldb_open_ms:-},Machine 1 reopened existing LevelDB database
CSV

cat > "$OUT_DIR/throughput.csv" <<CSV
event,elapsed_ms,workload,ops_per_sec
pre_crash,0,fill,$pre_fill_ops
pre_crash,0,read,$pre_read_ops
crash_started,$(timeline_ms 0),fill,0
crash_started,$(timeline_ms 0),read,0
machine0_killed,$(timeline_ms "$kill_elapsed_ms"),fill,0
machine0_killed,$(timeline_ms "$kill_elapsed_ms"),read,0
machine0_detected,$(timeline_ms "$detect_elapsed_ms"),fill,0
machine0_detected,$(timeline_ms "$detect_elapsed_ms"),read,0
fs_recovered,$(timeline_ms "$fs_elapsed_ms"),fill,0
fs_recovered,$(timeline_ms "$fs_elapsed_ms"),read,0
leveldb_reopened,$(timeline_ms "$leveldb_elapsed_ms"),fill,0
leveldb_reopened,$(timeline_ms "$leveldb_elapsed_ms"),read,0
post_read_completed,$(timeline_ms "$read_elapsed_ms"),fill,0
post_read_completed,$(timeline_ms "$read_elapsed_ms"),read,$post_read_ops
post_fill_completed,$(timeline_ms "$fill_elapsed_ms"),fill,$post_fill_ops
post_fill_completed,$(timeline_ms "$fill_elapsed_ms"),read,$post_read_ops
CSV

MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/matplotlib-$USER}" \
    python3 "$AE_DIR/plot.py" --detail "$OUT_DIR/recovery_detail.csv" \
    --throughput "$OUT_DIR/throughput.csv" --out-dir "$OUT_DIR"

echo "[AE] LevelDB DB::Open on machine 1: ${leveldb_open_ms:-unknown} ms"
echo "[AE] Fill throughput: pre=$pre_fill_ops ops/s post=$post_fill_ops ops/s"
echo "[AE] Read throughput: pre=$pre_read_ops ops/s post=$post_read_ops ops/s"
echo "[AE] Figure: $OUT_DIR/recovery-performance-single.png"
