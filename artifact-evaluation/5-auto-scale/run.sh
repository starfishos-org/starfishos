#!/usr/bin/env bash
#
# Artifact script for the paper auto-scale figures:
#   auto-scale-matrix.png  (Matrix-multiply MapReduce)
#   db1000.png             (DBx1000 TPC-C)
#   gemini-chcore.png      (GeminiGraph PageRank)
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
#   CONFIGS="Mixed CXL"           TIMEOUT=1200
#   RUN_BASELINES=1                BASELINE_STAGES=linux,matrix-tcp,tigon
#   TIGON_DIR=/path/to/tigon       default: AE ../deps/tigon submodule
#   TIGON_SETUP=1                  set 0 to reuse already-running Tigon VMs
#   TIGON_IMAGE_ATTEMPTS=3         retry transient mkosi download failures
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/5-auto-scale"
ae_init_output_dirs "$AE_DIR"
AE_LOG_DIR="$LOG_DIR"
TIMEOUT="${TIMEOUT:-1200}"
DBX_WAREHOUSES_PER_MACHINE="${DBX_WAREHOUSES_PER_MACHINE:-8}"
DBX_WARMUP_PER_MACHINE="${DBX_WARMUP_PER_MACHINE:-880000}"

APPS="${APPS-matrix db1000 gemini}"
MACHINES="${MACHINES-1 2 4 6 8}"
CONFIGS="${CONFIGS-Mixed CXL}"
RUN_BASELINES="${RUN_BASELINES-1}"
BASELINE_STAGES="${BASELINE_STAGES-linux,matrix-tcp,tigon}"
TIGON_SETUP="${TIGON_SETUP-1}"

# Validate every scope selector before taking the global runner lock or
# changing host/repository state.  A reordered or whitespace-normalized full
# set remains a full request; duplicates and unknown values are rejected.
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
        echo "[AE] $label must contain at least one value" >&2
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

validate_scope_list APPS "$APPS" matrix db1000 gemini || exit 1
validate_scope_list MACHINES "$MACHINES" 1 2 4 6 8 || exit 1
validate_scope_list CONFIGS "$CONFIGS" Mixed CXL || exit 1

validate_baseline_stages() {
    local raw="$1" baseline_stage baseline_stage_count=0
    local -a requested_baseline_stages=()

    case "$raw" in
        *$'\n'*|*$'\r'*)
            echo "[AE] BASELINE_STAGES must be a single comma-separated line" >&2
            return 1
            ;;
    esac
    IFS=, read -r -a requested_baseline_stages <<< "$raw"
    baseline_stage_list=","
    for baseline_stage in "${requested_baseline_stages[@]}"; do
        # Match run_baselines.py's per-token strip() without accepting spaces
        # inside a stage name.
        baseline_stage="${baseline_stage#"${baseline_stage%%[![:space:]]*}"}"
        baseline_stage="${baseline_stage%"${baseline_stage##*[![:space:]]}"}"
        case "$baseline_stage" in
            "") continue ;;
            linux|matrix-tcp|tigon) ;;
            *)
                echo "[AE] unknown baseline stage: $baseline_stage" >&2
                return 1
                ;;
        esac
        if [[ "$baseline_stage_list" == *,"$baseline_stage",* ]]; then
            echo "[AE] duplicate baseline stage: $baseline_stage" >&2
            return 1
        fi
        baseline_stage_list+="${baseline_stage},"
        baseline_stage_count=$((baseline_stage_count + 1))
    done
    if [ "$baseline_stage_count" -eq 0 ]; then
        echo "[AE] BASELINE_STAGES must contain at least one stage" >&2
        return 1
    fi
    if [ "$baseline_stage_count" -ne 3 ]; then
        FULL_PLOT_REQUEST=0
    fi
}

case "$RUN_BASELINES" in
    0|1) ;;
    *) echo "[AE] RUN_BASELINES must be 0 or 1" >&2; exit 1 ;;
esac
case "$TIGON_SETUP" in
    0|1) ;;
    *) echo "[AE] TIGON_SETUP must be 0 or 1" >&2; exit 1 ;;
esac
baseline_stage_list=","
if [ "$RUN_BASELINES" = "1" ]; then
    validate_baseline_stages "$BASELINE_STAGES" || exit 1
else
    FULL_PLOT_REQUEST=0
fi

ae_acquire_run_lock "auto-scale" || exit 1

mkdir -p "$AE_LOG_DIR" "$CSV_DIR" "$FIG_DIR"
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
        matrix) echo "matrix_multiply.bin -l 4000 -r 4000 -t $((8 * n)) -c 0 &" ;;
        db1000) echo "rundb.bin -t$((8 * n)) &" ;;
        gemini) echo "pagerank /host/twitter-2010.bin 41652230 50 $n &" ;;
        *) echo "Unknown app: $1" >&2; return 1 ;;
    esac
}

# app -> completion marker printed once the workload has reported its result.
app_marker() {
    case "$1" in
        # matrix_multiply prints library: / finalize:, not "inter library:"
        matrix) echo "finalize:" ;;
        db1000) echo "thp=" ;;
        # Wait for the final checksum record, not just the preceding timing.
        # The escaped '[' is a grep basic-regex literal.
        gemini) echo 'pr\[' ;;
    esac
}

# Matrix finalization, DBx1000 table initialization, and GeminiGraph graph
# loading/processing may spend several minutes without writing to the serial
# console.  That is normal progress, so those workload phases use the
# experiment timeout instead of the generic frozen-boot-log heuristic.
# ae_wait_in_log still checks guest fatal errors and every tmux pane while the
# serial-stall timer is disabled.
wait_for_app() {
    local app="$1" marker="$2" label="$3" machines="$4"
    case "$app" in
        matrix|db1000|gemini)
            AE_LOG_STALL_S=0 ae_wait_in_log \
                0 "$marker" "$TIMEOUT" "$label" "$machines"
            ;;
        *)
            ae_wait_in_log 0 "$marker" "$TIMEOUT" "$label" "$machines"
            ;;
    esac
}

ae_ensure_clean_tmux
ae_check_global_prepare || exit 1
ae_save_build_configs
cleanup() {
    local rc=$? cleanup_failed=0
    trap - EXIT
    ae_kill_cluster || cleanup_failed=1
    if [ -d "$TMP_DIR" ]; then
        if cp "$TMP_DIR/dbx1000-config.h" "$DBX_CONFIG"; then
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

# Auto-scale consumes Phoenix's userspace timing but not the optional kernel
# set_affinity-to-dequeue probe.  Its cross-machine TSC barrier deliberately
# releases every guest's late boot at once, which defeats the serialized shell
# boot used below and can strand a secondary QEMU during SMP initialization.
ae_set_dsm_var PHOENIX_SCHED_TIMING OFF

worker_bind_cpu_list() {
    local n="$1" machine cpu values=()
    for machine in $(seq 0 $((n - 1))); do
        for cpu in $(seq 0 7); do
            values+=("$((machine * 12 + cpu))")
        done
    done
    (IFS=,; echo "${values[*]}")
}

set_dbx_define() {
    local name="$1" value="$2"
    sed -i "s/^#define ${name}[[:space:]].*/#define ${name}\t\t\t\t${value}/" "$DBX_CONFIG"
    grep -qE "^#define ${name}[[:space:]]+${value}([[:space:]]|$)" "$DBX_CONFIG" || {
        echo "Failed to set DBx1000 ${name}=${value}" >&2
        return 1
    }
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
            # config.h is the 8-machine point: 64 warehouses and 7,040,000
            # warmup transactions.  Scale both with N so every machine keeps
            # the same 8 warehouses and 880,000 warmup transactions.  Leaving
            # the 8-machine totals in place for N=1/2 exhausts their 16-GiB
            # local pools while prebuilding the TPC-C tables/query arrays.
            set_dbx_define NUM_MACHINES "$n"
            set_dbx_define NUM_WH "$((DBX_WAREHOUSES_PER_MACHINE * n))"
            set_dbx_define WARMUP "$((DBX_WARMUP_PER_MACHINE * n))"
            set_dbx_define PERC_REMOTE_PAYMENT 15
            set_dbx_define PERC_REMOTE_NEW_ORDER 15
            ae_build || { echo "[AE] build failed for $app/$config/N=$n" >&2; return 1; }
        fi
        # PHOENIX_SCHED_TIMING is disabled above, so waiting for each shell is
        # safe and prevents four or more QEMUs from entering late kernel/SMP
        # initialization at once.  The concurrent path strands machine 3.
        if ! AE_WAIT_SHELL_PER_MACHINE=1 ae_boot_cluster "$n"; then
            echo "[AE] boot failed for $app/$config/N=$n" >&2
            ae_archive_logs "$n" "$AE_LOG_DIR" \
                "-boot-failed-${app}-${config}-N${n}"
            ae_kill_cluster
            return 1
        fi
        if [ "$app" = "matrix" ]; then
            # Phoenix's -t value is the cluster-wide worker count.  Give each
            # machine eight workers and keep the bind file scoped to this N.
            ae_send_command 0 "write matrix_multiply_bind_cpu.txt $(worker_bind_cpu_list "$n")"
        elif [ "$app" = "db1000" ]; then
            ae_send_command 0 "write dbx1000_bind_cpu.txt $(worker_bind_cpu_list "$n")"
        elif [ "$app" = "gemini" ]; then
            # Keep Gemini's global CPU namespace consistent with the cluster
            # booted for this point.  The ramdisk default contains four
            # machine segments and is therefore unsafe for N=1.
            ae_send_command 0 "write gemini_bind_cpu.txt $(worker_bind_cpu_list "$n")"
        fi
        cmd="$(app_cmd "$app" "$n")"
        # Drive the workload on machine 0; the unmodified shared-memory app
        # creates workers across the N-machine global CPU namespace.
        ae_send_command 0 "$cmd"
        if wait_for_app "$app" "$marker" "$app/$config N=$n" "$n"; then
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
    tigon_pending=0
    if [ "$TIGON_SETUP" = "1" ] && [[ "$baseline_stage_list" == *,tigon,* ]]; then
        for n in 1 2 4 6 8; do
            tigon_log="$AE_LOG_DIR/db1000_Tigon_N${n}.log"
            if ! grep -qE "^AE_RESULT app=db1000 series=Tigon n=${n} value=[-+0-9.eE]+ unit=mops$" \
                "$tigon_log" 2>/dev/null; then
                tigon_pending=1
                break
            fi
        done
    fi
    if [ "$tigon_pending" = "1" ]; then
        # The StarfishOS sweep uses about 191 GiB of tmpfs-backed memory.  Tigon
        # subsequently preallocates eight 10-GiB guests on node 0; retaining
        # the now-unused StarfishOS files can starve the final VM.  The helper
        # refuses any file still mapped by a process and removes only this
        # user's exact StarfishOS resource names.
        ae_release_memdev_backing || exit 1
    fi
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
    plot_args=(--csv-dir "$CSV_DIR" --fig-dir "$FIG_DIR" --log-dir "$AE_LOG_DIR")
    required_plot_logs=()
    for app in $APPS; do
        for config in $CONFIGS; do
            for n in $MACHINES; do
                required_plot_logs+=("${app}_${config}_N${n}.log")
            done
        done
    done
    if [ "$RUN_BASELINES" = "1" ]; then
        for n in 1 2 4 6 8; do
            if [[ "$baseline_stage_list" == *,linux,* ]]; then
                required_plot_logs+=(
                    "matrix_Ideal_N${n}.log"
                    "db1000_Ideal_N${n}.log"
                    "gemini_Ideal_N${n}.log"
                    "gemini_Distributed_N${n}.log"
                )
            fi
            if [[ "$baseline_stage_list" == *,matrix-tcp,* ]]; then
                required_plot_logs+=("matrix_Distributed_N${n}.log")
            fi
            if [[ "$baseline_stage_list" == *,tigon,* ]]; then
                required_plot_logs+=("db1000_Tigon_N${n}.log")
            fi
        done
    fi
    for required_plot_log in "${required_plot_logs[@]}"; do
        plot_args+=(--require-log "$required_plot_log")
    done
    if [ "$FULL_PLOT_REQUEST" != "1" ]; then
        # APPS/MACHINES/CONFIGS are documented subset controls.  Such a run is
        # successful when every requested measurement succeeds, even though it
        # cannot satisfy the paper figure's full-dataset contract.  Partial
        # mode relaxes only completeness checks; parse and rendering failures
        # still propagate from plot.py.
        echo "[AE] subset run requested; plotting only the available points."
        plot_args+=(--allow-partial)
    fi
    python3 "$AE_DIR/plot.py" "${plot_args[@]}"
else
    echo "[AE] No sweep logs under $AE_LOG_DIR; nothing to plot." >&2
    echo "[AE] To verify plotters against paper data:"
    echo "[AE]   python3 $AE_DIR/plot.py --csv-dir $CSV_DIR --fig-dir $FIG_DIR \\"
    echo "[AE]     --matrix-data <path>/4000size.txt \\"
    echo "[AE]     --db1000-data <path>/db1000-p3os-tigon.csv \\"
    echo "[AE]     --gemini-data <path>/data.log"
fi

echo ""
echo "auto-scale figures target: $FIG_DIR/{auto-scale-matrix,db1000,gemini-chcore}.png + auto-scale-legend.png"
echo "Artifact output: $OUT_DIR"
ae_finish
