#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AE_DIR="$REPO_ROOT/artifact-evaluation/3-memory-allocator"
LOG_DIR="$AE_DIR/logs"
CSV_FILE="$AE_DIR/allocator_results.csv"
NRUNS="${NRUNS:-1}"
RUN_OFFSET="${RUN_OFFSET:-0}"
USER_BENCH_THREADS="${USER_BENCH_THREADS:-1 2 4 8 16 32 64 96}"
CPU_NUM="${CPU_NUM:-96}"
CLEAN_RESULTS="${CLEAN_RESULTS:-1}"
PROJECT_CONFIG="$REPO_ROOT/.config"
PROJECT_INI="$REPO_ROOT/chcore.ini"
LOCK_FILE="$AE_DIR/.run.lock"

mkdir -p "$LOG_DIR"
if [ "${MEMORY_ALLOCATOR_LOCK_HELD:-0}" != "1" ]; then
    export MEMORY_ALLOCATOR_LOCK_HELD=1
    # The flock parent owns the lock. --close prevents QEMU/ivshmem children
    # from inheriting its descriptor and keeping the lock after this run exits.
    exec flock --close --nonblock "$LOCK_FILE" "$0" "$@"
fi

if [ "$CLEAN_RESULTS" = "1" ]; then
    find "$LOG_DIR" -mindepth 1 -maxdepth 1 -type f -delete
    rm -f "$CSV_FILE" \
        "$AE_DIR/allocator_summary.csv" \
        "$AE_DIR/allocator_overview.png" \
        "$AE_DIR/allocator_cxl.png" \
        "$AE_DIR/user_malloc.png" \
        "$AE_DIR/fig00-allocator-all.png" \
        "$AE_DIR/fig00-allocator-all.pdf" \
        "$AE_DIR/fig00-allocator-all.eps"
fi

PROJECT_CONFIG_BACKUP="$(mktemp)"
PROJECT_INI_BACKUP="$(mktemp)"
cp "$PROJECT_CONFIG" "$PROJECT_CONFIG_BACKUP"
cp "$PROJECT_INI" "$PROJECT_INI_BACKUP"

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"
ae_check_global_prepare

cd "$REPO_ROOT"

# Load benchmark execution/parsing helpers only. Configuration selection and
# compilation intentionally live in this AE entry point.
BENCH_MALLOC_LIB_ONLY=1
# shellcheck source=../../dsm-scripts/malloc/bench_malloc_e2e.sh
source "$REPO_ROOT/dsm-scripts/malloc/bench_malloc_e2e.sh"

cleanup_ae_config() {
    ae_kill_all_ae_sessions
    restore_config
    cp "$PROJECT_CONFIG_BACKUP" "$PROJECT_CONFIG"
    cp "$PROJECT_INI_BACKUP" "$PROJECT_INI"
    rm -f "$PROJECT_CONFIG_BACKUP" "$PROJECT_INI_BACKUP"
}
trap cleanup_ae_config EXIT

CSV_OUT="$CSV_FILE"
LOG_DIR="$AE_DIR/logs"
NRUNS="${NRUNS:-1}"
RUN_OFFSET="${RUN_OFFSET:-0}"
USER_BENCH_THREADS="${USER_BENCH_THREADS:-1 2 4 8 16 32 64 96}"
export CPU_NUM
mkdir -p "$LOG_DIR"

echo "[AE] Output directory: $AE_DIR"
echo "[AE] Guest CPUs: $CPU_NUM"
echo "[AE] Kernel parallel levels: 1 4 8 16 32 48 64 96"
echo "[AE] User threads: $USER_BENCH_THREADS"

echo "[AE] Enabling user allocator benchmark in .config"
sed -i 's/^CHCORE_BUILD_USER_MALLOC_TESTS:BOOL=.*/CHCORE_BUILD_USER_MALLOC_TESTS:BOOL=ON/' \
    "$PROJECT_CONFIG"
grep -q '^CHCORE_BUILD_USER_MALLOC_TESTS:BOOL=ON$' "$PROJECT_CONFIG"

echo "[AE] Setting compile-time CPU maximum to $CPU_NUM"
sed -i "s/^cpu_num[[:space:]]*=.*/cpu_num = $CPU_NUM/" "$PROJECT_INI"
grep -q "^cpu_num[[:space:]]*=[[:space:]]*$CPU_NUM$" "$PROJECT_INI"
sed -i "s/^CHCORE_PLAT_CPU_NUM:STRING=.*/CHCORE_PLAT_CPU_NUM:STRING=$CPU_NUM/" \
    "$PROJECT_CONFIG"
grep -q "^CHCORE_PLAT_CPU_NUM:STRING=$CPU_NUM$" "$PROJECT_CONFIG"

echo "[AE] Saving kernel/dsm_config.cmake"
save_config
echo "config,memory,test,parallel,run,ops_per_sec" > "$CSV_OUT"

run_configuration() {
    local label="$1" llfree="$2" crash_recovery="$3"
    echo "=== Configuring $label: DSM_CXL_LF_BUDDY=$llfree, SLAB_CRASH_RECOVERY=$crash_recovery ==="
    set_cmake_var DSM_CXL_LF_BUDDY "$llfree"
    set_cmake_var SLAB_CRASH_RECOVERY "$crash_recovery"

    echo "=== Building $label with CHCORE_KERNEL_TEST=ON ==="
    sed -i 's/^CHCORE_KERNEL_TEST:BOOL=.*/CHCORE_KERNEL_TEST:BOOL=ON/' "$PROJECT_CONFIG"
    grep -q '^CHCORE_KERNEL_TEST:BOOL=ON$' "$PROJECT_CONFIG"
    build_current_config "${label}_kernel"
    run_kernel_benchmarks "$label"

    echo "=== Building $label with CHCORE_KERNEL_TEST=OFF for user malloc ==="
    sed -i 's/^CHCORE_KERNEL_TEST:BOOL=.*/CHCORE_KERNEL_TEST:BOOL=OFF/' "$PROJECT_CONFIG"
    grep -q '^CHCORE_KERNEL_TEST:BOOL=OFF$' "$PROJECT_CONFIG"
    build_current_config "${label}_user"
    run_user_benchmarks "$label"
}

run_configuration llfree_cr_on ON ON
run_configuration llfree_cr_off ON OFF
run_configuration buddy_cr_off OFF OFF

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

echo "=== Drawing fig00-allocator-all.png ==="
MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/matplotlib-$USER}" \
    python3 "$AE_DIR/plot.py" --csv "$CSV_FILE" --out-dir "$AE_DIR"
echo "Artifact output: $AE_DIR"
