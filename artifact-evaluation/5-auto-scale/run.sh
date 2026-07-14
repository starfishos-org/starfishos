#!/usr/bin/env bash
#
# Artifact script for the paper auto-scale figures:
#   auto-scale-matrix.eps  (Matrix-multiply MapReduce)
#   db1000.eps             (DBx1000 TPC-C)
#   gemini-chcore.eps      (GeminiGraph PageRank)
#
# For each application it sweeps the machine count (1,2,4,6,8) under the two
# StarfishOS placements — Mixed (MIXED_DEFAULT_CXL) and CXL (all-CXL) — booting
# an N-machine cluster per point and recording the run time / throughput.  The
# StarfishOS curves are what this script produces.
#
# The paper's other curves are EXTERNAL baselines that this script does NOT run:
#   Ideal        = Linux DRAM        -> test-on-linux/{phoenix,dbx1000,GeminiGraph}
#   Distributed  = Linux-MPI         -> test-on-linux/ggraph-distri
#   Tigon        = external DB system -> (db1000 only; not vendored here)
# See test-on-linux/README.md. Their numbers are merged into the data files
# below (auto-scale-matrix expects TCP=Distributed + IDEAL rows; db1000 expects
# tigon + linux rows; gemini expects LINUX-DRAM + DISTRIBUTED columns).
#
# ###########################################################################
# ## CAVEAT — NOT YET VALIDATED AGAINST A LIVE SWEEP.
# ## The Mixed/CXL config mapping, the per-app completion markers, and the
# ## result-line extraction below are derived from dsm_config.cmake and the
# ## existing test scripts, not from an executed multi-machine sweep. Validate
# ## before trusting the numbers. The PLOTTING path IS validated: it reproduces
# ## all three figures from the paper's data files (see README / parse_and_plot).
# ## External baselines must be produced separately and merged in.
# ###########################################################################
#
# Usage (from repo root):
#   ./artifact-evaluation/prepare.sh          # once
#   ./artifact-evaluation/6-auto-scale/run.sh
#
# Env overrides:
#   APPS="matrix db1000 gemini"   MACHINES="1 2 4 6 8"
#   CONFIGS="Mixed CXL"           TIMEOUT=1200   OUT_DIR=out/<timestamp>
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/5-auto-scale"
TS="${TS:-$(date +%Y%m%d_%H%M%S)}"
OUT_DIR="${OUT_DIR:-$AE_DIR/out/$TS}"
AE_LOG_DIR="$OUT_DIR/logs"
RESULTS="$OUT_DIR/results"
TIMEOUT="${TIMEOUT:-1200}"

APPS="${APPS:-matrix db1000 gemini}"
MACHINES="${MACHINES:-1 2 4 6 8}"
CONFIGS="${CONFIGS:-Mixed CXL}"

mkdir -p "$AE_LOG_DIR" "$RESULTS" "$OUT_DIR/figures"

# config -> "DSM_MALLOC_MODE DSM_USER_MALLOC_MODE" (per ae-figure-mapping).
config_params() {
    case "$1" in
        Mixed) echo "MIXED_DEFAULT_CXL DEFAULT_CXL" ;;
        CXL)   echo "CXL DEFAULT_CXL" ;;
        *) echo "Unknown config: $1" >&2; return 1 ;;
    esac
}

# app -> "<command template>" (uses %N% placeholder for #machines if needed).
app_cmd() {
    case "$1" in
        matrix) echo "matrix_multiply.bin -l 4000 -r 4000 -t 8 -c 0 &" ;;
        db1000) echo "rundb.bin -t8 -r1 -w0 -z0.6 &" ;;
        gemini) echo "pagerank /host/twitter-2010.bin 41652230 50 2 &" ;;
        *) echo "Unknown app: $1" >&2; return 1 ;;
    esac
}

# app -> completion marker printed once the workload has reported its result.
app_marker() {
    case "$1" in
        matrix) echo "inter library:" ;;
        db1000) echo "thp=" ;;
        gemini) echo "exec_time" ;;
    esac
}

ae_check_global_prepare || exit 1
ae_save_build_configs
trap 'ae_restore_build_configs; ae_kill_cluster' EXIT

sweep_app_config() {
    local app="$1" config="$2"
    local params malloc user_malloc cmd marker
    params="$(config_params "$config")"
    malloc="${params%% *}"; user_malloc="${params##* }"
    cmd="$(app_cmd "$app")"
    marker="$(app_marker "$app")"

    echo ""
    echo "############################################################"
    echo "### auto-scale: $app / $config ($malloc, $user_malloc)"
    echo "############################################################"
    ae_set_dsm_var DSM_MALLOC_MODE "$malloc"
    ae_set_dsm_var DSM_USER_MALLOC_MODE "$user_malloc"
    ae_build || { echo "[AE] build failed for $app/$config" >&2; return 1; }

    for n in $MACHINES; do
        echo "### $app/$config: $n machine(s)"
        ae_boot_cluster "$n" || { ae_kill_cluster; continue; }
        # Drive the workload on machine 0 (each app's own runner reduces across
        # the N-machine shared queue). TODO: confirm per-app multi-machine
        # invocation + that the result line carries the aggregate time.
        ae_send_command 0 "$cmd"
        ae_wait_in_log 0 "$marker" "$TIMEOUT" "$app/$config N=$n" \
            && cp "$(ae_machine_log 0)" "$AE_LOG_DIR/${app}_${config}_N${n}.log"
        ae_kill_cluster
    done
}

for app in $APPS; do
    for config in $CONFIGS; do
        sweep_app_config "$app" "$config" || true
    done
done

# NOTE: converting the per-run logs into the data-file formats that
# plot.py consumes (matrix RESULT: lines, db1000 CSV, gemini CSV) and
# merging the external baselines is left as a validated-run step. Once the data
# files exist under $RESULTS, draw with:
echo ""
echo "=== Plotting (from data files under $RESULTS, if present) ==="
plot_args=()
[ -f "$RESULTS/4000size.txt" ] && plot_args+=(--matrix-data "$RESULTS/4000size.txt")
[ -f "$RESULTS/db1000-p3os-tigon.csv" ] && plot_args+=(--db1000-data "$RESULTS/db1000-p3os-tigon.csv")
[ -f "$RESULTS/gemini-data.log" ] && plot_args+=(--gemini-data "$RESULTS/gemini-data.log")
if [ "${#plot_args[@]}" -gt 0 ]; then
    python3 "$AE_DIR/plot.py" --out-dir "$OUT_DIR" "${plot_args[@]}"
else
    echo "[AE] No data files under $RESULTS yet. To verify the plotters against"
    echo "[AE] the paper data:"
    echo "[AE]   python3 $AE_DIR/plot.py --out-dir $OUT_DIR \\"
    echo "[AE]     --matrix-data $AE_REPO_ROOT/../p3os-paper/eval/mapreduce/4000size.txt \\"
    echo "[AE]     --db1000-data $AE_REPO_ROOT/../p3os-paper/eval/db1000/db1000-p3os-tigon.csv \\"
    echo "[AE]     --gemini-data $AE_REPO_ROOT/../p3os-paper/eval/gemini_graph/data.log"
fi

echo ""
echo "auto-scale figures target: $OUT_DIR/figures/{auto-scale-matrix,db1000,gemini-chcore}.{eps,pdf,png}"
ae_finish
