#!/bin/bash
# Sweep CXL_SAMPLE_SET_PAGES to locate the window length at which the Accessed
# bit still discriminates hot from cold.
#
# At the committed scan interval one pass over the window costs
# S / CXL_DEMOTE_MAX_BATCH rounds, i.e. S/640 seconds.  Runs 5-7 bracketed the
# transition between 0.32 s (77% referenced) and 6.4 s (95% referenced); this
# resolves the curve in between.
#
# Coverage does not depend on S: the scan interval fixes the observation rate,
# so pages enter observation at 640/CXL_SAMPLE_PASSES per second whatever the
# window size.  Only the per-page window length changes -- until one pass drops
# below CXL_CLOCK_EPOCH_NS, where the maturity check starts rejecting pages and
# scan_skips climbs.  Watch that counter at the small end.
#
# Usage: dsm-scripts/bench/run_cxl_sample_window_sweep.sh [S ...]

set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO" || exit 1

RECLAIM="kernel/dsm/dsm_cxl_reclaim.c"
CONFIG="kernel/dsm_config.cmake"
OUTDIR="log/cxl-aging-8m/sweep-window"
SIZES=("$@")
[ ${#SIZES[@]} -eq 0 ] && SIZES=(2048 1024 512 256 128)

# Snapshot what we are about to rewrite so an interrupt cannot leave the tree
# holding a sweep value.
ORIG_WINDOW="$(grep -oP '(?<=^#define CXL_SAMPLE_SET_PAGES )\d+' "$RECLAIM")"
ORIG_LIMIT="$(grep -oP '(?<=^set\(DSM_CXL_DEMOTE_LIMIT_MB ")\d+' "$CONFIG")"
if [ -z "$ORIG_WINDOW" ] || [ -z "$ORIG_LIMIT" ]; then
    echo "FATAL: could not read current CXL_SAMPLE_SET_PAGES / DEMOTE_LIMIT_MB" >&2
    exit 1
fi
echo "[sweep] restoring on exit: window=$ORIG_WINDOW limit=${ORIG_LIMIT}MB"

restore() {
    sed -i "s/^#define CXL_SAMPLE_SET_PAGES .*/#define CXL_SAMPLE_SET_PAGES $ORIG_WINDOW/" "$RECLAIM"
    sed -i "s/^set(DSM_CXL_DEMOTE_LIMIT_MB \".*\")/set(DSM_CXL_DEMOTE_LIMIT_MB \"$ORIG_LIMIT\")/" "$CONFIG"
    echo "[sweep] restored window=$ORIG_WINDOW limit=${ORIG_LIMIT}MB"
}
cleanup_run() {
    tmux kill-session -t "$USER-qemu" 2>/dev/null
    sleep 3
    local left
    left="$(ps -eo pid,cmd | grep -c '[q]emu-6.2-system')"
    [ "$left" != "0" ] && echo "[sweep] WARNING: $left QEMU process(es) still alive"
    return 0
}
trap 'echo "[sweep] interrupted"; cleanup_run; restore; exit 130' INT TERM

# The pressure knob stays at the value runs 3-7 used, so every point in this
# sweep is comparable to them and the only variable is the window.
sed -i "s/^set(DSM_CXL_DEMOTE_LIMIT_MB \".*\")/set(DSM_CXL_DEMOTE_LIMIT_MB \"64\")/" "$CONFIG"

mkdir -p "$OUTDIR"
for S in "${SIZES[@]}"; do
    echo "=============================================================="
    echo "[sweep] CXL_SAMPLE_SET_PAGES=$S  (window $(python3 -c "print(f'{$S/640:.2f}')") s)"
    echo "=============================================================="
    sed -i "s/^#define CXL_SAMPLE_SET_PAGES .*/#define CXL_SAMPLE_SET_PAGES $S/" "$RECLAIM"

    dest="$OUTDIR/S${S}"
    mkdir -p "$dest"
    BENCHS=dbx1000 CONFIGS=Kernel_Page_CXL_Other_DRAM MACHINE_COUNTS=8 \
        DBX_CROSS_WAREHOUSE_RATIO_MODE=true DBX_CROSS_WAREHOUSE_PCT=15 \
        DBX_MEASURE_DURATION_SEC=1200 DBX_TIMEOUT=2400 TIMEOUT=2400 \
        ./artifact-evaluation/4-state-partition/run.sh > "$dest/run.out" 2>&1
    echo "[sweep] S=$S run.sh exit=$?"

    cp logs/exec_log*.log "$dest/" 2>/dev/null
    latest="$(ls -1dt artifact-evaluation/4-state-partition/out/*/ 2>/dev/null | head -1)"
    [ -n "$latest" ] && cp "$latest"logs/dbx1000_*.log "$dest/" 2>/dev/null

    echo "[sweep] S=$S result: $(grep -ah '^thp=' "$dest"/dbx1000_*.log 2>/dev/null | tail -1)"
    echo "[sweep] S=$S clock:  $(grep -ah 'CXL_CLOCK' "$dest"/exec_log*.log 2>/dev/null | tail -1)"
    cleanup_run
done

restore
echo "[sweep] done; results under $OUTDIR"
