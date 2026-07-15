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
# The same entry point also collects every external baseline:
#   Ideal        = Linux DRAM        -> test-on-linux/{phoenix,dbx1000,GeminiGraph}
#   Distributed  = Linux-MPI         -> test-on-linux/ggraph-distri
#   Tigon        = pinned Tigon checkout and its VM CXL-pod emulator
# See run_baselines.py and test-on-linux/README.md.
#
# Usage (from repo root):
#   ./artifact-evaluation/prepare.sh          # once
#   ./artifact-evaluation/5-auto-scale/run.sh
#
# Env overrides:
#   APPS="matrix db1000 gemini"   MACHINES="1 2 4 6 8"
#   CONFIGS="Mixed CXL"           TIMEOUT=1200   OUT_DIR=out/<timestamp>
#   RUN_BASELINES=1                BASELINE_STAGES=linux,matrix-tcp,tigon
#   TIGON_DIR=/path/to/tigon       default: AE ../deps/tigon submodule
#   TIGON_SETUP=1                  set 0 to reuse already-running Tigon VMs
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
RUN_BASELINES="${RUN_BASELINES:-1}"
BASELINE_STAGES="${BASELINE_STAGES:-linux,matrix-tcp,tigon}"
TIGON_SETUP="${TIGON_SETUP:-1}"

mkdir -p "$AE_LOG_DIR" "$RESULTS" "$OUT_DIR/figures"
DBX_CONFIG="$AE_REPO_ROOT/user/demos/dbx1000/config.h"
TMP_DIR="$(mktemp -d)"
cp "$DBX_CONFIG" "$TMP_DIR/dbx1000-config.h"

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
    local app="$1" n="$2"
    case "$app" in
        matrix) echo "matrix_multiply.bin -l 4000 -r 4000 -t 8 -c 0 &" ;;
        db1000) echo "rundb.bin -t$((8 * n)) &" ;;
        gemini) echo "pagerank /host/twitter-2010.bin 41652230 50 2 &" ;;
        *) echo "Unknown app: $1" >&2; return 1 ;;
    esac
}

# app -> completion marker printed once the workload has reported its result.
app_marker() {
    case "$1" in
        # matrix_multiply prints library: / finalize:, not "inter library:"
        matrix) echo "finalize:" ;;
        db1000) echo "thp=" ;;
        gemini) echo "exec_time=" ;;
    esac
}

ae_ensure_clean_tmux
ae_check_global_prepare || exit 1
ae_save_build_configs
cleanup() {
    cp "$TMP_DIR/dbx1000-config.h" "$DBX_CONFIG"
    rm -rf "$TMP_DIR"
    ae_restore_build_configs
    ae_kill_cluster
}
trap cleanup EXIT

dbx_bind_cpu_list() {
    local n="$1" machine cpu values=()
    for machine in $(seq 0 $((n - 1))); do
        for cpu in $(seq 0 7); do
            values+=("$((machine * 12 + cpu))")
        done
    done
    (IFS=,; echo "${values[*]}")
}

sweep_app_config() {
    local app="$1" config="$2"
    local params malloc user_malloc cmd marker
    params="$(config_params "$config")"
    malloc="${params%% *}"; user_malloc="${params##* }"
    marker="$(app_marker "$app")"

    echo ""
    echo "############################################################"
    echo "### auto-scale: $app / $config ($malloc, $user_malloc)"
    echo "############################################################"
    ae_set_dsm_var DSM_MALLOC_MODE "$malloc"
    ae_set_dsm_var DSM_USER_MALLOC_MODE "$user_malloc"
    if [ "$app" != "db1000" ]; then
        ae_build || { echo "[AE] build failed for $app/$config" >&2; return 1; }
    fi

    for n in $MACHINES; do
        echo "### $app/$config: $n machine(s)"
        if [ "$app" = "db1000" ]; then
            sed -i "s/^#define NUM_MACHINES[[:space:]].*/#define NUM_MACHINES\t\t\t\t$n/" "$DBX_CONFIG"
            sed -i 's/^#define PERC_REMOTE_PAYMENT[[:space:]].*/#define PERC_REMOTE_PAYMENT\t\t15/' "$DBX_CONFIG"
            sed -i 's/^#define PERC_REMOTE_NEW_ORDER[[:space:]].*/#define PERC_REMOTE_NEW_ORDER\t\t15/' "$DBX_CONFIG"
            ae_build || { echo "[AE] build failed for $app/$config/N=$n" >&2; return 1; }
        fi
        ae_boot_cluster "$n" || { ae_kill_cluster; return 1; }
        if [ "$app" = "db1000" ]; then
            ae_send_command 0 "write dbx1000_bind_cpu.txt $(dbx_bind_cpu_list "$n")"
        fi
        cmd="$(app_cmd "$app" "$n")"
        # Drive the workload on machine 0; the unmodified shared-memory app
        # creates workers across the N-machine global CPU namespace.
        ae_send_command 0 "$cmd"
        if ae_wait_in_log 0 "$marker" "$TIMEOUT" "$app/$config N=$n"; then
            cp "$(ae_machine_log 0)" "$AE_LOG_DIR/${app}_${config}_N${n}.log"
        else
            cp "$(ae_machine_log 0)" "$AE_LOG_DIR/${app}_${config}_N${n}.log" || true
            ae_kill_cluster
            return 1
        fi
        ae_kill_cluster
    done
}

for app in $APPS; do
    for config in $CONFIGS; do
        sweep_app_config "$app" "$config"
    done
done

if [ "$RUN_BASELINES" = "1" ]; then
    echo ""
    echo "=== Collecting Linux / TCP / Tigon baselines ==="
    baseline_args=(
        --log-dir "$AE_LOG_DIR"
        --stages "$BASELINE_STAGES"
        --graph "${GRAPH_DATASET:-$AE_REPO_ROOT/datasets/twitter-2010.bin}"
        --tigon-dir "${TIGON_DIR:-$AE_REPO_ROOT/artifact-evaluation/deps/tigon}"
    )
    if [ "$TIGON_SETUP" = "0" ]; then
        baseline_args+=(--skip-tigon-setup)
    fi
    python3 "$AE_DIR/run_baselines.py" "${baseline_args[@]}"
fi

# Convert archived sweep logs into the data files plot.py consumes, then draw.
echo ""
echo "=== Collecting results + plotting (from $AE_LOG_DIR) ==="
if ls "$AE_LOG_DIR"/*_N*.log >/dev/null 2>&1; then
    python3 "$AE_DIR/plot.py" --out-dir "$OUT_DIR" --log-dir "$AE_LOG_DIR"
else
    echo "[AE] No sweep logs under $AE_LOG_DIR; nothing to plot." >&2
    echo "[AE] To verify plotters against paper data:"
    echo "[AE]   python3 $AE_DIR/plot.py --out-dir $OUT_DIR \\"
    echo "[AE]     --matrix-data <path>/4000size.txt \\"
    echo "[AE]     --db1000-data <path>/db1000-p3os-tigon.csv \\"
    echo "[AE]     --gemini-data <path>/data.log"
fi

echo ""
echo "auto-scale figures target: $OUT_DIR/figures/{auto-scale-matrix,db1000,gemini-chcore}.{eps,pdf,png} + auto-scale-legend.{eps,pdf}"
ae_finish
