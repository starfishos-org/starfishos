#!/usr/bin/env bash
#
# Artifact script for the reviewer-requested DBx1000 cross-warehouse sweep:
# TPC-C throughput and CXL memory footprint as the fraction of
# cross-warehouse transactions varies.
#
# For each ratio R in RATIOS this script:
#   1. Sets PERC_REMOTE_PAYMENT / PERC_REMOTE_NEW_ORDER = R in
#      user/demos/dbx1000/config.h.
#   2. Enables PRINT_VMSPACE_STATS (+_NO_DETAILS) in kernel/CMakeLists.txt so
#      dbx1000's usys_print_vmspace_stats() calls report the per-machine
#      DRAM/CXL page counts (after init / after warmup / after execution).
#   3. Rebuilds ChCore and boots an NUM_MACHINES-machine cluster.
#   4. Runs TPC-C (rundb.bin, THREAD_CNT = THREADS_PER_MACHINE x NUM_MACHINES
#      from dbx1000's config.h) and waits for "PASS! SimTime".
#   5. Archives all machine logs; parses thp= and the post-execution
#      [VMSPACE MEMORY] summaries into CSV + figures.
#
# Usage (from repo root):
#   ./artifact-evaluation/prepare.sh          # once
#   ./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
#
# Env overrides:
#   RATIOS="15 50 80"     cross-warehouse percentages to sweep
#   NUM_MACHINES=8        cluster size (dbx1000 config.h NUM_MACHINES is
#                         synced to this value automatically)
#   DRAM_SIZE=24G         per-QEMU guest DRAM (16 GB-scale tables need >16G)
#   TIMEOUT=3600          per-run timeout (s)
#   TIMEOUT=3600          per-run timeout (s)
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/8-dbx1000-cross-warehouse"
ae_init_output_dirs "$AE_DIR"
AE_LOG_DIR="$LOG_DIR"
RATIOS="${RATIOS:-15 50 80}"
NUM_MACHINES="${NUM_MACHINES:-8}"
DRAM_SIZE="${DRAM_SIZE:-24G}"
TIMEOUT="${TIMEOUT:-3600}"

DBX_CONFIG="$AE_REPO_ROOT/user/demos/dbx1000/config.h"
KERNEL_CMAKE="$AE_REPO_ROOT/kernel/CMakeLists.txt"

mkdir -p "$AE_LOG_DIR" "$CSV_DIR" "$FIG_DIR"

TMP_DIR="$(mktemp -d)"
cp "$DBX_CONFIG" "$TMP_DIR/config.h"
cp "$KERNEL_CMAKE" "$TMP_DIR/CMakeLists.txt"

restore_files() {
    local failed=0
    [ -d "$TMP_DIR" ] || return 0

    cp "$TMP_DIR/config.h" "$DBX_CONFIG" || failed=1
    cp "$TMP_DIR/CMakeLists.txt" "$KERNEL_CMAKE" || failed=1
    if [ "$failed" -eq 0 ]; then
        rm -rf "$TMP_DIR"
    else
        echo "[ERROR] failed to restore source files; backup retained at $TMP_DIR" >&2
    fi
    return "$failed"
}

cleanup() {
    local rc=$?
    trap - EXIT
    ae_kill_cluster || { [ "$rc" -ne 0 ] || rc=1; }
    restore_files || { [ "$rc" -ne 0 ] || rc=1; }
    ae_restore_build_configs || { [ "$rc" -ne 0 ] || rc=1; }
    exit "$rc"
}
trap cleanup EXIT

set_dbx_define() {
    local name="$1" val="$2"
    sed -i "s/^#define ${name}[[:space:]].*/#define ${name}\t\t\t${val}/" "$DBX_CONFIG"
    grep -qE "^#define ${name}[[:space:]]+${val}\b" "$DBX_CONFIG" || {
        echo "Failed to set ${name}=${val} in $DBX_CONFIG" >&2
        return 1
    }
}

enable_vmspace_stats() {
    sed -i \
        -e 's|^# *\(target_compile_definitions(${kernel_target} PRIVATE PRINT_VMSPACE_STATS)\)|\1|' \
        -e 's|^# *\(target_compile_definitions(${kernel_target} PRIVATE PRINT_VMSPACE_STATS_NO_DETAILS)\)|\1|' \
        "$KERNEL_CMAKE"
    grep -q '^target_compile_definitions(${kernel_target} PRIVATE PRINT_VMSPACE_STATS)' "$KERNEL_CMAKE" || {
        echo "Failed to enable PRINT_VMSPACE_STATS in $KERNEL_CMAKE" >&2
        return 1
    }
}

# CPU-bind list covering THREADS_PER_MACHINE threads on each of the
# NUM_MACHINES machines (12 cores per machine, 8 DBx1000 workers each:
# 0-7,12-19,24-31,...).  The main thread is not part of this bind list.
bind_cpu_list() {
    local n="$1" i parts=()
    for i in $(seq 0 $((n - 1))); do
        parts+=("$((i * 12))-$((i * 12 + 7))")
    done
    (IFS=,; echo "${parts[*]}")
}

cd "$AE_REPO_ROOT"
ae_ensure_clean_tmux
ae_check_global_prepare
ae_prepare_microbench_guest_cpu
ae_save_build_configs

echo "=== Enabling PRINT_VMSPACE_STATS in kernel ==="
enable_vmspace_stats

echo "=== Syncing dbx1000 NUM_MACHINES=$NUM_MACHINES ==="
set_dbx_define NUM_MACHINES "$NUM_MACHINES"

BIND_LIST="$(bind_cpu_list "$NUM_MACHINES")"
echo "CPU bind list: $BIND_LIST"

for ratio in $RATIOS; do
    echo ""
    echo "########################################################"
    echo "### Cross-warehouse ratio: ${ratio}%"
    echo "########################################################"

    set_dbx_define PERC_REMOTE_PAYMENT "$ratio"
    set_dbx_define PERC_REMOTE_NEW_ORDER "$ratio"
    ae_build

    if ! AE_EXTRA_ENV="DRAM_SIZE=$DRAM_SIZE" ae_boot_cluster "$NUM_MACHINES" "$AE_MICROBENCH_GUEST_CPU_NUM"; then
        echo "[WARN] ratio=${ratio}% cluster failed to boot; archiving partial logs and continuing" >&2
        ae_archive_logs "$NUM_MACHINES" "$AE_LOG_DIR" "_r${ratio}" || true
        ae_kill_cluster
        continue
    fi
    ae_send_command 0 "write dbx1000_bind_cpu.txt $BIND_LIST"
    sleep 2
    ae_send_command 0 "rundb.bin"
    if ae_wait_in_log 0 "PASS! SimTime" "$TIMEOUT" \
        "dbx1000 done (ratio=${ratio}%)" "$NUM_MACHINES"; then
        sleep 5   # let trailing vmspace stats flush on all machines
    else
        # rc 1 (timeout) or 3 (guest error/crash) — reason recorded above;
        # save logs and move on to the next ratio.
        echo "[WARN] ratio=${ratio}% did not complete; skipping to next ratio" >&2
    fi
    ae_archive_logs "$NUM_MACHINES" "$AE_LOG_DIR" "_r${ratio}"
    ae_kill_cluster
done

restore_files
ae_restore_build_configs

echo ""
echo "=== Parsing logs and generating figures ==="
python3 "$AE_DIR/plot.py" --log-dir "$AE_LOG_DIR" --csv-dir "$CSV_DIR" \
    --fig-dir "$FIG_DIR" --num-machines "$NUM_MACHINES" --ratios $RATIOS

echo "Artifact output: $OUT_DIR"
ae_finish
