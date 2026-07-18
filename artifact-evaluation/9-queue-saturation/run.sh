#!/usr/bin/env bash
#
# Camera-ready revision plan (Reviewer B on paper Figure 11b): tail latency
# and saturation throughput per service queue.
#
# Sweeps client concurrency against the remote polling service queue on a
# two-machine cluster (client on machine 1, service on machine 0) for two
# request services sharing the CXL durable queue:
#
#   empty  - POLLING_REQ_EMPTY: queue-only no-op service (raw queue cost)
#   read   - POLLING_FS_REQ_READ: 4 KiB read served by the tmpfs-backed
#            polling FS service
#
# Each point reports the client-side latency percentiles ([SUMMARY], tail
# latency) and the aggregate wall clock ([TPUT], throughput).  plot.py turns
# the sweep into throughput-vs-load and p99-vs-throughput saturation curves.
#
# Usage (from repo root):
#   ./artifact-evaluation/prepare.sh          # once
#   ./artifact-evaluation/9-queue-saturation/run.sh
#
# Env overrides:
#   THREADS="1 2 4 8"   QUEUES="empty read"   ITERS=20000   TIMEOUT=600
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/9-queue-saturation"
ae_init_output_dirs "$AE_DIR"
AE_LOG_DIR="$LOG_DIR"

NUM_MACHINES=2
# Small guest as in ipc-cdf: large CPU counts have triggered rr_sched budget
# BUGs during boot, and the client spin-waits so THREADS must stay below the
# per-guest vCPU count.
QSAT_CPU_NUM="${QSAT_CPU_NUM:-${AE_MICROBENCH_GUEST_CPU_NUM:-12}}"
THREADS="${THREADS:-1 2 4 8}"
QUEUES="${QUEUES:-empty read}"
ITERS="${ITERS:-20000}"
TIMEOUT="${TIMEOUT:-600}"
SKIP_BUILD="${SKIP_BUILD:-0}"

for t in $THREADS; do
    if ! [[ "$t" =~ ^[1-9][0-9]*$ ]]; then
        echo "THREADS entries must be positive integers: $t" >&2
        exit 1
    fi
    if [ "$t" -ge "$QSAT_CPU_NUM" ]; then
        echo "THREADS entry $t must stay below the guest vCPU count ($QSAT_CPU_NUM):" >&2
        echo "the polling client spin-waits and oversubscribed guests distort tails." >&2
        exit 1
    fi
done
for q in $QUEUES; do
    case "$q" in
        empty|read) ;;
        *) echo "Unknown QUEUES entry: $q (expected: empty read)" >&2; exit 1 ;;
    esac
done
if ! [[ "$ITERS" =~ ^[1-9][0-9]*$ ]]; then
    echo "ITERS must be a positive integer: $ITERS" >&2
    exit 1
fi

ae_acquire_run_lock "queue-saturation" || exit 1

cleanup() {
    local rc=$?
    trap - EXIT
    # Preserve whatever serial output exists, even when a point failed.
    cp "$(ae_machine_log 0)" "$AE_LOG_DIR/machine0.log" 2>/dev/null || true
    cp "$(ae_machine_log 1)" "$AE_LOG_DIR/machine1.log" 2>/dev/null || true
    ae_kill_cluster || rc=1
    ae_restore_build_configs || rc=1
    exit "$rc"
}
trap cleanup EXIT

cd "$AE_REPO_ROOT"
ae_ensure_clean_tmux
ae_check_global_prepare
ae_save_build_configs
ae_set_paper_guest_cpu_config "$QSAT_CPU_NUM"
ae_export_guest_cpu_num "$QSAT_CPU_NUM"
# Kernel malloc tests print for minutes before the shell; keep them off.
ae_set_dotconfig CHCORE_KERNEL_TEST BOOL OFF
# Match the run_all.py baseline: the saturation figure is not an allocator
# experiment, so stay on the conventional CXL buddy backend.
ae_set_dsm_var DSM_CXL_LF_BUDDY OFF

if [ "$SKIP_BUILD" = "1" ]; then
    echo "=== Skipping build (SKIP_BUILD=1) ==="
else
    ae_build_with_config_restore
fi

# wait_for_done_count <machine> <count> <label>
# The client log accumulates across sweep points, so completion is the Nth
# occurrence of the client's final marker rather than its first appearance.
wait_for_done_count() {
    local machine="$1" want="$2" label="$3"
    local logfile elapsed=0 done_count err
    logfile="$(ae_machine_log "$machine")"
    while [ "$elapsed" -lt "$TIMEOUT" ]; do
        err="$(grep -aEo "$AE_ERROR_PATTERN" "$logfile" 2>/dev/null | head -1 || true)"
        if [ -n "$err" ]; then
            ae_record_error "$label: guest error -> $err (log: $logfile)"
            tail -40 "$logfile" >&2 || true
            return 3
        fi
        if ! tmux list-panes -t "$AE_SESSION:$machine" >/dev/null 2>&1; then
            ae_record_error "$label: tmux window $AE_SESSION:$machine died (log: $logfile)"
            tail -40 "$logfile" >&2 || true
            return 3
        fi
        done_count="$(grep -ac 'polling_client: done' "$logfile" 2>/dev/null || true)"
        if [ "${done_count:-0}" -ge "$want" ]; then
            echo "$label"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    ae_record_timeout "$label ('polling_client: done' #$want not seen within ${TIMEOUT}s in $logfile)"
    tail -60 "$logfile" >&2 || true
    return 1
}

# The serial console can interleave the "booting polling server" banner with
# concurrent output (observed live: lwip split the line on machine 0), so
# poll a loose marker and never record a failure — the first client run is
# the real functional check for the service queue.
soft_wait_polling_server() {
    local machine="$1" elapsed=0 logfile
    logfile="$(ae_machine_log "$machine")"
    while [ "$elapsed" -lt 60 ]; do
        if grep -aq "polling server" "$logfile" 2>/dev/null; then
            echo "machine $machine polling server marker seen"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    echo "[WARN] no polling-server marker on machine $machine after 60s;" \
         "relying on the first client run" >&2
    return 0
}

echo "=== Booting the two-machine saturation cluster (cpu=${QSAT_CPU_NUM}) ==="
if ! ae_boot_cluster "$NUM_MACHINES" "$QSAT_CPU_NUM"; then
    ae_record_error "boot failed for queue-saturation cluster"
    ae_finish
fi
soft_wait_polling_server 0
soft_wait_polling_server 1

expected_done=0
for queue in $QUEUES; do
    queue_flag=""
    [ "$queue" = "empty" ] && queue_flag="-e "
    for t in $THREADS; do
        tag="sat_${queue}_t${t}"
        expected_done=$((expected_done + 1))
        echo "=== Running $tag (queue=$queue threads=$t iters=$ITERS) ==="
        ae_send_command 1 \
            "polling_client.bin -s 0 ${queue_flag}-q -t $t -n $ITERS -m $tag"
        if ! wait_for_done_count 1 "$expected_done" "$tag done"; then
            echo "[WARN] $tag did not complete; skipping remaining points on this boot" >&2
            break 2
        fi
    done
done

cp "$(ae_machine_log 0)" "$AE_LOG_DIR/machine0.log"
cp "$(ae_machine_log 1)" "$AE_LOG_DIR/machine1.log"
ae_kill_cluster

echo ""
echo "=== Parsing logs and generating figure ==="
# shellcheck disable=SC2086
if ! python3 "$AE_DIR/plot.py" \
    --log-dir "$AE_LOG_DIR" --csv-dir "$CSV_DIR" --fig-dir "$FIG_DIR" \
    --queues $QUEUES --threads $THREADS; then
    ae_record_error "plot.py failed for the queue-saturation sweep"
fi

echo "Artifact output: $OUT_DIR"
ae_finish
