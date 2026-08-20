#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")/../.." && pwd)"
AE_DIR="$REPO_ROOT/artifact-evaluation/3-memory-allocator"
NRUNS="${NRUNS:-1}"
RUN_OFFSET="${RUN_OFFSET:-0}"
# The paper figure's user-malloc x axis.  plot.py hardcodes the same list and
# rejects a dataset that misses any of it, so a thinned sweep must tell the
# plotter that it is a subset (see PAPER_USER_BENCH_THREADS below).
PAPER_USER_BENCH_THREADS="1 2 4 8 16 32 64 96"
USER_BENCH_THREADS="${USER_BENCH_THREADS:-$PAPER_USER_BENCH_THREADS}"
CPU_NUM="${CPU_NUM:-96}"
# Build every configuration but run none of them.  The three configurations
# differ only in two cmake variables, and two of them (DSM_CXL_LF_BUDDY=ON)
# have broken the build before while the third kept working, so "do all three
# still compile" is worth asking on its own -- it costs six builds instead of
# an afternoon of boots.  No CSV or figure is produced.
BUILD_ONLY="${BUILD_ONLY:-0}"
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
# BUILD_ONLY boots no guest, so it needs neither the /dev/shm backing
# files nor the doorbell server.  Demanding them would make the
# build-only check impossible on a host that has not been provisioned
# for a full run, which is the one situation it is most useful in.
[ "$BUILD_ONLY" = "1" ] || ae_check_global_prepare

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
export CPU_NUM

echo "[AE] Output directory: $OUT_DIR"
echo "[AE] Guest CPUs: $CPU_NUM"
echo "[AE] Kernel parallel levels: 1 4 8 16 32 48 64 96"
echo "[AE] User threads: $USER_BENCH_THREADS"
# RUN_KERNEL_BENCH is normalized by bench_malloc_e2e.sh above.  Turning it off
# drops one full build per configuration (the CHCORE_KERNEL_TEST=ON one) plus
# the long boot each kernel session needs, which is the difference between a
# user-malloc-only rerun and an afternoon.
if [ "$RUN_KERNEL_BENCH" = "1" ]; then
    echo "[AE] Kernel tests: ON"
else
    echo "[AE] Kernel tests: OFF (RUN_KERNEL_BENCH=0; figure keeps only panel (c))"
fi

echo "[AE] Enabling user allocator benchmark in .config"
sed -i 's/^CHCORE_BUILD_USER_MALLOC_TESTS:BOOL=.*/CHCORE_BUILD_USER_MALLOC_TESTS:BOOL=ON/' \
    "$PROJECT_CONFIG"
grep -q '^CHCORE_BUILD_USER_MALLOC_TESTS:BOOL=ON$' "$PROJECT_CONFIG"

echo "[AE] Setting compile-time CPU maximum to $CPU_NUM"
ae_set_paper_guest_cpu_config "$CPU_NUM"
ae_export_guest_cpu_num "$CPU_NUM"

echo "[AE] Saving kernel/dsm_config.cmake"
save_config

run_configuration() {
    local label="$1" llfree="$2" crash_recovery="$3"
    echo "=== Configuring $label: DSM_CXL_LF_BUDDY=$llfree, SLAB_CRASH_RECOVERY=$crash_recovery ==="
    set_cmake_var DSM_CXL_LF_BUDDY "$llfree"
    set_cmake_var SLAB_CRASH_RECOVERY "$crash_recovery"

    if [ "$RUN_KERNEL_BENCH" = "1" ]; then
        echo "=== Building $label with CHCORE_KERNEL_TEST=ON ==="
        sed -i 's/^CHCORE_KERNEL_TEST:BOOL=.*/CHCORE_KERNEL_TEST:BOOL=ON/' "$PROJECT_CONFIG"
        grep -q '^CHCORE_KERNEL_TEST:BOOL=ON$' "$PROJECT_CONFIG"
        build_current_config "${label}_kernel"
        [ "$BUILD_ONLY" = "1" ] || run_kernel_benchmarks "$label"
    else
        echo "=== Skipping $label kernel build+tests (RUN_KERNEL_BENCH=0) ==="
    fi

    echo "=== Building $label with CHCORE_KERNEL_TEST=OFF for user malloc ==="
    sed -i 's/^CHCORE_KERNEL_TEST:BOOL=.*/CHCORE_KERNEL_TEST:BOOL=OFF/' "$PROJECT_CONFIG"
    grep -q '^CHCORE_KERNEL_TEST:BOOL=OFF$' "$PROJECT_CONFIG"
    build_current_config "${label}_user"
    [ "$BUILD_ONLY" = "1" ] || run_user_benchmarks "$label"
}

run_configuration llfree_cr_on ON ON
run_configuration llfree_cr_off ON OFF
run_configuration buddy_cr_off OFF OFF

echo "[AE] Restoring kernel/dsm_config.cmake"
restore_config

if [ "$BUILD_ONLY" = "1" ]; then
    echo "=== BUILD_ONLY=1: all three configurations built; skipping runs ==="
    echo "Artifact output: $OUT_DIR"
    exit 0
fi

echo "config,memory,test,parallel,run,ops_per_sec" > "$CSV_OUT"
echo "=== Parsing allocator logs ==="
for entry in "LLFree+CR:llfree_cr_on" "LLFree:llfree_cr_off" "Buddy:buddy_cr_off"; do
    config="${entry%%:*}"
    label="${entry##*:}"
    for run in $(seq 1 "$NRUNS"); do
        absolute_run=$((run + RUN_OFFSET))
        if [ "$RUN_KERNEL_BENCH" = "1" ]; then
            parse_kernel_log "$LOG_DIR/${label}_run${absolute_run}_kernel.log" \
                "$config" "$absolute_run" >> "$CSV_OUT"
        fi
        for threads in $USER_BENCH_THREADS; do
            parse_user_log "$LOG_DIR/${label}_run${absolute_run}_user_t${threads}.log" \
                "$config" "$absolute_run" >> "$CSV_OUT"
        done
    done
done

echo "=== Drawing allocator-all figure ==="
plot_args=(--csv "$CSV_FILE" --fig-dir "$FIG_DIR")
# USER_BENCH_THREADS is a supported scope control.  Thinning it cannot satisfy
# plot.py's full-dataset contract, so relax the completeness check for the
# points this run deliberately did not measure; parse and rendering failures
# still propagate.
if [ "$(echo $USER_BENCH_THREADS)" != "$PAPER_USER_BENCH_THREADS" ]; then
    echo "[AE] thinned USER_BENCH_THREADS; plotting only the available points."
    plot_args+=(--allow-partial)
elif [ "$RUN_KERNEL_BENCH" != "1" ]; then
    # No kernel rows at all, so the paper-completeness check would reject the
    # dataset.  Panels (a) and (b) come out empty; panel (c) is still real.
    echo "[AE] RUN_KERNEL_BENCH=0; kernel panels are empty in this figure."
    plot_args+=(--allow-partial)
fi
MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/matplotlib-$USER}" \
    python3 "$AE_DIR/plot.py" "${plot_args[@]}"
echo "Artifact output: $OUT_DIR"
