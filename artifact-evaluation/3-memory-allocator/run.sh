#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AE_DIR="$REPO_ROOT/artifact-evaluation/3-memory-allocator"
NRUNS="${NRUNS:-3}"
RUN_OFFSET="${RUN_OFFSET:-0}"
USER_BENCH_THREADS="${USER_BENCH_THREADS:-1 2 4 8 16 32 64 96}"
CPU_NUM="${CPU_NUM:-96}"
ALLOCATOR_CONFIGS="${ALLOCATOR_CONFIGS:-llfree_cr_on:ON:ON:DEFAULT_DRAM:DRAM llfree_cr_off:ON:OFF:DEFAULT_CXL:MIXED_DEFAULT_CXL buddy_cr_off:OFF:OFF:DEFAULT_CXL:MIXED_DEFAULT_CXL}"
RUN_KERNEL_BENCHMARKS="${RUN_KERNEL_BENCHMARKS:-1}"
RUN_USER_BENCHMARKS="${RUN_USER_BENCHMARKS:-1}"
DRAW_FIGURES="${DRAW_FIGURES:-1}"
export CHBUILD_JOBS="${CHBUILD_JOBS:-32}"
# Each allocator sample must start without persistent filesystem/recovery
# state from the previous guest. Host cache dropping is ineffective on the
# AE host without passwordless sudo and does not affect the in-guest malloc
# working set.
export AE_RECREATE_CXLFS="${AE_RECREATE_CXLFS:-1}"
export AE_DROP_CACHES="${AE_DROP_CACHES:-0}"
PROJECT_CONFIG="$REPO_ROOT/.config"
PROJECT_INI="$REPO_ROOT/chcore.ini"
LOCK_FILE="$AE_DIR/.run.lock"

source "$REPO_ROOT/artifact-evaluation/common.sh"

if [ "${MEMORY_ALLOCATOR_LOCK_HELD:-0}" != "1" ]; then
    export MEMORY_ALLOCATOR_LOCK_HELD=1
    # The flock parent owns the lock. --close prevents QEMU/ivshmem children
    # from inheriting its descriptor and keeping the lock after this run exits.
    exec flock --close --nonblock "$LOCK_FILE" "$0" "$@"
fi

ae_init_output_dirs "$AE_DIR"
CSV_FILE="$CSV_DIR/allocator_results.csv"

PROJECT_CONFIG_BACKUP="$(mktemp)"
PROJECT_INI_BACKUP="$(mktemp)"
cp "$PROJECT_CONFIG" "$PROJECT_CONFIG_BACKUP"
cp "$PROJECT_INI" "$PROJECT_INI_BACKUP"

ae_ensure_clean_tmux
ae_check_global_prepare

# Optional: recreate guest DRAM backing (numa*.*) without pinning each file to
# a single host NUMA node. Default off.
#   AE_UNBIND_DRAM_NUMA=1           → interleave across host CPU nodes 0-3
#   AE_UNBIND_DRAM_NUMA=off         → plain dd (often still lands on one node)
#   AE_UNBIND_DRAM_NUMA=interleave  → same as 1
# EXIT restores the normal per-file membind layout.
AE_UNBIND_DRAM_NUMA="${AE_UNBIND_DRAM_NUMA:-0}"
AE_NUMA_UNBOUND_ACTIVE=0

cd "$REPO_ROOT"

# Load benchmark execution/parsing helpers only. Configuration selection and
# compilation intentionally live in this AE entry point.
BENCH_MALLOC_LIB_ONLY=1
# shellcheck source=../../dsm-scripts/malloc/bench_malloc_e2e.sh
source "$REPO_ROOT/dsm-scripts/malloc/bench_malloc_e2e.sh"

cleanup_ae_config() {
    ae_kill_all_ae_sessions
    if [ "${AE_NUMA_UNBOUND_ACTIVE:-0}" = "1" ]; then
        echo "[AE] Restoring host-NUMA-bound guest DRAM for machine 0 (numa0.0)"
        ae_ensure_clean_tmux
        if ! "$REPO_ROOT/artifact-evaluation/3-memory-allocator/recreate_numa_dram.sh" bind; then
            echo "[AE] WARNING: failed to restore NUMA-bound dram devices" >&2
        fi
        AE_NUMA_UNBOUND_ACTIVE=0
    fi
    restore_config
    cp "$PROJECT_CONFIG_BACKUP" "$PROJECT_CONFIG"
    cp "$PROJECT_INI_BACKUP" "$PROJECT_INI"
    rm -f "$PROJECT_CONFIG_BACKUP" "$PROJECT_INI_BACKUP"
}
trap cleanup_ae_config EXIT

if [ "$AE_UNBIND_DRAM_NUMA" = "1" ] || [ "$AE_UNBIND_DRAM_NUMA" = "interleave" ] || [ "$AE_UNBIND_DRAM_NUMA" = "off" ]; then
    local_mode=interleave
    if [ "$AE_UNBIND_DRAM_NUMA" = "off" ]; then
        local_mode=off
    fi
    echo "[AE] Recreating guest DRAM numa0.0 with mode=$local_mode (AE_UNBIND_DRAM_NUMA=$AE_UNBIND_DRAM_NUMA)"
    ae_ensure_clean_tmux
    "$REPO_ROOT/artifact-evaluation/3-memory-allocator/recreate_numa_dram.sh" "$local_mode"
    AE_NUMA_UNBOUND_ACTIVE=1
fi

CSV_OUT="$CSV_FILE"
export CPU_NUM

echo "[AE] Output directory: $OUT_DIR"
echo "[AE] Guest CPUs: $CPU_NUM"
echo "[AE] Kernel parallel levels: 1 4 8 16 32 48 64 96"
echo "[AE] User guest CPUs: $USER_GUEST_CPU_NUM"
echo "[AE] User threads: $USER_BENCH_THREADS"
echo "[AE] User measured loops/thread: $USER_BENCH_LOOPS"

echo "[AE] Enabling user allocator benchmark in .config"
sed -i 's/^CHCORE_BUILD_USER_MALLOC_TESTS:BOOL=.*/CHCORE_BUILD_USER_MALLOC_TESTS:BOOL=ON/' \
    "$PROJECT_CONFIG"
grep -q '^CHCORE_BUILD_USER_MALLOC_TESTS:BOOL=ON$' "$PROJECT_CONFIG"

echo "[AE] Setting compile-time CPU maximum to $CPU_NUM"
ae_set_paper_guest_cpu_config "$CPU_NUM"
ae_export_guest_cpu_num "$CPU_NUM"

echo "[AE] Saving kernel/dsm_config.cmake"
save_config
echo "config,memory,test,parallel,run,ops_per_sec" > "$CSV_OUT"

run_configuration() {
    local label="$1" llfree="$2" crash_recovery="$3"
    local user_malloc_mode="${4:-DEFAULT_CXL}"
    local malloc_mode="${5:-MIXED_DEFAULT_CXL}"
    echo "=== Configuring $label: DSM_CXL_LF_BUDDY=$llfree, SLAB_CRASH_RECOVERY=$crash_recovery, DSM_USER_MALLOC_MODE=$user_malloc_mode, DSM_MALLOC_MODE=$malloc_mode ==="
    set_cmake_var DSM_CXL_LF_BUDDY "$llfree"
    set_cmake_var SLAB_CRASH_RECOVERY "$crash_recovery"
    set_cmake_var DSM_USER_MALLOC_MODE "$user_malloc_mode"
    set_cmake_var DSM_MALLOC_MODE "$malloc_mode"

    if [ "$RUN_KERNEL_BENCHMARKS" = "1" ]; then
        echo "=== Building $label with CHCORE_KERNEL_TEST=ON ==="
        sed -i 's/^CHCORE_KERNEL_TEST:BOOL=.*/CHCORE_KERNEL_TEST:BOOL=ON/' "$PROJECT_CONFIG"
        grep -q '^CHCORE_KERNEL_TEST:BOOL=ON$' "$PROJECT_CONFIG"
        build_current_config "${label}_kernel"
        run_kernel_benchmarks "$label"
    fi

    if [ "$RUN_USER_BENCHMARKS" = "1" ]; then
        echo "=== Building $label with CHCORE_KERNEL_TEST=OFF for user malloc ==="
        sed -i 's/^CHCORE_KERNEL_TEST:BOOL=.*/CHCORE_KERNEL_TEST:BOOL=OFF/' "$PROJECT_CONFIG"
        grep -q '^CHCORE_KERNEL_TEST:BOOL=OFF$' "$PROJECT_CONFIG"
        build_current_config "${label}_user"
        run_user_benchmarks "$label"
    fi
}

for config_spec in $ALLOCATOR_CONFIGS; do
    IFS=: read -r config_label config_llfree config_cr config_user_malloc config_malloc_mode <<< "$config_spec"
    run_configuration "$config_label" "$config_llfree" "$config_cr" \
        "${config_user_malloc:-DEFAULT_CXL}" \
        "${config_malloc_mode:-MIXED_DEFAULT_CXL}"
done

echo "[AE] Restoring kernel/dsm_config.cmake"
restore_config

echo "=== Parsing allocator logs ==="
for entry in "LLFree+CR:llfree_cr_on" "LLFree:llfree_cr_off" "Buddy:buddy_cr_off"; do
    config="${entry%%:*}"
    label="${entry##*:}"
    for run in $(seq 1 "$NRUNS"); do
        absolute_run=$((run + RUN_OFFSET))
        parse_kernel_log "$LOG_DIR/${label}_run${absolute_run}_kernel.log" \
            "$config" "$absolute_run" >> "$CSV_OUT"
        for threads in $USER_BENCH_THREADS; do
            parse_user_log "$LOG_DIR/${label}_run${absolute_run}_user_t${threads}.log" \
                "$config" "$absolute_run" >> "$CSV_OUT"
        done
    done
done

if [ "$DRAW_FIGURES" = "1" ]; then
    echo "=== Drawing allocator-all figure ==="
    MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/matplotlib-$USER}" \
        python3 "$AE_DIR/plot.py" --csv "$CSV_FILE" --fig-dir "$FIG_DIR"
fi
echo "Artifact output: $OUT_DIR"
