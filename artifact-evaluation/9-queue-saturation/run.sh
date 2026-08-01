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
# Each point reports client-side latency percentiles ([SUMMARY], tail latency)
# and aggregate throughput over the synchronized request interval ([TPUT]).
# plot.py marks a saturation throughput only after the sweep observes a
# high-load plateau; otherwise it reports the maximum as a lower bound.
#
# Usage (from repo root):
#   ./artifact-evaluation/prepare.sh          # once
#   ./artifact-evaluation/9-queue-saturation/run.sh
#
# Env overrides:
#   THREADS="1 2 4 6 8 10 12"   QUEUES="empty read"   REPEATS=3
#   ITERS=20000   TIMEOUT=600   CLIENT_MODE_FLAGS="-d"  # local direct IPC
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/9-queue-saturation"
ae_init_output_dirs "$AE_DIR"
AE_LOG_DIR="$LOG_DIR"
current_logs_archived=1

archive_current_logs() {
    local group="$1" machine live_log aggregate_log
    [ "$current_logs_archived" = "1" ] && return 0
    for ((machine = 0; machine < NUM_MACHINES; machine++)); do
        live_log="$(ae_machine_log "$machine")"
        aggregate_log="$AE_LOG_DIR/machine${machine}.log"
        {
            printf '\n===== queue-saturation boot: %s machine=%d =====\n' \
                   "$group" "$machine"
            if [ -f "$live_log" ]; then
                cat "$live_log"
            fi
        } >> "$aggregate_log"
    done
    current_logs_archived=1
}

NUM_MACHINES=2
# The client spin-waits, so THREADS must stay strictly below the per-guest vCPU
# count -- these two defaults move together, and plot.py's DEFAULT_THREADS with
# them.  32 vCPUs / 1..12 client threads is what the camera-ready sweep used
# (out/qsat32_*_20260730); the ipc-cdf-sized 12-vCPU guest cannot reach 12
# threads.  Large CPU counts have triggered rr_sched budget BUGs during boot,
# so raise this only as far as a sweep actually needs.
QSAT_CPU_NUM="${QSAT_CPU_NUM:-${AE_MICROBENCH_GUEST_CPU_NUM:-32}}"
THREADS="${THREADS:-1 2 4 6 8 10 12}"
QUEUES="${QUEUES:-empty read}"
ITERS="${ITERS:-20000}"
REPEATS="${REPEATS:-3}"
TIMEOUT="${TIMEOUT:-600}"
SKIP_BUILD="${SKIP_BUILD:-0}"
PLATEAU_THRESHOLD_PCT="${PLATEAU_THRESHOLD_PCT:-5}"
CLIENT_MODE_FLAGS="${CLIENT_MODE_FLAGS:-}"

seen_threads=""
for t in $THREADS; do
    if ! [[ "$t" =~ ^[1-9][0-9]*$ ]]; then
        echo "THREADS entries must be positive integers: $t" >&2
        exit 1
    fi
    if [[ " $seen_threads " == *" $t "* ]]; then
        echo "THREADS must not contain duplicates: $t" >&2
        exit 1
    fi
    seen_threads="${seen_threads:+$seen_threads }$t"
    if [ "$t" -ge "$QSAT_CPU_NUM" ]; then
        echo "THREADS entry $t must stay below the guest vCPU count ($QSAT_CPU_NUM):" >&2
        echo "the polling client spin-waits and oversubscribed guests distort tails." >&2
        exit 1
    fi
done
seen_queues=""
for q in $QUEUES; do
    case "$q" in
        empty|read) ;;
        *) echo "Unknown QUEUES entry: $q (expected: empty read)" >&2; exit 1 ;;
    esac
    if [[ " $seen_queues " == *" $q "* ]]; then
        echo "QUEUES must not contain duplicates: $q" >&2
        exit 1
    fi
    seen_queues="${seen_queues:+$seen_queues }$q"
done
if ! [[ "$ITERS" =~ ^[1-9][0-9]*$ ]]; then
    echo "ITERS must be a positive integer: $ITERS" >&2
    exit 1
fi
if ! [[ "$REPEATS" =~ ^[1-9][0-9]*$ ]]; then
    echo "REPEATS must be a positive integer: $REPEATS" >&2
    exit 1
fi
if [ "$REPEATS" -lt 3 ]; then
    # Say it up front rather than after the sweep: plot.py trims the lowest
    # and highest trial per point, which needs three, and falls back to a
    # plain mean below that.  Fine for a debug run, not what the paper reports.
    echo "[AE] REPEATS=$REPEATS (< 3): outlier trimming is disabled;" \
         "saturation.csv will hold plain means." >&2
fi
if ! [[ "$PLATEAU_THRESHOLD_PCT" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "PLATEAU_THRESHOLD_PCT must be a non-negative number: $PLATEAU_THRESHOLD_PCT" >&2
    exit 1
fi
case "$CLIENT_MODE_FLAGS" in
    ""|-d) ;;
    *)
        echo "CLIENT_MODE_FLAGS must be empty (remote) or -d (local): $CLIENT_MODE_FLAGS" >&2
        exit 1
        ;;
esac

ae_acquire_run_lock "queue-saturation" || exit 1

cleanup() {
    local rc=$?
    trap - EXIT
    # Preserve whatever serial output exists, even when a point failed.
    archive_current_logs "interrupted" || rc=1
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

# check_cluster_health <label>
check_cluster_health() {
    local label="$1" watch_machine watch_log err
    for ((watch_machine = 0; watch_machine < NUM_MACHINES; watch_machine++)); do
        watch_log="$(ae_machine_log "$watch_machine")"
        if grep -aq 'polling_client: failed' "$watch_log" 2>/dev/null; then
            ae_record_error "$label: polling client failed on machine $watch_machine"
            tail -40 "$watch_log" >&2 || true
            return 3
        fi
        err="$(grep -aEo "$AE_ERROR_PATTERN" "$watch_log" 2>/dev/null | head -1 || true)"
        if [ -n "$err" ]; then
            ae_record_error "$label: guest error on machine $watch_machine -> $err (log: $watch_log)"
            tail -40 "$watch_log" >&2 || true
            return 3
        fi
        if ! tmux list-panes -t "$AE_SESSION:$watch_machine" >/dev/null 2>&1; then
            ae_record_error "$label: tmux window $AE_SESSION:$watch_machine died (log: $watch_log)"
            tail -40 "$watch_log" >&2 || true
            return 3
        fi
    done
    return 0
}

# wait_for_client_exit <machine> <tag> <label>
# A tag-specific marker emitted after client-side cleanup, followed by a bare
# guest-shell prompt, proves that the process returned after cap-group teardown.
queue_client_returned_to_shell() {
    local logfile="$1" tag="$2"
    awk -v marker="queue_client_exited:${tag}:0" '
        {
            line = $0
            sub(/\r$/, "", line)
            if (line == marker) {
                after_marker = 1
                next
            }
            if (after_marker && line ~ /^[$][[:blank:]]*$/)
                found_prompt = 1
        }
        END { exit(found_prompt ? 0 : 1) }
    ' "$logfile" 2>/dev/null
}

wait_for_client_exit() {
    local machine="$1" tag="$2" label="$3"
    local logfile elapsed=0 exit_line
    logfile="$(ae_machine_log "$machine")"
    while [ "$elapsed" -lt "$TIMEOUT" ]; do
        check_cluster_health "$label" || return 3
        exit_line="$(grep -aEo "queue_client_exited:${tag}:[0-9]+" "$logfile" 2>/dev/null | tail -1 || true)"
        if [ -n "$exit_line" ] && [ "$exit_line" != "queue_client_exited:${tag}:0" ]; then
            ae_record_error "$label: client process returned non-zero -> $exit_line"
            tail -40 "$logfile" >&2 || true
            return 3
        fi
        if [ "$exit_line" = "queue_client_exited:${tag}:0" ] \
            && queue_client_returned_to_shell "$logfile" "$tag"; then
            # Close the done/teardown race with a final all-machine scan.
            check_cluster_health "$label final health" || return 3
            echo "$label"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    ae_record_timeout "$label (exit marker or following shell prompt not seen within ${TIMEOUT}s in $logfile)"
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

: > "$AE_LOG_DIR/machine0.log"
: > "$AE_LOG_DIR/machine1.log"
run_failed=0
for queue in $QUEUES; do
    queue_flag=""
    [ "$queue" = "empty" ] && queue_flag="-e "
    for t in $THREADS; do
        group="${queue}_t${t}"
        echo "=== Booting the two-machine saturation cluster for $group (cpu=${QSAT_CPU_NUM}) ==="
        current_logs_archived=0
        if ! ae_boot_cluster "$NUM_MACHINES" "$QSAT_CPU_NUM"; then
            ae_record_error "boot failed for queue-saturation group $group"
            archive_current_logs "$group"
            run_failed=1
            break 2
        fi
        soft_wait_polling_server 0
        soft_wait_polling_server 1

        # Rebooting between concurrency levels avoids carrying guest process
        # teardown state or queue generations from one independent point into
        # the next.  The serial log for this boot therefore starts at zero.
        for ((repeat = 1; repeat <= REPEATS; repeat++)); do
            tag="sat_${queue}_t${t}_r${repeat}"
            echo "=== Running $tag (queue=$queue threads=$t repeat=$repeat/$REPEATS iters=$ITERS) ==="
            ae_send_command 1 \
                "polling_client.bin ${CLIENT_MODE_FLAGS:+$CLIENT_MODE_FLAGS }-s 0 ${queue_flag}-q -t $t -n $ITERS -m $tag"
            if ! wait_for_client_exit 1 "$tag" "$tag done"; then
                echo "[WARN] $tag did not complete; stopping the sweep" >&2
                run_failed=1
                break
            fi
        done
        archive_current_logs "$group"
        ae_kill_cluster
        [ "$run_failed" = "1" ] && break 2
    done
done

echo ""
echo "=== Parsing logs and generating figure ==="
# shellcheck disable=SC2086
if ! python3 "$AE_DIR/plot.py" \
    --log-dir "$AE_LOG_DIR" --csv-dir "$CSV_DIR" --fig-dir "$FIG_DIR" \
    --queues $QUEUES --threads $THREADS --repeats "$REPEATS" \
    --plateau-threshold-pct "$PLATEAU_THRESHOLD_PCT"; then
    ae_record_error "plot.py failed for the queue-saturation sweep"
fi

echo "Artifact output: $OUT_DIR"
ae_finish
