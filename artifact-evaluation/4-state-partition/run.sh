#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi
#
# Artifact script for paper Figure 13: "Performance across state-partition
# choices".
#
# Camera-ready: only the 8-machine cluster is measured.  The three shared
# placements run at every cluster size in MACHINE_COUNTS (default "8"); the
# multi-size machinery is kept so a smaller ablation is one env var away
# (MACHINE_COUNTS="4 8" reinstates the two-panel sweep).  Each QEMU guest uses
# 12 vCPUs, so the default cluster is 8 x 12 = 96 vCPUs — matching the paper
# testbed.
#
# All six benchmarks scale their worker count with the panel (8 per machine,
# WORKERS_PER_MACHINE), so a panel really is a bigger cluster doing the same
# fixed-size work.  Sourcing the ramdisk run_<bench>.sh scripts instead ran
# LevelDB and the three non-Matrix Phoenix apps as a fixed single-machine
# 8-thread workload, so their 4- and 8-machine panels were the identical run
# and nothing but machine 0 was ever exercised.
#
# Private (All_DRAM) is the single-machine ideal baseline and gets its own
# point per cluster size, at the SAME total worker count as the shared
# placements at that size: one guest with <machines> x 12 vCPUs running
# <machines> x 8 workers on the same CPU pattern (the per-machine segments
# all live inside that one guest).  It used to be a single 12-vCPU / 8-worker run
# reused by every panel, which normalized 32- and 64-worker cluster points
# against an 8-worker baseline and reported scale-out speedup (Matrix
# Multiply read ~3x) as if it were a placement effect.
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
#   MACHINE_COUNTS="8"   TIMEOUT=1200
#   (use MACHINE_COUNTS=2 for a smaller ablation / debug, or "4 8" for the
#   two-panel sweep; NUM_MACHINES=N is accepted as a legacy alias for
#   MACHINE_COUNTS="N")
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/4-state-partition"
ae_init_output_dirs "$AE_DIR"
AE_LOG_DIR="$LOG_DIR"
# Cluster sizes.  Every config produces one point per size; the shared
# placements run on that many machines, Private runs the same total worker
# count on a single larger guest.  The figure only needs the 8-machine panel,
# so that is the default; pass a space-separated list for a wider sweep.
# NUM_MACHINES=N is kept as a legacy single-size alias.
MACHINE_COUNTS="${MACHINE_COUNTS:-${NUM_MACHINES:-8}}"
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
    if [ -n "${ZSH_VERSION:-}" ]; then
        read -r -A values <<< "$raw"
    else
        read -r -a values <<< "$raw"
    fi
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

# Points that are known not to be measurable on this host and are deliberately
# left blank in the figure.  Format: BENCH/CONFIG/MACHINES, space separated.
# There are no default skips.  Skipping a Private point also removes the
# baseline that benchmark normalizes against, so use this only for deliberate
# subset/debug runs.
SKIP_POINTS="${SKIP_POINTS:-}"
for entry in $SKIP_POINTS; do
    IFS=/ read -r skip_bench skip_cfg skip_count <<< "$entry"
    if ! [[ " $BENCHS " == *" $skip_bench "* ]] \
            || ! [[ " $CONFIGS " == *" $skip_cfg "* ]] \
            || ! [[ " $MACHINE_COUNTS " == *" $skip_count "* ]]; then
        echo "[AE] SKIP_POINTS entry does not name a point of this run: $entry" >&2
        echo "[AE] expected BENCH/CONFIG/MACHINES drawn from BENCHS, CONFIGS," \
             "MACHINE_COUNTS" >&2
        exit 1
    fi
done

point_is_skipped() {
    local want="$1/$2/$3" entry
    for entry in $SKIP_POINTS; do
        [ "$entry" = "$want" ] && return 0
    done
    return 1
}

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
        # bench_command() runs a single db_bench benchmark (fillbatch), whose
        # one result line carries both micros/op and MB/s.  plot.py parses that
        # same line by name; keep the two in step if the workload gains a
        # second benchmark, or this marker will fire on the wrong one.
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
# 1.5x the slowest successful dbx1000 point measured across the clean runs in
# out/ (322 s, an 8-machine point under host contention; the median is ~115 s).
# The old 3600 s was 31x the median and only served to make a hung or OOMing
# point take an hour to report.
DBX_TIMEOUT="${DBX_TIMEOUT:-480}"
DBX_DRAM_SIZE="${DBX_DRAM_SIZE:-24G}"
DBX_PRIVATE_DRAM_DEVICE="${DBX_PRIVATE_DRAM_DEVICE:-/dev/shm/numa0.0-$USER}"
DBX_PRIVATE_DRAM_SIZE="${DBX_PRIVATE_DRAM_SIZE:-32G}"
# NOTE: DBX_DRAM_SIZE sets QEMU's -m and the dram_size bootarg, but it does
# NOT size the kernel's local DRAM pool.  Each machine's DRAM is exactly one
# ivshmem device -- dsm_metadata.c takes dram_devices_map[CUR_MACHINE_ID],
# whose size comes from dsm-scripts/numa_sizes.conf (16 GiB per device today).
# The 8-machine-equivalent Private DBx1000 point temporarily expands machine
# 0's backing file to DBX_PRIVATE_DRAM_SIZE, then restores its original size as
# soon as that one point stops.  Changing DBX_DRAM_SIZE alone does not do this.
# TPC-C setup follows the paper (and 8-dbx1000-cross-warehouse).  The warmup
# matters for state placement: with a short warmup the measured interval is
# dominated by the one-time first-touch DRAM->CXL page migration, which made
# every DRAM-first placement look 10-30x slower than a single machine
# (K-mix/U-mix at 8 machines: 0.01 Mtxn/s at warmup 10000 vs 0.48 Mtxn/s here).
DBX_WARMUP="${DBX_WARMUP:-7040000}"
DBX_MAX_TXN="${DBX_MAX_TXN:-10000}"
DBX_ITEM_I_DATA_LEN="${DBX_ITEM_I_DATA_LEN:-1000}"
# config.h is shared with 8-dbx1000-cross-warehouse, which writes its own
# cross-warehouse ratio and timed measurement window into it.  Pin every knob
# that experiment touches or this figure silently inherits them: a smoke run on
# 2026-07-31 came up as transaction_cross_warehouse_mode=1 /
# cross_warehouse_txn_pct=15 / measure_duration_sec=5, none of which the
# Figure 13 data uses.  Mode 0 plus MEASURE_DURATION_SEC=0 is "run to
# MAX_TXN_PER_PART", matching the paper.
#
# The pinned set must stay a superset of 8-dbx1000-cross-warehouse's
# set_dbx_define calls; LOAD_UNUSED_TABLES is here for that reason even though
# both experiments happen to want false today.
DBX_CROSS_WAREHOUSE_RATIO_MODE="${DBX_CROSS_WAREHOUSE_RATIO_MODE:-false}"
DBX_CROSS_WAREHOUSE_PCT="${DBX_CROSS_WAREHOUSE_PCT:-15}"
DBX_MEASURE_DURATION_SEC="${DBX_MEASURE_DURATION_SEC:-0}"
DBX_LOAD_UNUSED_TABLES="${DBX_LOAD_UNUSED_TABLES:-false}"
# Worker threads per machine, for every benchmark: the `-t` / `--threads` of
# the Phoenix apps, LevelDB and Matrix Multiply, and DBx1000's compile-time
# THREADS_PER_MACHINE.  A point at cluster size N always runs N * this many
# workers in total, whether they are spread over N machines (shared configs)
# or run on one larger guest (Private).  MATRIX_THREADS_PER_MACHINE is the
# legacy name from when only Matrix Multiply scaled.
WORKERS_PER_MACHINE="${WORKERS_PER_MACHINE:-${MATRIX_THREADS_PER_MACHINE:-8}}"

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

# Warehouses scale with the cluster, matching 5-auto-scale and the paper's
# DBx1000 setup (eight warehouses per machine, one per worker).  A fixed total
# instead makes every worker beyond the first cluster size contend for the same
# warehouses: 64 workers over 8 warehouses aborted ~90% of transactions, so the
# point measured TPC-C lock contention rather than state placement.
# DBX_NUM_WH still pins a fixed total for a deliberate contention study.
DBX_WH_PER_MACHINE="${DBX_WH_PER_MACHINE:-$WORKERS_PER_MACHINE}"
if ! [[ "$DBX_WH_PER_MACHINE" =~ ^[1-9][0-9]*$ ]]; then
    echo "DBX_WH_PER_MACHINE must be a positive integer: $DBX_WH_PER_MACHINE" >&2
    exit 1
fi
if [ -n "${DBX_NUM_WH:-}" ]; then
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
fi

# dbx_warehouses <machines>
dbx_warehouses() {
    if [ -n "${DBX_NUM_WH:-}" ]; then
        echo "$DBX_NUM_WH"
    else
        echo "$((DBX_WH_PER_MACHINE * $1))"
    fi
}

# Convert the integer binary sizes accepted by QEMU/coreutils (for example,
# 32G or 34359738368) into bytes.  ivshmem BAR sizes must be powers of two, so
# the temporary size is validated before the backing file is touched.
binary_size_to_bytes() {
    local raw="$1" number multiplier=1
    case "$raw" in
        *[Kk]) number="${raw%?}"; multiplier=$((1024)) ;;
        *[Mm]) number="${raw%?}"; multiplier=$((1024 * 1024)) ;;
        *[Gg]) number="${raw%?}"; multiplier=$((1024 * 1024 * 1024)) ;;
        *)     number="$raw" ;;
    esac
    if ! [[ "$number" =~ ^[1-9][0-9]*$ ]]; then
        echo "[AE] invalid binary size: $raw" >&2
        return 1
    fi
    echo "$((number * multiplier))"
}

PRIVATE_DRAM_RESIZE_ACTIVE=0
PRIVATE_DRAM_ORIGINAL_SIZE_BYTES=""

expand_private_dram_for_dbx_m8() {
    local target_bytes current_bytes owner

    if [ "$PRIVATE_DRAM_RESIZE_ACTIVE" = "1" ]; then
        echo "[AE] Private DRAM resize is already active" >&2
        return 1
    fi
    if [ -L "$DBX_PRIVATE_DRAM_DEVICE" ] || [ ! -f "$DBX_PRIVATE_DRAM_DEVICE" ]; then
        echo "[AE] invalid Private DRAM backing file: $DBX_PRIVATE_DRAM_DEVICE" >&2
        return 1
    fi
    owner="$(stat -Lc '%u' -- "$DBX_PRIVATE_DRAM_DEVICE" 2>/dev/null || true)"
    if [ "$owner" != "$UID" ]; then
        echo "[AE] refusing Private DRAM backing file not owned by uid $UID: $DBX_PRIVATE_DRAM_DEVICE" >&2
        return 1
    fi
    if ae_has_chcore_qemu; then
        echo "[AE] refusing to resize Private DRAM while a ChCore QEMU is running" >&2
        return 1
    fi
    target_bytes="$(binary_size_to_bytes "$DBX_PRIVATE_DRAM_SIZE")" || return 1
    if (( (target_bytes & (target_bytes - 1)) != 0 )); then
        echo "[AE] DBX_PRIVATE_DRAM_SIZE must be a power of two: $DBX_PRIVATE_DRAM_SIZE" >&2
        return 1
    fi
    current_bytes="$(stat -Lc '%s' -- "$DBX_PRIVATE_DRAM_DEVICE")" || return 1
    if [ "$target_bytes" -lt "$current_bytes" ]; then
        echo "[AE] refusing to shrink Private DRAM for DBx1000: $current_bytes -> $target_bytes" >&2
        return 1
    fi
    if [ "$target_bytes" -eq "$current_bytes" ]; then
        echo "[AE] Private DRAM already has the requested size: $current_bytes bytes"
        return 0
    fi

    PRIVATE_DRAM_ORIGINAL_SIZE_BYTES="$current_bytes"
    PRIVATE_DRAM_RESIZE_ACTIVE=1
    echo "[AE] Temporarily expanding $DBX_PRIVATE_DRAM_DEVICE: $current_bytes -> $target_bytes bytes"
    if command -v numactl >/dev/null 2>&1 && command -v fallocate >/dev/null 2>&1; then
        # numa0.0 is backed by host NUMA node 0.  Preallocation keeps the new
        # half on that same node instead of first-touching it from arbitrary
        # QEMU vCPU threads.
        if ! numactl --membind=0 fallocate -l "$target_bytes" -- "$DBX_PRIVATE_DRAM_DEVICE"; then
            restore_private_dram_backing || true
            return 1
        fi
    elif ! truncate -s "$target_bytes" -- "$DBX_PRIVATE_DRAM_DEVICE"; then
        restore_private_dram_backing || true
        return 1
    fi
    if [ "$(stat -Lc '%s' -- "$DBX_PRIVATE_DRAM_DEVICE")" != "$target_bytes" ]; then
        echo "[AE] Private DRAM backing-file expansion did not reach $target_bytes bytes" >&2
        restore_private_dram_backing || true
        return 1
    fi
}

restore_private_dram_backing() {
    local restore_bytes
    [ "$PRIVATE_DRAM_RESIZE_ACTIVE" = "1" ] || return 0
    restore_bytes="$PRIVATE_DRAM_ORIGINAL_SIZE_BYTES"
    if ae_has_chcore_qemu; then
        echo "[AE] cannot restore Private DRAM while a ChCore QEMU is running" >&2
        return 1
    fi
    echo "[AE] Restoring $DBX_PRIVATE_DRAM_DEVICE to $restore_bytes bytes"
    truncate -s "$restore_bytes" -- "$DBX_PRIVATE_DRAM_DEVICE" || return 1
    if [ "$(stat -Lc '%s' -- "$DBX_PRIVATE_DRAM_DEVICE")" != "$restore_bytes" ]; then
        echo "[AE] failed to restore Private DRAM backing-file size" >&2
        return 1
    fi
    PRIVATE_DRAM_RESIZE_ACTIVE=0
    PRIVATE_DRAM_ORIGINAL_SIZE_BYTES=""
}

ae_acquire_run_lock "state-partition" || exit 1

mkdir -p "$AE_LOG_DIR" "$CSV_DIR" "$FIG_DIR"

TMP_DIR="$(mktemp -d)"
cp "$DBX_CONFIG" "$TMP_DIR/config.h"

# Eight worker CPUs on each 12-vCPU machine segment: 0-7,12-19,24-31,...
# This is a global CPU namespace consumed by every workload's bind file; for
# Private the segments all live inside its one wide guest.
worker_bind_cpu_list() {
    local n="$1" i parts=()
    for i in $(seq 0 $((n - 1))); do
        parts+=("$((i * STATE_PARTITION_CPU_NUM))-$((i * STATE_PARTITION_CPU_NUM + WORKERS_PER_MACHINE - 1))")
    done
    (IFS=,; echo "${parts[*]}")
}

# bench -> the guest CPU-binding file its launcher reads.
bench_bind_file() {
    case "$1" in
        leveldb)            echo "leveldb_bind_cpu.txt" ;;
        dbx1000)            echo "dbx1000_bind_cpu.txt" ;;
        pca)                echo "pca_bind_cpu.txt" ;;
        matrix_multiply)    echo "matrix_multiply_bind_cpu.txt" ;;
        linear_regression)  echo "linear_regression_bind_cpu.txt" ;;
        word_count)         echo "word_count_bind_cpu.txt" ;;
        *) echo "Unknown bench: $1" >&2; return 1 ;;
    esac
}

# bench <workers> -> the guest command line.
#
# Datasets are fixed, so a larger panel divides the same work over more
# workers (strong scaling) — the same regime Matrix Multiply and DBx1000
# already used.  This is what makes the 4- and 8-machine panels different
# workloads at all: the ramdisk run_<bench>.sh scripts hardcode a binding that
# does not depend on the panel — 0-11 / -t 8 for LevelDB and the three
# non-Matrix Phoenix apps, 0-7,12-19 / -t 16 for Matrix Multiply — so sourcing
# them ran the identical workload in both panels and the shared placements
# never touched more than one or two machines.
bench_command() {
    local bench="$1" workers="$2" cmd
    case "$bench" in
        # Only the flags db_bench actually parses: an unrecognized one is a
        # hard "Invalid flag" + exit(1) (benchmarks/db_bench.cc), not a
        # warning.  In particular there is no --background_cpu in the pinned
        # submodule (6fc338e), so background compaction is left unpinned, as
        # it is in user/script/run_leveldb.sh.
        leveldb)
            echo "leveldb-dbbench.bin --benchmarks=fillbatch --num=100000" \
                 "--db=/tmp --threads=$workers --write_num_is_total=1"
            ;;
        # DBx1000's worker count is compile-time (THREADS_PER_MACHINE *
        # NUM_MACHINES), applied to config.h before the build below.
        dbx1000)            echo "rundb.bin" ;;
        pca)                echo "pca.bin -c 1000 -r 1000 -t $workers" ;;
        matrix_multiply)    echo "matrix_multiply.bin -l 2000 -r 2000 -c 0 -t $workers" ;;
        linear_regression)  echo "linear_regression.bin -f key_file_100MB.txt -t $workers" ;;
        word_count)         echo "word_count.bin -f word_100MB.txt -t $workers" ;;
        *) echo "Unknown bench: $1" >&2; return 1 ;;
    esac
}

cleanup() {
    local rc=$? cleanup_failed=0
    trap - EXIT
    ae_kill_cluster || cleanup_failed=1
    restore_private_dram_backing || cleanup_failed=1
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
# Keep the kernel's per-machine CPU stride and QEMU's SMP count in sync.  Each
# point re-applies this for the guest it is about to boot (Private's baseline
# guest is <panel> x STATE_PARTITION_CPU_NUM wide); the saved configuration is
# restored by cleanup after this experiment.
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

    ae_set_dsm_var DSM_MALLOC_MODE "$malloc_mode"
    ae_set_dsm_var DSM_USER_MALLOC_MODE "$user_malloc_mode"
    for t in THREADCTX PGTABLE STACK OBJECT PAGE; do
        ae_set_dsm_var "DSM_${t}_MODE" "$type_mode"
    done

    for cfg_size in $MACHINE_COUNTS; do
        # cfg_size is the panel (cluster size) this point belongs to; it fixes
        # the total worker count.  Private realizes that panel on a single
        # guest, so the machines it actually boots and the vCPUs that guest
        # needs are derived from it rather than equal to it.
        if [ "$cfg" = "All_DRAM" ]; then
            # With all state forced into private DRAM, cross-machine sharing is
            # impossible by design (2-machine runs crash in virt_to_page), so
            # the baseline is always one machine — but one machine wide enough
            # to hold the panel's whole worker set.
            cfg_machines=1
            cfg_cpus=$((cfg_size * STATE_PARTITION_CPU_NUM))
        else
            cfg_machines="$cfg_size"
            cfg_cpus="$STATE_PARTITION_CPU_NUM"
        fi
        cfg_workers=$((WORKERS_PER_MACHINE * cfg_size))
        cfg_bind_list="$(worker_bind_cpu_list "$cfg_size")"

        echo ""
        echo "########################################################"
        echo "### Config: $cfg (panel: $cfg_size machine(s))"
        echo "###   booting $cfg_machines machine(s) x $cfg_cpus vCPUs, $cfg_workers worker(s)"
        echo "###   DSM_MALLOC_MODE=$malloc_mode  DSM_USER_MALLOC_MODE=$user_malloc_mode"
        echo "###   THREADCTX/PGTABLE/STACK/OBJECT/PAGE=$type_mode"
        echo "########################################################"

        # dbx1000's compile-time NUM_MACHINES/PART_CNT must match the booted
        # cluster, and THREAD_CNT is THREADS_PER_MACHINE * NUM_MACHINES — so
        # Private carries the panel's whole worker set as one machine's share.
        # Each (config, panel) point therefore rebuilds.
        sed -i "s/^#define NUM_MACHINES[[:space:]].*/#define NUM_MACHINES\t\t\t$cfg_machines/" "$DBX_CONFIG"
        sed -i "s/^#define THREADS_PER_MACHINE[[:space:]].*/#define THREADS_PER_MACHINE\t\t\t$((cfg_workers / cfg_machines))/" "$DBX_CONFIG"
        cfg_warehouses="$(dbx_warehouses "$cfg_size")"
        sed -i "s/^#define NUM_WH[[:space:]].*/#define NUM_WH\t\t\t\t\t\t$cfg_warehouses/" "$DBX_CONFIG"
        sed -i "s/^#define WARMUP[[:space:]].*/#define WARMUP\t\t\t\t\t\t$DBX_WARMUP/" "$DBX_CONFIG"
        sed -i "s/^#define MAX_TXN_PER_PART[[:space:]].*/#define MAX_TXN_PER_PART\t\t\t$DBX_MAX_TXN/" "$DBX_CONFIG"
        sed -i "s/^#define ITEM_I_DATA_LEN[[:space:]].*/#define ITEM_I_DATA_LEN\t\t\t$DBX_ITEM_I_DATA_LEN/" "$DBX_CONFIG"
        sed -i "s/^#define USE_TRANSACTION_CROSS_WAREHOUSE_RATIO[[:space:]].*/#define USE_TRANSACTION_CROSS_WAREHOUSE_RATIO\t$DBX_CROSS_WAREHOUSE_RATIO_MODE/" "$DBX_CONFIG"
        sed -i "s/^#define PERC_CROSS_WAREHOUSE_TXN[[:space:]].*/#define PERC_CROSS_WAREHOUSE_TXN\t\t$DBX_CROSS_WAREHOUSE_PCT/" "$DBX_CONFIG"
        sed -i "s/^#define MEASURE_DURATION_SEC[[:space:]].*/#define MEASURE_DURATION_SEC\t\t\t$DBX_MEASURE_DURATION_SEC/" "$DBX_CONFIG"
        sed -i "s/^#define LOAD_UNUSED_TABLES[[:space:]].*/#define LOAD_UNUSED_TABLES\t\t\t$DBX_LOAD_UNUSED_TABLES/" "$DBX_CONFIG"
        # Compile-time CPU ceiling must cover the guest this point boots; the
        # saved configuration is restored by cleanup.
        ae_set_paper_guest_cpu_config "$cfg_cpus"
        ae_build

        for bench in $BENCHS; do
            pattern="$(bench_done_pattern "$bench")"
            point_resized_private_dram=0
            # Logs and plot points are keyed by the panel, not by the number of
            # guests: Private's m8 log is its 8-machine-equivalent baseline.
            logfile="$AE_LOG_DIR/${bench}_${cfg}_m${cfg_size}.log"
            point="${bench}-${cfg}-m${cfg_size}"
            if point_is_skipped "$bench" "$cfg" "$cfg_size"; then
                echo "=== [$cfg] SKIPPING $bench at the $cfg_size-machine panel (SKIP_POINTS) ==="
                continue
            fi
            # A targeted retry may reuse the same log directory.  Remove
            # this point before boot so a failed retry cannot be parsed as the
            # prior run's successful measurement.  After the skip check, so a
            # skipped point keeps whatever an earlier run measured for it.
            rm -f -- "$logfile"
            echo "=== [$cfg] running $bench for the $cfg_size-machine panel on $cfg_machines machine(s) (done pattern: '$pattern') ==="
            if [ "$bench" = "dbx1000" ] && [ "$cfg" = "All_DRAM" ] \
                    && [ "$cfg_size" = "8" ]; then
                if ! expand_private_dram_for_dbx_m8; then
                    ae_record_error "failed to expand Private DRAM for dbx1000/All_DRAM/8"
                    restore_private_dram_backing || true
                    continue
                fi
                point_resized_private_dram=1
            fi
            if [ "$bench" = "dbx1000" ]; then
                boot_env="DRAM_SIZE=$DBX_DRAM_SIZE"
                bench_timeout="$DBX_TIMEOUT"
            else
                boot_env=""
                bench_timeout="$TIMEOUT"
            fi
            if ! AE_EXTRA_ENV="$boot_env" \
                    ae_boot_cluster "$cfg_machines" "$cfg_cpus"; then
                ae_record_error "boot failed for $bench under $cfg (${cfg_size}-machine panel)"
                ae_archive_logs "$cfg_machines" "$AE_LOG_DIR" \
                    "-boot-failed-${point}"
                ae_kill_cluster
                if [ "$point_resized_private_dram" = "1" ] \
                        && ! restore_private_dram_backing; then
                    ae_record_error "failed to restore Private DRAM after ${point} boot failure"
                    exit 1
                fi
                continue
            fi
            # Every config at a given panel runs the same worker count on the
            # same CPU pattern (WORKERS_PER_MACHINE per 12-vCPU segment) — for
            # Private those segments are all inside its one wide guest.  See
            # bench_command() for why the ramdisk run_<bench>.sh scripts are
            # deliberately not sourced.
            ae_send_command 0 \
                "write $(bench_bind_file "$bench") $cfg_bind_list"
            sleep 2
            ae_send_command 0 "$(bench_command "$bench" "$cfg_workers")"
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
                echo "[WARN] $bench under $cfg (${cfg_size}-machine panel) did not complete; skipping to next test" >&2
                ae_record_error "$bench under $cfg (${cfg_size}-machine panel) did not produce a complete result"
            fi
            ae_kill_cluster
            if [ "$point_resized_private_dram" = "1" ] \
                    && ! restore_private_dram_backing; then
                ae_record_error "failed to restore Private DRAM after $point"
                exit 1
            fi
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
        for count in $MACHINE_COUNTS; do
            point_is_skipped "$bench" "$cfg" "$count" && continue
            plot_args+=(--require-point "$bench/$cfg/$count")
        done
    done
done
if [ "$FULL_PLOT_REQUEST" != "1" ] || [ -n "$SKIP_POINTS" ]; then
    # BENCHS/CONFIGS/SKIP_POINTS are supported subset controls.  Partial mode
    # relaxes only the completeness check over unrequested points; every point
    # this run actually attempted remains mandatory through --require-point.
    echo "[AE] subset run requested; plotting only the available points."
    plot_args+=(--allow-partial)
fi
python3 "$AE_DIR/plot.py" "${plot_args[@]}"

echo "Artifact output: $OUT_DIR"
ae_finish
