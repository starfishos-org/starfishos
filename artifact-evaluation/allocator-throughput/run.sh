#!/usr/bin/env bash
#
# Artifact script for paper Figure 12: "Memory allocator throughput".
#
# For each allocator configuration (Buddy / LLFree / LLFree+CR) this script:
#   1. Edits kernel/dsm_config.cmake + .config and rebuilds ChCore.
#   2. Boots one QEMU machine with CHCORE_KERNEL_TEST=ON: the kernel-side
#      malloc benchmarks (kmalloc, get_pages, mixed random 4K/2M) run at boot
#      over parallel levels {1,4,8,16,32,48,64,96}.
#   3. Rebuilds with CHCORE_KERNEL_TEST=OFF and runs the userspace rpmalloc
#      benchmark (malloc_benchmark.bin), one fresh QEMU boot per thread count.
#   4. Parses all logs and plots the combined 3-panel figure.
#
# Usage (from repo root):
#   ./artifact-evaluation/prepare.sh          # once
#   ./artifact-evaluation/allocator-throughput/run.sh
#
# Env overrides:
#   NRUNS=1                       repeats per config
#   USER_BENCH_THREADS="1 2 4 8 16 32 64 96"
#   CPU_NUM=96                    QEMU cores (kernel parallel levels above this are skipped)
#   CONFIGS="buddy llfree llfree_cr"
#   TIMEOUT=900                   per-QEMU-session timeout (s)
#   OUT_DIR                       output dir (default out/<timestamp>)
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/allocator-throughput"
TS="${TS:-$(date +%Y%m%d_%H%M%S)}"
OUT_DIR="${OUT_DIR:-$AE_DIR/out/$TS}"
AE_LOG_DIR="$OUT_DIR/logs"
NRUNS="${NRUNS:-1}"
CPU_NUM="${CPU_NUM:-96}"
USER_BENCH_THREADS="${USER_BENCH_THREADS:-1 2 4 8 16 32 64 96}"
USER_BENCH_ARGS="0 0 0 10 100 1000 16 256"
CONFIGS="${CONFIGS:-buddy llfree llfree_cr}"
TIMEOUT="${TIMEOUT:-900}"

mkdir -p "$AE_LOG_DIR" "$OUT_DIR/results" "$OUT_DIR/figures"

# config name -> DSM_CXL_LF_BUDDY  SLAB_CRASH_RECOVERY  DSM_USER_MALLOC_MODE
config_params() {
    case "$1" in
        buddy)     echo "OFF OFF DEFAULT_CXL" ;;
        llfree)    echo "ON  OFF DEFAULT_CXL" ;;
        llfree_cr) echo "ON  ON  DEFAULT_DRAM" ;;
        *) echo "Unknown config: $1" >&2; return 1 ;;
    esac
}

cleanup() {
    ae_kill_cluster
    ae_restore_build_configs
}
trap cleanup EXIT

cd "$AE_REPO_ROOT"
ae_check_global_prepare
ae_save_build_configs

# chcore.ini's cpu_num feeds both the kernel's compile-time PLAT_CPU_NUM (via
# chbuild -D, which overrides .config) and simulate.sh's default -smp.
ae_set_ini_cpu_num "$CPU_NUM"

for cfg in $CONFIGS; do
    read -r lf_buddy slab_cr user_malloc <<< "$(config_params "$cfg")"
    echo ""
    echo "########################################################"
    echo "### Config: $cfg (LF_BUDDY=$lf_buddy CR=$slab_cr USER_MALLOC=$user_malloc)"
    echo "########################################################"

    ae_set_dsm_var DSM_CXL_LF_BUDDY "$lf_buddy"
    ae_set_dsm_var SLAB_CRASH_RECOVERY "$slab_cr"
    ae_set_dsm_var DSM_USER_MALLOC_MODE "$user_malloc"
    ae_set_dotconfig CHCORE_PLAT_CPU_NUM STRING "$CPU_NUM"

    # ── kernel-side tests (run automatically at boot) ──
    ae_set_dotconfig CHCORE_KERNEL_TEST BOOL ON
    ae_build

    for run in $(seq 1 "$NRUNS"); do
        echo "=== [$cfg run $run] kernel malloc tests ==="
        ae_boot_cluster 1 "$CPU_NUM"
        if ! ae_wait_in_log 0 "kernel tests done" "$TIMEOUT" "kernel tests done"; then
            echo "[WARN] kernel tests for $cfg timed out; partial log saved" >&2
        fi
        cp "$(ae_machine_log 0)" "$AE_LOG_DIR/${cfg}_run${run}_kernel.log" 2>/dev/null || true
        ae_kill_cluster
    done

    # ── userspace rpmalloc benchmark ──
    ae_set_dotconfig CHCORE_KERNEL_TEST BOOL OFF
    ae_build

    for run in $(seq 1 "$NRUNS"); do
        for threads in $USER_BENCH_THREADS; do
            echo "=== [$cfg run $run] user bench threads=$threads ==="
            ae_boot_cluster 1 "$CPU_NUM"
            ae_send_command 0 "malloc_benchmark.bin $threads $USER_BENCH_ARGS"
            if ae_wait_in_log 0 "throughput(ops/s):" "$TIMEOUT" "user bench done (t=$threads)"; then
                cp "$(ae_machine_log 0)" "$AE_LOG_DIR/${cfg}_run${run}_user_t${threads}.log"
            else
                cp "$(ae_machine_log 0)" "$AE_LOG_DIR/${cfg}_run${run}_user_t${threads}.log" || true
                echo "user bench t=$threads timed out; log saved, stopping sweep for this run" >&2
                ae_kill_cluster
                break
            fi
            ae_kill_cluster
        done
    done
done

ae_restore_build_configs

echo ""
echo "=== Parsing logs and generating figure ==="
python3 "$AE_DIR/parse_and_plot.py" --log-dir "$AE_LOG_DIR" --out-dir "$OUT_DIR"

echo "Artifact output: $OUT_DIR"
ae_finish
