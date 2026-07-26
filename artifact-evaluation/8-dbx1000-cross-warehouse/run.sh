#!/usr/bin/env bash
# Reviewer-requested TPC-C sweep over transaction-level cross-warehouse ratios.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

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
elif [ "$DBX_SMOKE" = 0 ]; then
    DEFAULT_RATIOS="15 50 80"
    DEFAULT_MACHINES=8
    DEFAULT_WAREHOUSES=64
    DEFAULT_THREADS=8
    DEFAULT_WARMUP=7040000
    DEFAULT_MAX_TXN=10000
    DEFAULT_REPETITIONS=3
    DEFAULT_DRAM_SIZE=24G
    DEFAULT_BACKING_BYTES=$((32 * 1024 * 1024 * 1024))
    DEFAULT_TIMEOUT=3600
    DEFAULT_LOG_STALL=0
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
BASELINE_MACHINES=1
# Kernel/user memory placement. The defaults keep user pages in local DRAM so
# only cross-machine sharing pulls them into CXL; override to compare against
# the paper's auto-scale placement (MIXED_DEFAULT_CXL + DEFAULT_CXL).
MALLOC_MODE="${DBX_MALLOC_MODE:-MIXED_DEFAULT_CXL}"
USER_MALLOC_MODE="${DBX_USER_MALLOC_MODE:-DEFAULT_DRAM}"

case "$MALLOC_MODE" in
    CXL|DRAM|MIXED_DEFAULT_CXL|MIXED_DEFAULT_DRAM) ;;
    *) echo "Invalid DBX_MALLOC_MODE: $MALLOC_MODE" >&2; exit 1 ;;
esac
case "$USER_MALLOC_MODE" in
    DEFAULT_CXL|DEFAULT_DRAM) ;;
    *) echo "Invalid DBX_USER_MALLOC_MODE: $USER_MALLOC_MODE" >&2; exit 1 ;;
esac

is_positive_int() { [[ "$1" =~ ^[1-9][0-9]*$ ]]; }
for value in "$NUM_MACHINES" "$NUM_WAREHOUSES" "$THREADS_PER_MACHINE" \
    "$WARMUP" "$MAX_TXN" "$REPETITIONS" "$GUEST_CPUS" "$TIMEOUT" \
    "$EXIT_TIMEOUT" "$BACKING_MIN_BYTES"; do
    is_positive_int "$value" || { echo "Invalid positive integer: $value" >&2; exit 1; }
done
[[ "$LOG_STALL" =~ ^[0-9]+$ ]] || {
    echo "DBX_LOG_STALL_S must be a non-negative integer" >&2; exit 1;
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
read -r -a RATIO_LIST <<< "$RATIOS"
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

set_placement() {
    ae_set_dsm_var DSM_SHM_DEVICE IVSHMEM
    ae_set_dsm_var DSM_MALLOC_MODE "$MALLOC_MODE"
    ae_set_dsm_var DSM_USER_MALLOC_MODE "$USER_MALLOC_MODE"
    for type in THREADCTX PGTABLE STACK OBJECT PAGE; do
        ae_set_dsm_var "DSM_${type}_MODE" CXL
    done
    ae_set_dsm_var USE_DEV_AS_DRAM ON
    ae_set_dsm_var DSM_CXL_LF_BUDDY OFF
    ae_set_dsm_var SLAB_CRASH_RECOVERY OFF
    ae_set_dsm_var PHOENIX_SCHED_TIMING OFF
}

check_backing_files() {
    local paths=() i j path size
    for i in 0 1 2 3; do
        for j in 0 1; do paths+=("/dev/shm/numa${i}.${j}-$USER"); done
    done
    for ((i = 0; i < NUM_MACHINES; i++)); do
        path="${paths[$i]}"
        [ -f "$path" ] && [ ! -L "$path" ] || {
            echo "Missing NUMA backing file: $path" >&2; return 1;
        }
        size="$(stat -Lc '%s' -- "$path")"
        [ "$size" -eq "$BACKING_MIN_BYTES" ] || {
            echo "$path is $size bytes; expected exactly $BACKING_MIN_BYTES bytes" >&2
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
            after && line ~ /^[$][[:blank:]]*$/ { prompt=1 }
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
    set_dbx_define USE_TRANSACTION_CROSS_WAREHOUSE_RATIO true
    set_dbx_define PERC_CROSS_WAREHOUSE_TXN "$ratio"
    set_dbx_define LOAD_UNUSED_TABLES false
    set_dbx_define ITEM_I_DATA_LEN 1000

    echo "=== ratio=${ratio}% machines=$machines warehouses=$wh ==="
    ae_build_with_config_restore "$DBX_BINARY"
    for rep in $(seq 1 "$REPETITIONS"); do
        suffix="_m${machines}_r${ratio}_rep${rep}"
        AE_EXTRA_ENV="DRAM_SIZE=$DRAM_SIZE" ae_boot_cluster "$machines" "$GUEST_CPUS"
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
set_placement
enable_vmspace_stats

for ratio in "${RATIO_LIST[@]}"; do
    run_configuration "$NUM_MACHINES" "$ratio"
    run_configuration "$BASELINE_MACHINES" "$ratio"
done

restore_files
ae_restore_build_configs

python3 "$AE_DIR/plot.py" --log-dir "$AE_LOG_DIR" --csv-dir "$CSV_DIR" \
    --fig-dir "$FIG_DIR" --num-machines "$NUM_MACHINES" \
    --num-warehouses "$NUM_WAREHOUSES" --threads-per-machine "$THREADS_PER_MACHINE" \
    --guest-cpus "$GUEST_CPUS" \
    --max-txn "$MAX_TXN" --warmup "$WARMUP" --repetitions "$REPETITIONS" \
    --ratios "${RATIO_LIST[@]}"

echo "Artifact output: $OUT_DIR"
ae_finish
