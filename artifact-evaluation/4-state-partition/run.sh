#!/usr/bin/env bash
#
# Artifact script for paper Figure 13: "Performance across state-partition
# choices".
#
# Camera-ready / shepherd revision plan: Reviewer B asked to show
# K-mix/U-mix vs Share at 4 AND 8 machines so the benefit can be seen
# growing.  The three shared placements therefore run at every cluster size
# in MACHINE_COUNTS (default "4 8"); Private (All_DRAM) remains the
# single-machine ideal baseline shared by all panels.  Each QEMU guest uses
# 12 vCPUs, so the largest default cluster is 8 x 12 = 96 vCPUs — matching
# the paper testbed.
#
# Runs 6 applications (LevelDB, DBx1000, PCA, Matrix Multiply, Linear
# Regression, Word Count) under four state-partition configurations, then
# plots performance normalized to the Private (All-DRAM) setup.
#
# Config -> kernel/dsm_config.cmake mapping (all five per-type modes —
# THREADCTX/PGTABLE/STACK/OBJECT/PAGE — stay "CXL" except in All_DRAM):
#
#   Config (paper label)                  DSM_MALLOC_MODE     DSM_USER_MALLOC_MODE  type modes
#   All_CXL (Share)                       CXL                 DEFAULT_CXL           CXL
#   Kernel_DRAM_User_CXL (K-mix/U-share)  MIXED_DEFAULT_DRAM  DEFAULT_CXL           CXL
#   Kernel_Page_CXL_Other_DRAM (K-mix/U-mix) MIXED_DEFAULT_DRAM DEFAULT_DRAM        CXL
#   All_DRAM (Private)                    DRAM                DEFAULT_DRAM          DRAM
#
# Usage (from repo root):
#   ./artifact-evaluation/prepare.sh          # once
#   ./artifact-evaluation/4-state-partition/run.sh
#
# Env overrides:
#   BENCHS="leveldb dbx1000 pca matrix_multiply linear_regression word_count"
#   CONFIGS="All_CXL Kernel_DRAM_User_CXL Kernel_Page_CXL_Other_DRAM All_DRAM"
#   MACHINE_COUNTS="4 8"   TIMEOUT=1200
#   (use MACHINE_COUNTS=2 for a smaller ablation / debug; NUM_MACHINES=N is
#   accepted as a legacy alias for MACHINE_COUNTS="N")
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/4-state-partition"
ae_init_output_dirs "$AE_DIR"
AE_LOG_DIR="$LOG_DIR"
# Cluster sizes for the shared placements (Private always runs on 1 machine).
# NUM_MACHINES=N is kept as a legacy single-size alias.
MACHINE_COUNTS="${MACHINE_COUNTS:-${NUM_MACHINES:-4 8}}"
TIMEOUT="${TIMEOUT:-1200}"
# Per-guest SMP is 12 vCPUs.  Global CPU layout is therefore
# 0-11,12-23,...,(N-1)*12..(N*12-1).  DBx1000 and Matrix Multiply bind
# eight workers per machine (0-7 on each 12-core segment).
STATE_PARTITION_CPU_NUM="${STATE_PARTITION_CPU_NUM:-${AE_MICROBENCH_GUEST_CPU_NUM:-12}}"
# The default is the complete 6 x 4 matrix used by the paper figure.  The
# DBx1000 compile-time configuration is kept modest for this state-placement
# experiment (one warehouse per machine); the 64-warehouse auto-scale
# configuration would otherwise consume far more memory than this figure
# requires.
BENCHS="${BENCHS-leveldb dbx1000 pca matrix_multiply linear_regression word_count}"
CONFIGS="${CONFIGS-All_CXL Kernel_DRAM_User_CXL Kernel_Page_CXL_Other_DRAM All_DRAM}"

# Validate the documented scope controls before creating output, taking a
# runner lock, or changing any repository/host state.  Since unknown and
# duplicate values are rejected, a reordered/whitespace-normalized full set
# is still unambiguously the complete paper request.
FULL_PLOT_REQUEST=1
validate_scope_list() {
    local label="$1" raw="$2"
    shift 2
    local -a values=()
    local value allowed found seen=""

    case "$raw" in
        *$'\n'*|*$'\r'*)
            echo "[AE] $label must be a whitespace-separated single line" >&2
            return 1
            ;;
    esac
    read -r -a values <<< "$raw"
    if [ "${#values[@]}" -eq 0 ]; then
        echo "[AE] $label must select at least one value" >&2
        return 1
    fi
    for value in "${values[@]}"; do
        found=0
        for allowed in "$@"; do
            if [ "$value" = "$allowed" ]; then
                found=1
                break
            fi
        done
        if [ "$found" != "1" ]; then
            echo "[AE] unknown $label value: $value" >&2
            return 1
        fi
        if [[ " $seen " == *" $value "* ]]; then
            echo "[AE] duplicate $label value: $value" >&2
            return 1
        fi
        seen+=" $value"
    done
    if [ "${#values[@]}" -ne "$#" ]; then
        FULL_PLOT_REQUEST=0
    fi
}

validate_scope_list BENCHS "$BENCHS" \
    leveldb dbx1000 pca matrix_multiply linear_regression word_count || exit 1
validate_scope_list CONFIGS "$CONFIGS" \
    All_CXL Kernel_DRAM_User_CXL Kernel_Page_CXL_Other_DRAM All_DRAM || exit 1

# config -> DSM_MALLOC_MODE  DSM_USER_MALLOC_MODE  <5 type modes>
config_params() {
    case "$1" in
        All_CXL)                    echo "CXL                DEFAULT_CXL  CXL" ;;
        Kernel_DRAM_User_CXL)       echo "MIXED_DEFAULT_DRAM DEFAULT_CXL  CXL" ;;
        Kernel_Page_CXL_Other_DRAM) echo "MIXED_DEFAULT_DRAM DEFAULT_DRAM CXL" ;;
        All_DRAM)                   echo "DRAM               DEFAULT_DRAM DRAM" ;;
        *) echo "Unknown config: $1" >&2; return 1 ;;
    esac
}

# bench -> string that marks completion in machine 0's log
bench_done_pattern() {
    case "$1" in
        leveldb)  echo "MB/s" ;;
        dbx1000)  echo "thp=" ;;
        # all four phoenix apps print "finalize: <us>" as their last timing line
        pca|matrix_multiply|linear_regression|word_count) echo "finalize:" ;;
        *) echo "Unknown bench: $1" >&2; return 1 ;;
    esac
}

# DBx1000 is silent while it initializes the TPC-C tables; LevelDB and the
# Phoenix applications can likewise be silent during their measured work.
# CXL page placement can stretch these phases beyond the generic 120-second
# serial-log stall threshold.  Keep checking fatal guest signatures and tmux
# liveness, but use the benchmark's hard timeout as the fail-safe for these
# known-silent completion markers.
wait_for_bench() {
    local bench="$1" pattern="$2" timeout="$3" label="$4" machines="$5"
    case "$bench" in
        dbx1000|leveldb|pca|matrix_multiply|linear_regression|word_count)
            AE_LOG_STALL_S=0 ae_wait_in_log \
                0 "$pattern" "$timeout" "$label" "$machines"
            ;;
        *)
            ae_wait_in_log 0 "$pattern" "$timeout" "$label" "$machines"
            ;;
    esac
}

# Sync dbx1000's compile-time NUM_MACHINES to this experiment's cluster size
# and launch it with threads bound across all machines (8 workers / 12 cores
# per machine).
DBX_CONFIG="$AE_REPO_ROOT/user/demos/dbx1000/config.h"
DBX_TIMEOUT="${DBX_TIMEOUT:-3600}"
DBX_DRAM_SIZE="${DBX_DRAM_SIZE:-24G}"
DBX_WARMUP="${DBX_WARMUP:-10000}"
DBX_MAX_TXN="${DBX_MAX_TXN:-10000}"
# Matrix Multiply: eight workers per machine on the multi-machine configs;
# Private (All_DRAM) keeps the original single-machine 8-thread binding.
MATRIX_THREADS_PER_MACHINE="${MATRIX_THREADS_PER_MACHINE:-8}"

MAX_MACHINES=0
for count in $MACHINE_COUNTS; do
    if ! [[ "$count" =~ ^[1-9][0-9]*$ ]]; then
        echo "MACHINE_COUNTS entries must be positive integers: $count" >&2
        exit 1
    fi
    if [ "$count" -gt "$MAX_MACHINES" ]; then
        MAX_MACHINES="$count"
    fi
done

# Every multi-machine partition must own at least one warehouse.  Keep the
# total warehouse count fixed across all placement points (including the
# single-machine All_DRAM baseline) so their working sets remain comparable;
# the default is the largest requested cluster size.
DBX_NUM_WH="${DBX_NUM_WH:-$MAX_MACHINES}"
if ! [[ "$DBX_NUM_WH" =~ ^[1-9][0-9]*$ ]]; then
    echo "DBX_NUM_WH must be a positive integer: $DBX_NUM_WH" >&2
    exit 1
fi
for count in $MACHINE_COUNTS; do
    if [ "$DBX_NUM_WH" -lt "$count" ]; then
        echo "DBX_NUM_WH ($DBX_NUM_WH) must be >= every MACHINE_COUNTS entry ($count)" >&2
        echo "DBx1000 requires at least one local warehouse per machine." >&2
        exit 1
    fi
    if [ $((DBX_NUM_WH % count)) -ne 0 ]; then
        echo "DBX_NUM_WH ($DBX_NUM_WH) must be divisible by every MACHINE_COUNTS entry ($count)" >&2
        echo "DBx1000 assigns warehouses to machines in equal contiguous slices." >&2
        exit 1
    fi
done

ae_acquire_run_lock "state-partition" || exit 1

mkdir -p "$AE_LOG_DIR" "$CSV_DIR" "$FIG_DIR"

TMP_DIR="$(mktemp -d)"
cp "$DBX_CONFIG" "$TMP_DIR/config.h"

# Eight worker CPUs on each 12-vCPU machine segment: 0-7,12-19,24-31,...
worker_bind_cpu_list() {
    local n="$1" i parts=()
    for i in $(seq 0 $((n - 1))); do
        parts+=("$((i * STATE_PARTITION_CPU_NUM))-$((i * STATE_PARTITION_CPU_NUM + MATRIX_THREADS_PER_MACHINE - 1))")
    done
    (IFS=,; echo "${parts[*]}")
}

dbx_bind_cpu_list() {
    worker_bind_cpu_list "$1"
}

cleanup() {
    local rc=$? cleanup_failed=0
    trap - EXIT
    ae_kill_cluster || cleanup_failed=1
    if [ -d "$TMP_DIR" ]; then
        if cp "$TMP_DIR/config.h" "$DBX_CONFIG"; then
            rm -rf "$TMP_DIR"
        else
            echo "[AE] failed to restore DBx1000 config; backup retained at $TMP_DIR" >&2
            cleanup_failed=1
        fi
    fi
    ae_restore_build_configs || cleanup_failed=1
    if [ "$rc" -eq 0 ] && [ "$cleanup_failed" -ne 0 ]; then
        rc=1
    fi
    exit "$rc"
}
trap cleanup EXIT

cd "$AE_REPO_ROOT"
ae_ensure_clean_tmux
ae_check_global_prepare
ae_save_build_configs
# Keep the kernel's per-machine CPU stride and QEMU's SMP count in sync.
# The saved configuration is restored by cleanup after this experiment.
ae_set_paper_guest_cpu_config "$STATE_PARTITION_CPU_NUM"
ae_export_guest_cpu_num "$STATE_PARTITION_CPU_NUM"
# LLFree is evaluated by test 3.  Its shared allocator metadata is not a
# state-partition variable and can retain incompatible per-core reservations
# when the preceding single-guest 96-vCPU allocator test is followed by this
# 12-vCPU-per-guest cluster.  Keep this experiment on the conventional CXL
# buddy backend.
ae_set_dsm_var DSM_CXL_LF_BUDDY OFF

for cfg in $CONFIGS; do
    read -r malloc_mode user_malloc_mode type_mode <<< "$(config_params "$cfg")"

    # Private (All_DRAM) is the single-machine ideal baseline: with all
    # state forced into private DRAM, cross-machine sharing is impossible by
    # design (2-machine runs crash in virt_to_page), so it runs on 1 machine
    # — matching the original experiments (old logs used ./build/simulate.sh).
    # The shared placements run at every requested cluster size.
    if [ "$cfg" = "All_DRAM" ]; then
        cfg_counts="1"
    else
        cfg_counts="$MACHINE_COUNTS"
    fi

    ae_set_dsm_var DSM_MALLOC_MODE "$malloc_mode"
    ae_set_dsm_var DSM_USER_MALLOC_MODE "$user_malloc_mode"
    for t in THREADCTX PGTABLE STACK OBJECT PAGE; do
        ae_set_dsm_var "DSM_${t}_MODE" "$type_mode"
    done

    for cfg_machines in $cfg_counts; do
        echo ""
        echo "########################################################"
        echo "### Config: $cfg ($cfg_machines machine(s))"
        echo "###   DSM_MALLOC_MODE=$malloc_mode  DSM_USER_MALLOC_MODE=$user_malloc_mode"
        echo "###   THREADCTX/PGTABLE/STACK/OBJECT/PAGE=$type_mode"
        echo "########################################################"

        # dbx1000's compile-time NUM_MACHINES/PART_CNT must match the cluster
        # size, so each (config, cluster size) point rebuilds.
        sed -i "s/^#define NUM_MACHINES[[:space:]].*/#define NUM_MACHINES\t\t\t$cfg_machines/" "$DBX_CONFIG"
        sed -i "s/^#define NUM_WH[[:space:]].*/#define NUM_WH\t\t\t\t\t\t$DBX_NUM_WH/" "$DBX_CONFIG"
        sed -i "s/^#define WARMUP[[:space:]].*/#define WARMUP\t\t\t\t\t\t$DBX_WARMUP/" "$DBX_CONFIG"
        sed -i "s/^#define MAX_TXN_PER_PART[[:space:]].*/#define MAX_TXN_PER_PART\t\t\t$DBX_MAX_TXN/" "$DBX_CONFIG"
        sed -i "s/^#define ITEM_I_DATA_LEN[[:space:]].*/#define ITEM_I_DATA_LEN\t\t\t50/" "$DBX_CONFIG"
        DBX_BIND_LIST="$(dbx_bind_cpu_list "$cfg_machines")"
        ae_build

        for bench in $BENCHS; do
            pattern="$(bench_done_pattern "$bench")"
            logfile="$AE_LOG_DIR/${bench}_${cfg}_m${cfg_machines}.log"
            point="${bench}-${cfg}-m${cfg_machines}"
            # A targeted retry may reuse the same log directory.  Remove
            # this point before boot so a failed retry cannot be parsed as the
            # prior run's successful measurement.
            rm -f -- "$logfile"
            echo "=== [$cfg] running $bench on $cfg_machines machine(s) (done pattern: '$pattern') ==="
            if [ "$bench" = "dbx1000" ]; then
                if ! AE_EXTRA_ENV="DRAM_SIZE=$DBX_DRAM_SIZE" ae_boot_cluster "$cfg_machines" "$STATE_PARTITION_CPU_NUM"; then
                    ae_record_error "boot failed for $bench under $cfg ($cfg_machines machines)"
                    ae_archive_logs "$cfg_machines" "$AE_LOG_DIR" \
                        "-boot-failed-${point}"
                    continue
                fi
                ae_send_command 0 "write dbx1000_bind_cpu.txt $DBX_BIND_LIST"
                sleep 2
                ae_send_command 0 "rundb.bin"
                bench_timeout="$DBX_TIMEOUT"
            elif [ "$bench" = "matrix_multiply" ]; then
                # Do not source run_matrix_multiply.sh: it hardcodes the old
                # 2-machine bind list (0-7,12-19 / -t 16).  Scale eight workers
                # per machine for the multi-machine ablation; keep the original
                # single-machine Private baseline (8 threads on CPUs 0-11).
                if ! ae_boot_cluster "$cfg_machines" "$STATE_PARTITION_CPU_NUM"; then
                    ae_record_error "boot failed for $bench under $cfg ($cfg_machines machines)"
                    ae_archive_logs "$cfg_machines" "$AE_LOG_DIR" \
                        "-boot-failed-${point}"
                    continue
                fi
                if [ "$cfg_machines" -eq 1 ]; then
                    ae_send_command 0 "write matrix_multiply_bind_cpu.txt 0-11"
                    sleep 2
                    ae_send_command 0 "matrix_multiply.bin -l 2000 -r 2000 -c 0 -t 8"
                else
                    matrix_bind="$(worker_bind_cpu_list "$cfg_machines")"
                    matrix_threads=$((MATRIX_THREADS_PER_MACHINE * cfg_machines))
                    ae_send_command 0 "write matrix_multiply_bind_cpu.txt $matrix_bind"
                    sleep 2
                    ae_send_command 0 \
                        "matrix_multiply.bin -l 2000 -r 2000 -c 0 -t $matrix_threads"
                fi
                bench_timeout="$TIMEOUT"
            else
                if ! ae_boot_cluster "$cfg_machines" "$STATE_PARTITION_CPU_NUM"; then
                    ae_record_error "boot failed for $bench under $cfg ($cfg_machines machines)"
                    ae_archive_logs "$cfg_machines" "$AE_LOG_DIR" \
                        "-boot-failed-${point}"
                    continue
                fi
                ae_send_command 0 "source run_${bench}.sh"
                bench_timeout="$TIMEOUT"
            fi
            if wait_for_bench \
                "$bench" "$pattern" "$bench_timeout" "$bench done" "$cfg_machines"; then
                sleep 3   # let trailing output (e.g. summary lines) flush
                cp "$(ae_machine_log 0)" "$logfile"
            else
                # rc 1 (timeout) or 3 (guest error/crash) — the specific reason
                # is already recorded above; save the log and skip to the next
                # test.
                cp "$(ae_machine_log 0)" "$logfile" || true
                ae_archive_logs "$cfg_machines" "$AE_LOG_DIR" \
                    "-failed-${point}"
                echo "[WARN] $bench under $cfg ($cfg_machines machines) did not complete; skipping to next test" >&2
                ae_record_error "$bench under $cfg ($cfg_machines machines) did not produce a complete result"
            fi
            ae_kill_cluster
        done
    done
done

ae_restore_build_configs

echo ""
echo "=== Parsing logs and generating figure ==="
plot_args=(--log-dir "$AE_LOG_DIR" --csv-dir "$CSV_DIR" --fig-dir "$FIG_DIR")
# shellcheck disable=SC2086
plot_args+=(--machine-counts $MACHINE_COUNTS)
for bench in $BENCHS; do
    for cfg in $CONFIGS; do
        if [ "$cfg" = "All_DRAM" ]; then
            plot_args+=(--require-point "$bench/$cfg/1")
        else
            for count in $MACHINE_COUNTS; do
                plot_args+=(--require-point "$bench/$cfg/$count")
            done
        fi
    done
done
if [ "$FULL_PLOT_REQUEST" != "1" ]; then
    # BENCHS/CONFIGS are supported subset controls.  Partial mode relaxes only
    # the other 24-point completeness checks; every requested Cartesian point
    # remains mandatory through --require-point.
    echo "[AE] subset run requested; plotting only the available points."
    plot_args+=(--allow-partial)
fi
python3 "$AE_DIR/plot.py" "${plot_args[@]}"

echo "Artifact output: $OUT_DIR"
ae_finish
