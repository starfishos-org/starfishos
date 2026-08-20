#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi
# Reviewer-requested TPC-C sweep over transaction-level cross-warehouse ratios.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/8-dbx1000-cross-warehouse"
DBX_CONFIG="$AE_REPO_ROOT/user/demos/dbx1000/config.h"
KERNEL_CMAKE="$AE_REPO_ROOT/kernel/CMakeLists.txt"
DBX_BINARY="$AE_REPO_ROOT/user/build/ramdisk/rundb.bin"
DBX_SMOKE="${DBX_SMOKE:-0}"

if [ "$DBX_SMOKE" = 1 ]; then
    DEFAULT_RATIOS="15"
    DEFAULT_MACHINES=2
    DEFAULT_WAREHOUSES=2
    DEFAULT_THREADS=2
    DEFAULT_WARMUP=2000
    DEFAULT_MAX_TXN=100
    DEFAULT_REPETITIONS=1
    DEFAULT_DRAM_SIZE=16G
    DEFAULT_BACKING_BYTES=$((16 * 1024 * 1024 * 1024))
    DEFAULT_TIMEOUT=600
    DEFAULT_LOG_STALL=0
    DEFAULT_MEASURE_SEC=2
elif [ "$DBX_SMOKE" = 0 ]; then
    DEFAULT_RATIOS="0 5 10 15 50 80 100"
    DEFAULT_MACHINES=8
    DEFAULT_WAREHOUSES=64
    DEFAULT_THREADS=8
    # The 8-machine total. run_configuration scales the one-machine baseline
    # to the same 64000 transactions per machine (8000 per worker).
    DEFAULT_WARMUP=512000
    DEFAULT_MAX_TXN=10000
    DEFAULT_REPETITIONS=3
    DEFAULT_DRAM_SIZE=16G
    DEFAULT_BACKING_BYTES=$((16 * 1024 * 1024 * 1024))
    # 1.5x the slowest successful point measured over 80 points in the clean
    # runs under out/ (697 s).
    DEFAULT_TIMEOUT=1050
    DEFAULT_LOG_STALL=0
    DEFAULT_MEASURE_SEC=5
else
    echo "DBX_SMOKE must be 0 or 1" >&2
    exit 1
fi

RATIOS="${RATIOS:-$DEFAULT_RATIOS}"
NUM_MACHINES="${NUM_MACHINES:-$DEFAULT_MACHINES}"
NUM_WAREHOUSES="${DBX_NUM_WH:-$DEFAULT_WAREHOUSES}"
THREADS_PER_MACHINE="${DBX_THREADS_PER_MACHINE:-$DEFAULT_THREADS}"
WARMUP="${DBX_WARMUP:-$DEFAULT_WARMUP}"
MAX_TXN="${DBX_MAX_TXN:-$DEFAULT_MAX_TXN}"
REPETITIONS="${DBX_REPETITIONS:-$DEFAULT_REPETITIONS}"
DRAM_SIZE="${DBX_DRAM_SIZE:-$DEFAULT_DRAM_SIZE}"
BACKING_MIN_BYTES="${DBX_BACKING_MIN_BYTES:-$DEFAULT_BACKING_BYTES}"
GUEST_CPUS="${DBX_GUEST_CPUS:-12}"
TIMEOUT="${TIMEOUT:-$DEFAULT_TIMEOUT}"
LOG_STALL="${DBX_LOG_STALL_S:-$DEFAULT_LOG_STALL}"
EXIT_TIMEOUT="${DBX_EXIT_TIMEOUT:-120}"
# Length of the measurement window in seconds. With a positive value every
# worker measures the same interval and stops on its own clock; set 0 to fall
# back to DBx1000's stock rule, where the run ends as soon as the first worker
# commits MAX_TXN_PER_PART transactions and the window length therefore depends
# on which worker got there first.
MEASURE_SEC="${DBX_MEASURE_SEC:-$DEFAULT_MEASURE_SEC}"
# Host NUMA pinning for the guests (qemu_wrapper.sh). It has to be forwarded
# explicitly: ae_boot_cluster launches simulate.sh inside a tmux pane and only
# AE_EXTRA_ENV crosses that boundary, so exporting the variable in this shell
# would reach the guests only when tmux happens to start a fresh server.
NUMA_BIND="${CHCORE_QEMU_NUMA_BIND:-1}"
BASELINE_MACHINES=1
# Kernel/user memory placement. The defaults keep both kernel objects and user
# pages in local DRAM so that only cross-machine sharing pulls them into CXL.
# This is the placement every published cross-warehouse number was measured
# with (out/20260728_125606, and 17 runs from 2026-07-27 to 2026-08-05 that all
# land at 0.59-0.68 Mtxn/s for the 8-machine arm at ratio 15%).
#
# Do not silently switch DSM_MALLOC_MODE to MIXED_DEFAULT_CXL here: putting the
# kernel's own objects on CXL collapses the cluster arm by 10-20x and makes it
# wildly unstable (0.0001-0.595 Mtxn/s at the same ratio across runs), which is
# what an AE reviewer hit on 2026-08-20 while the one-machine baseline arm
# reproduced normally.  Override per run with DBX_MALLOC_MODE if that variant
# is the thing being studied.
MALLOC_MODE="${DBX_MALLOC_MODE:-MIXED_DEFAULT_DRAM}"
USER_MALLOC_MODE="${DBX_USER_MALLOC_MODE:-DEFAULT_DRAM}"
# The one-machine baseline gets its own placement, and it is CXL-backed on
# purpose. Cross-machine faults migrate cluster pages into CXL and never move
# them back, so by steady state the cluster serves ~99% of its accesses from
# there; a DRAM-backed baseline would therefore compare two memory tiers rather
# than two machine counts, and would flatter the cluster's scaleup.
BASELINE_MALLOC_MODE="${DBX_BASELINE_MALLOC_MODE:-MIXED_DEFAULT_CXL}"
BASELINE_USER_MALLOC_MODE="${DBX_BASELINE_USER_MALLOC_MODE:-DEFAULT_CXL}"
# Case 2.3 DSM read-ahead depth in pages (1 disables read-ahead).  Bounded by
# POLLING_TLB_BATCH_MAX, which this must never change: it sizes entries[] in
# the kernel/polling shared-memory ABI struct.
READAHEAD="${DBX_READAHEAD:-4}"
# CXL residency cap experiment. Override with OFF for the no-cap baseline.
# Keep the fixed cap and policy scoped to this target.
CXL_DEMOTE="${DBX_CXL_DEMOTE:-ON}"
CXL_DEMOTE_LIMIT_MB="${DBX_CXL_DEMOTE_LIMIT_MB:-1024}"
CXL_DEMOTE_POLICY="${DBX_CXL_DEMOTE_POLICY:-CLOCK}"

for mode in "$MALLOC_MODE" "$BASELINE_MALLOC_MODE"; do
    case "$mode" in
        CXL|DRAM|MIXED_DEFAULT_CXL|MIXED_DEFAULT_DRAM) ;;
        *) echo "Invalid malloc mode: $mode" >&2; exit 1 ;;
    esac
done
for mode in "$USER_MALLOC_MODE" "$BASELINE_USER_MALLOC_MODE"; do
    case "$mode" in
        DEFAULT_CXL|DEFAULT_DRAM) ;;
        *) echo "Invalid user malloc mode: $mode" >&2; exit 1 ;;
    esac
done
case "$CXL_DEMOTE" in
    ON|OFF) ;;
    *) echo "DBX_CXL_DEMOTE must be ON or OFF" >&2; exit 1 ;;
esac
case "$CXL_DEMOTE_POLICY" in
    CLOCK|FIFO) ;;
    *) echo "DBX_CXL_DEMOTE_POLICY must be CLOCK or FIFO" >&2; exit 1 ;;
esac

is_positive_int() { [[ "$1" =~ ^[1-9][0-9]*$ ]]; }
for value in "$NUM_MACHINES" "$NUM_WAREHOUSES" "$THREADS_PER_MACHINE" \
    "$WARMUP" "$MAX_TXN" "$REPETITIONS" "$GUEST_CPUS" "$TIMEOUT" \
    "$EXIT_TIMEOUT" "$BACKING_MIN_BYTES" "$READAHEAD"; do
    is_positive_int "$value" || { echo "Invalid positive integer: $value" >&2; exit 1; }
done
is_positive_int "$CXL_DEMOTE_LIMIT_MB" || {
    echo "DBX_CXL_DEMOTE_LIMIT_MB must be a positive integer" >&2; exit 1;
}
[ "$READAHEAD" -le 32 ] || {
    echo "DBX_READAHEAD must be <= POLLING_TLB_BATCH_MAX (32)" >&2; exit 1;
}
[[ "$LOG_STALL" =~ ^[0-9]+$ ]] || {
    echo "DBX_LOG_STALL_S must be a non-negative integer" >&2; exit 1;
}
[[ "$MEASURE_SEC" =~ ^[0-9]+$ ]] || {
    echo "DBX_MEASURE_SEC must be a non-negative integer" >&2; exit 1;
}
[ "$NUM_MACHINES" -ge 2 ] && [ "$NUM_MACHINES" -le 8 ] || {
    echo "NUM_MACHINES must be in [2, 8]" >&2; exit 1;
}
[ $((NUM_WAREHOUSES % NUM_MACHINES)) -eq 0 ] || {
    echo "DBX_NUM_WH must be divisible by NUM_MACHINES" >&2; exit 1;
}
[ $((WARMUP % NUM_MACHINES)) -eq 0 ] || {
    echo "DBX_WARMUP must be divisible by NUM_MACHINES" >&2; exit 1;
}
[ "$THREADS_PER_MACHINE" -le "$GUEST_CPUS" ] || {
    echo "DBX_THREADS_PER_MACHINE exceeds DBX_GUEST_CPUS" >&2; exit 1;
}
if [ -n "${ZSH_VERSION:-}" ]; then
    read -r -A RATIO_LIST <<< "$RATIOS"
else
    read -r -a RATIO_LIST <<< "$RATIOS"
fi
[ "${#RATIO_LIST[@]}" -gt 0 ] || { echo "RATIOS must not be empty" >&2; exit 1; }
seen_ratios=" "
for ratio in "${RATIO_LIST[@]}"; do
    [[ "$ratio" =~ ^[0-9]+$ ]] && [ "$ratio" -le 100 ] || {
        echo "Invalid ratio: $ratio" >&2; exit 1;
    }
    [[ "$seen_ratios" != *" $ratio "* ]] || {
        echo "Duplicate ratio: $ratio" >&2; exit 1;
    }
    seen_ratios+="$ratio "
done

WAREHOUSES_PER_MACHINE=$((NUM_WAREHOUSES / NUM_MACHINES))
WARMUP_PER_MACHINE=$((WARMUP / NUM_MACHINES))
TMP_DIR=""

restore_files() {
    local failed=0
    [ -n "$TMP_DIR" ] && [ -d "$TMP_DIR" ] || return 0
    cp "$TMP_DIR/config.h" "$DBX_CONFIG" || failed=1
    cp "$TMP_DIR/CMakeLists.txt" "$KERNEL_CMAKE" || failed=1
    if [ "$failed" -eq 0 ]; then
        rm -r -- "$TMP_DIR"
        TMP_DIR=""
    else
        echo "Failed to restore source files; backup retained at $TMP_DIR" >&2
    fi
    return "$failed"
}

cleanup() {
    local rc=$?
    trap - EXIT
    ae_kill_cluster || { [ "$rc" -ne 0 ] || rc=1; }
    restore_files || rc=1
    ae_restore_build_configs || rc=1
    exit "$rc"
}

set_dbx_define() {
    local name="$1" value="$2"
    sed -i "s/^#define ${name}[[:space:]].*/#define ${name}\t\t\t${value}/" "$DBX_CONFIG"
    grep -qE "^#define ${name}[[:space:]]+${value}([[:space:]]|$)" "$DBX_CONFIG"
}

enable_vmspace_stats() {
    sed -i \
        -e 's|^# *\(target_compile_definitions(${kernel_target} PRIVATE PRINT_VMSPACE_STATS)\)|\1|' \
        -e 's|^# *\(target_compile_definitions(${kernel_target} PRIVATE PRINT_VMSPACE_STATS_NO_DETAILS)\)|\1|' \
        "$KERNEL_CMAKE"
    grep -q '^target_compile_definitions(${kernel_target} PRIVATE PRINT_VMSPACE_STATS)$' "$KERNEL_CMAKE"
    grep -q '^target_compile_definitions(${kernel_target} PRIVATE PRINT_VMSPACE_STATS_NO_DETAILS)$' "$KERNEL_CMAKE"
}

# Rewrite the read-ahead depth line, whether it is currently commented or not.
# Only PGFAULT_READAHEAD_MAX is touched; POLLING_TLB_BATCH_MAX stays at 32 on
# both sides of the polling ABI.
set_readahead() {
    local value="$1"
    sed -i \
        "s|^#* *target_compile_definitions(\${kernel_target} PRIVATE PGFAULT_READAHEAD_MAX=.*|target_compile_definitions(\${kernel_target} PRIVATE PGFAULT_READAHEAD_MAX=${value})|" \
        "$KERNEL_CMAKE"
    grep -qF "target_compile_definitions(\${kernel_target} PRIVATE PGFAULT_READAHEAD_MAX=${value})" \
        "$KERNEL_CMAKE"
}

# set_placement <malloc_mode> <user_malloc_mode>
# Called again before each configuration, since the cluster and the baseline
# deliberately run different placements. Only .config is snapshot/restored per
# build, so edits to kernel/dsm_config.cmake survive into the next build.
set_placement() {
    ae_set_dsm_var DSM_SHM_DEVICE IVSHMEM
    ae_set_dsm_var DSM_MALLOC_MODE "$1"
    ae_set_dsm_var DSM_USER_MALLOC_MODE "$2"
    for type in THREADCTX PGTABLE STACK OBJECT PAGE; do
        ae_set_dsm_var "DSM_${type}_MODE" CXL
    done
    ae_set_dsm_var USE_DEV_AS_DRAM ON
    ae_set_dsm_var DSM_CXL_LF_BUDDY OFF
    ae_set_dsm_var DSM_CXL_DEMOTE "$CXL_DEMOTE"
    ae_set_dsm_var DSM_CXL_DEMOTE_LIMIT_MB "$CXL_DEMOTE_LIMIT_MB"
    ae_set_dsm_var DSM_CXL_DEMOTE_POLICY "$CXL_DEMOTE_POLICY"
    ae_set_dsm_var SLAB_CRASH_RECOVERY OFF
    ae_set_dsm_var PHOENIX_SCHED_TIMING OFF
}

check_backing_files() {
    local paths=() i j backing_path size
    for i in 0 1 2 3; do
        for j in 0 1; do paths+=("/dev/shm/numa${i}.${j}-$USER"); done
    done
    for ((i = 0; i < NUM_MACHINES; i++)); do
        backing_path="${paths[$i]}"
        [ -f "$backing_path" ] && [ ! -L "$backing_path" ] || {
            echo "Missing NUMA backing file: $backing_path" >&2; return 1;
        }
        size="$(stat -Lc '%s' -- "$backing_path")"
        [ "$size" -eq "$BACKING_MIN_BYTES" ] || {
            echo "$backing_path is $size bytes; expected exactly $BACKING_MIN_BYTES bytes" >&2
            return 1
        }
    done
}

bind_cpu_list() {
    local machines="$1" machine parts=()
    for machine in $(seq 0 $((machines - 1))); do
        parts+=("$((machine * GUEST_CPUS))-$((machine * GUEST_CPUS + THREADS_PER_MACHINE - 1))")
    done
    (IFS=,; echo "${parts[*]}")
}

done_count() {
    awk '{ line=$0; sub(/\r$/, "", line); if (line=="done") n++ } END { print n+0 }' \
        "$(ae_machine_log 0)" 2>/dev/null
}

cluster_healthy() {
    local machines="$1" machine log fatal
    if ae_watchdog_tripped; then
        echo "$(ae_watchdog_reason)" >&2
        return 1
    fi
    for machine in $(seq 0 $((machines - 1))); do
        log="$(ae_machine_log "$machine")"
        fatal="$(_ae_error_grep "$log" || true)"
        [ -z "$fatal" ] || { echo "Fatal on machine $machine: $fatal" >&2; return 1; }
        tmux list-panes -t "$AE_SESSION:$machine" >/dev/null 2>&1 || return 1
    done
}

wait_for_process_exit() {
    local machines="$1" before="$2" elapsed=0 log
    log="$(ae_machine_log 0)"
    while [ "$elapsed" -lt "$EXIT_TIMEOUT" ]; do
        cluster_healthy "$machines" || return 1
        if [ "$(done_count)" -gt "$before" ] && awk -v skip="$before" '
            { line=$0; sub(/\r$/, "", line) }
            line=="done" { seen++; if (seen>skip) after=1; next }
            # Kernel diagnostics can race with the interactive shell and be
            # appended to the prompt (for example, "$ [INFO] ...").  The
            # leading prompt after the exact "done" marker is sufficient;
            # requiring the remainder of that physical log line to be blank
            # turns a completed run into a spurious EXIT_TIMEOUT wait.
            after && line ~ /^[$][[:blank:]]/ { prompt=1 }
            END { exit(prompt ? 0 : 1) }
        ' "$log"; then
            sleep 3
            cluster_healthy "$machines"
            return
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    echo "rundb did not return to the shell within ${EXIT_TIMEOUT}s" >&2
    return 1
}

run_configuration() {
    local machines="$1" ratio="$2" wh warmup bind rep machine before suffix
    wh=$((WAREHOUSES_PER_MACHINE * machines))
    warmup=$((WARMUP_PER_MACHINE * machines))
    bind="$(bind_cpu_list "$machines")"

    set_dbx_define NUM_MACHINES "$machines"
    set_dbx_define THREADS_PER_MACHINE "$THREADS_PER_MACHINE"
    set_dbx_define NUM_WH "$wh"
    set_dbx_define WARMUP "$warmup"
    set_dbx_define MAX_TXN_PER_PART "$MAX_TXN"
    set_dbx_define MEASURE_DURATION_SEC "$MEASURE_SEC"
    set_dbx_define USE_TRANSACTION_CROSS_WAREHOUSE_RATIO true
    set_dbx_define PERC_CROSS_WAREHOUSE_TXN "$ratio"
    set_dbx_define LOAD_UNUSED_TABLES false
    set_dbx_define ITEM_I_DATA_LEN 1000

    echo "=== ratio=${ratio}% machines=$machines warehouses=$wh ==="
    ae_build_with_config_restore "$DBX_BINARY"
    for rep in $(seq 1 "$REPETITIONS"); do
        suffix="_m${machines}_r${ratio}_rep${rep}"
        AE_EXTRA_ENV="DRAM_SIZE=$DRAM_SIZE CHCORE_QEMU_NUMA_BIND=$NUMA_BIND" \
            ae_boot_cluster "$machines" "$GUEST_CPUS"
        ae_send_command 0 "write dbx1000_bind_cpu.txt $bind"
        before="$(done_count)"
        ae_send_command 0 "rundb.bin"
        if ! AE_LOG_STALL_S="$LOG_STALL" ae_wait_in_log 0 "PASS! SimTime" "$TIMEOUT" \
            "dbx1000 PASS (ratio=${ratio}%, machines=$machines, rep=$rep)" "$machines" \
            || ! wait_for_process_exit "$machines" "$before"; then
            ae_archive_logs "$machines" "$AE_LOG_DIR" "$suffix"
            ae_kill_cluster
            return 1
        fi
        ae_archive_logs "$machines" "$AE_LOG_DIR" "$suffix"
        ae_kill_cluster
        for machine in $(seq 0 $((machines - 1))); do
            [ -s "$AE_LOG_DIR/machine${machine}${suffix}.log" ] || return 1
        done
    done
}

ae_acquire_run_lock "dbx1000-cross-warehouse"
trap cleanup EXIT
check_backing_files
ae_init_output_dirs "$AE_DIR"
AE_LOG_DIR="$LOG_DIR"
mkdir -p "$AE_LOG_DIR" "$CSV_DIR" "$FIG_DIR"

TMP_DIR="$(mktemp -d)"
cp "$DBX_CONFIG" "$TMP_DIR/config.h"
cp "$KERNEL_CMAKE" "$TMP_DIR/CMakeLists.txt"
ae_save_build_configs

cd "$AE_REPO_ROOT"
ae_check_global_prepare
ae_set_paper_guest_cpu_config "$GUEST_CPUS"
ae_export_guest_cpu_num "$GUEST_CPUS"
enable_vmspace_stats
set_readahead "$READAHEAD"
set_placement "$MALLOC_MODE" "$USER_MALLOC_MODE"

# Record the effective configuration so this out/<timestamp>/ stays readable
# after the build configs are restored (config/run_config.json + placement.txt).
ae_manifest_set_vars \
    DBX_SMOKE NUM_MACHINES BASELINE_MACHINES NUM_WAREHOUSES \
    WAREHOUSES_PER_MACHINE THREADS_PER_MACHINE WARMUP WARMUP_PER_MACHINE \
    MAX_TXN MEASURE_SEC RATIOS REPETITIONS DRAM_SIZE GUEST_CPUS NUMA_BIND \
    MALLOC_MODE USER_MALLOC_MODE READAHEAD CXL_DEMOTE CXL_DEMOTE_LIMIT_MB \
    CXL_DEMOTE_POLICY \
    BASELINE_MALLOC_MODE BASELINE_USER_MALLOC_MODE TIMEOUT
ae_write_run_manifest

for ratio in "${RATIO_LIST[@]}"; do
    set_placement "$MALLOC_MODE" "$USER_MALLOC_MODE"
    run_configuration "$NUM_MACHINES" "$ratio"
    set_placement "$BASELINE_MALLOC_MODE" "$BASELINE_USER_MALLOC_MODE"
    run_configuration "$BASELINE_MACHINES" "$ratio"
done

restore_files
ae_restore_build_configs

python3 "$AE_DIR/plot.py" --log-dir "$AE_LOG_DIR" --csv-dir "$CSV_DIR" \
    --fig-dir "$FIG_DIR" --num-machines "$NUM_MACHINES" \
    --num-warehouses "$NUM_WAREHOUSES" --threads-per-machine "$THREADS_PER_MACHINE" \
    --guest-cpus "$GUEST_CPUS" \
    --max-txn "$MAX_TXN" --warmup "$WARMUP" --repetitions "$REPETITIONS" \
    --measure-sec "$MEASURE_SEC" \
    --ratios "${RATIO_LIST[@]}"

echo "Artifact output: $OUT_DIR"
ae_finish
