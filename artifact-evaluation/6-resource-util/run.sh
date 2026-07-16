#!/usr/bin/env bash
#
# Artifact script for paper Figure 15 (real): 12 applications, each
# measured under three conditions and normalized to its own single-run.
#
#   single  — the application alone on one machine (baseline)
#   stress  — co-located with a competing workload, traditional DRAM placement
#   p3os    — co-located under StarfishOS (the competitor is offloaded to a
#             second machine over CXL/DSM)
#
# The co-location pairs are the ones already encoded in
#   user/script/single_stress_type{1..6}.sh   (traditional, one machine)
#   user/script/cross_stress_type{1..6}_m{0,1}.sh (StarfishOS, two machines)
# and driven manually by dsm-scripts/tests/real/{dram,cxl}.sh.  This script
# wraps that flow in the AE harness, captures one <bench>_<cond>.log per
# (application, condition), then parses + plots.
#
# Usage (from repo root):
#   ./artifact-evaluation/prepare.sh          # once
#   ./artifact-evaluation/6-resource-util/run.sh
#
# The runner enables and builds all paper workloads itself, including Redis,
# Memcached/Memcachetest, and the restored TinyCNN submodule.
#
# ###########################################################################
# ## CAVEAT — NOT YET VALIDATED AGAINST A LIVE RUN.
# ## The (application -> stress type) demux and the per-application completion
# ## markers below are derived from the stress scripts and config.exp, not from
# ## an executed co-location run.  Before trusting the numbers:
# ##   1. confirm which application's output lands in which log;
# ##   2. confirm the completion markers (COND_MARKER);
# ##   3. cross-check plot.py's EXTRACTORS against real app output.
# ## The plotting path IS validated: it reproduces real.png from the paper CSV.
# ###########################################################################
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/6-resource-util"
ae_init_output_dirs "$AE_DIR"
AE_LOG_DIR="$LOG_DIR"
TIMEOUT="${TIMEOUT:-1800}"
DBX_CONFIG="$AE_REPO_ROOT/user/demos/dbx1000/config.h"

# Validate every scope selector before taking the global runner lock or
# changing host/repository state.  Reordering a complete set (or changing its
# whitespace) still counts as a complete 36-point request.
ALL_STRESS_TYPES="1 2 3 4 5 6"
ALL_CONDS="single stress p3os"
ALL_SINGLE_BENCHES="matrix leveldb linear-regression dbx1000 pca redis word-count memcached gemini string-match kmeans cnn"
STRESS_TYPES="${STRESS_TYPES-$ALL_STRESS_TYPES}"
CONDS="${CONDS-$ALL_CONDS}"
SINGLE_BENCHES="${SINGLE_BENCHES-$ALL_SINGLE_BENCHES}"

validate_scope_list() {
    local label="$1" raw="$2"
    shift 2
    local -a values=() normalized=() seen_values=()
    local value allowed seen found

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
        for seen in "${seen_values[@]}"; do
            if [ "$value" = "$seen" ]; then
                echo "[AE] duplicate $label value: $value" >&2
                return 1
            fi
        done
        seen_values+=("$value")
        normalized+=("$value")
    done
    VALIDATED_SCOPE="${normalized[*]}"
}

validate_scope_list STRESS_TYPES "$STRESS_TYPES" 1 2 3 4 5 6 || exit 1
STRESS_TYPES="$VALIDATED_SCOPE"
validate_scope_list CONDS "$CONDS" single stress p3os || exit 1
CONDS="$VALIDATED_SCOPE"
validate_scope_list SINGLE_BENCHES "$SINGLE_BENCHES" \
    matrix leveldb linear-regression dbx1000 pca redis word-count memcached \
    gemini string-match kmeans cnn || exit 1
SINGLE_BENCHES="$VALIDATED_SCOPE"

FULL_PLOT_REQUEST=0
if [ "$(wc -w <<< "$STRESS_TYPES")" -eq "$(wc -w <<< "$ALL_STRESS_TYPES")" ] \
    && [ "$(wc -w <<< "$CONDS")" -eq "$(wc -w <<< "$ALL_CONDS")" ] \
    && [ "$(wc -w <<< "$SINGLE_BENCHES")" -eq "$(wc -w <<< "$ALL_SINGLE_BENCHES")" ]; then
    FULL_PLOT_REQUEST=1
fi

ae_acquire_run_lock "resource-util" || exit 1

TMP_DIR="$(mktemp -d)"
cp "$DBX_CONFIG" "$TMP_DIR/dbx1000-config.h"

# prepare_cnn.sh materializes ignored TinyCNN inputs in its source checkout.
# Snapshot their exact pre-run contents so cleanup restores user-provided model
# data and removes only files which were absent before this run.  The other
# legacy demos now build from clean, Git-filtered copies outside their sources.
snapshot_generated_tree() {
    local key="$1" git_root="$2"
    shift 2
    local manifest="$TMP_DIR/${key}-generated-before.nul"
    local backup="$TMP_DIR/${key}-generated-backup" path
    mkdir -p "$backup"
    {
        git -C "$git_root" ls-files -z --others --exclude-standard -- "$@"
        git -C "$git_root" ls-files -z --others --ignored --exclude-standard -- "$@"
    } | sort -zu > "$manifest"
    while IFS= read -r -d '' path; do
        mkdir -p "$backup/$(dirname "$path")"
        cp -a "$git_root/$path" "$backup/$path"
    done < "$manifest"
}

restore_generated_tree() {
    local key="$1" git_root="$2"
    shift 2
    local manifest="$TMP_DIR/${key}-generated-before.nul"
    local backup="$TMP_DIR/${key}-generated-backup"
    local current="$TMP_DIR/${key}-generated-current.nul" path failed=0
    {
        git -C "$git_root" ls-files -z --others --exclude-standard -- "$@"
        git -C "$git_root" ls-files -z --others --ignored --exclude-standard -- "$@"
    } | sort -zu > "$current" || return 1
    while IFS= read -r -d '' path; do
        rm -f "$git_root/$path" || failed=1
    done < "$current"
    while IFS= read -r -d '' path; do
        mkdir -p "$git_root/$(dirname "$path")" || failed=1
        cp -a "$backup/$path" "$git_root/$path" || failed=1
    done < "$manifest"
    return "$failed"
}

mkdir -p "$AE_LOG_DIR" "$CSV_DIR" "$FIG_DIR"

snapshot_generated_tree tinycnn "$AE_REPO_ROOT/user/demos/VeryTinyCnn" \
    include/CImg.h data image

WELCOME="Welcome to ChCore shell!"

# type N -> benches whose metrics land in single_stress_typeN.sh (machine 0).
stress_type_benches() {
    case "$1" in
        1) echo "leveldb matrix" ;;
        2) echo "dbx1000 word-count" ;;
        3) echo "linear-regression redis" ;;
        4) echo "memcached pca" ;;
        5) echo "cnn kmeans" ;;
        6) echo "string-match gemini" ;;
        *) echo "Unknown stress type: $1" >&2; return 1 ;;
    esac
}

# Cross scripts mirror single_stress pairs: one app per machine.
# Format: "bench:machine ..."  (machine 0 = local, 1 = offloaded / cross)
cross_type_bench_machines() {
    case "$1" in
        1) echo "leveldb:0 matrix:1" ;;
        2) echo "dbx1000:0 word-count:1" ;;
        3) echo "redis:0 linear-regression:1" ;;
        4) echo "memcached:0 pca:1" ;;
        5) echo "kmeans:0 cnn:1" ;;
        6) echo "string-match:0 gemini:1" ;;
        *) return 0 ;;
    esac
}

single_marker() {
    case "$1" in
        matrix|linear-regression|pca|word-count|string-match|kmeans) echo "finalize:" ;;
        leveldb)    echo "MB/s" ;;
        dbx1000)    echo "thp=" ;;
        redis)      echo "requests per second" ;;
        memcached)  echo "ops/sec:" ;;
        gemini)     echo "exec_time=" ;;
        cnn)        echo "All finished." ;;
        *) echo "Unknown bench: $1" >&2; return 1 ;;
    esac
}

# CPU-bound applications often produce no serial output between initialization
# and their final metric.  For workloads observed doing so, rely on the full
# experiment timeout instead of treating 120 seconds of silence as a hang.
wait_for_bench() {
    local machine="$1" bench="$2" marker="$3" label="$4"
    local watch_count="${5:-1}"
    case "$bench" in
        matrix|linear-regression|pca|kmeans|cnn|gemini|leveldb|memcached)
            AE_LOG_STALL_S=0 ae_wait_in_log \
                "$machine" "$marker" "$TIMEOUT" "$label" "$watch_count"
            ;;
        *)
            ae_wait_in_log \
                "$machine" "$marker" "$TIMEOUT" "$label" "$watch_count"
            ;;
    esac
}

# Launch one app on a ready 1-machine cluster (CPU binds safe for a single VM).
run_single_app() {
    local bench="$1"
    case "$bench" in
        matrix)
            ae_send_command 0 "write matrix_multiply_bind_cpu.txt 0-11"
            sleep 1
            ae_send_command 0 "matrix_multiply.bin -l 2000 -r 2000 -c 0 -t 8"
            ;;
        leveldb)           ae_send_command 0 "source run_leveldb.sh" ;;
        linear-regression) ae_send_command 0 "source run_linear_regression.sh" ;;
        dbx1000)
            ae_send_command 0 "write dbx1000_bind_cpu.txt 0-7"
            sleep 1
            ae_send_command 0 "rundb.bin -t8 -r1 -w0 -z0.6"
            ;;
        pca)               ae_send_command 0 "source run_pca.sh" ;;
        redis)             ae_send_command 0 "source run_redis.sh" ;;
        word-count)        ae_send_command 0 "source run_word_count.sh" ;;
        memcached)         ae_send_command 0 "source run_memcached.sh" ;;
        gemini)            ae_send_command 0 "source run_gemini.sh" ;;
        string-match)      ae_send_command 0 "source run_string_match.sh" ;;
        kmeans)            ae_send_command 0 "source run_kmeans.sh" ;;
        cnn)               ae_send_command 0 "source run_cnn.sh" ;;
        *) echo "Unknown bench: $bench" >&2; return 1 ;;
    esac
}

ae_ensure_clean_tmux
ae_check_global_prepare || exit 1
ae_ensure_base_build
ae_save_build_configs
cleanup() {
    local rc=$? cleanup_failed=0 source_failed=0
    trap - EXIT
    ae_kill_cluster || cleanup_failed=1
    if [ -d "$TMP_DIR" ]; then
        cp "$TMP_DIR/dbx1000-config.h" "$DBX_CONFIG" || source_failed=1
        restore_generated_tree tinycnn "$AE_REPO_ROOT/user/demos/VeryTinyCnn" \
            include/CImg.h data image || source_failed=1
        if [ "$source_failed" -eq 0 ]; then
            rm -rf "$TMP_DIR"
        else
            echo "[AE] failed to restore optional source files; backup retained at $TMP_DIR" >&2
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

# Resource-util is a fixed placement experiment.  Do not inherit allocation
# policy from a preceding state-partition or auto-scale run.
ae_set_dsm_var DSM_MALLOC_MODE MIXED_DEFAULT_CXL
ae_set_dsm_var DSM_USER_MALLOC_MODE DEFAULT_DRAM

"$AE_DIR/prepare_cnn.sh"
for demo in REDIS MEMCACHED MEMCACHETEST TINYCNN; do
    ae_set_demo_var "CHCORE_DEMOS_${demo}" ON
    ae_set_dotconfig "CHCORE_DEMOS_${demo}" BOOL ON
done
# real.png uses DBx1000's compact read-only YCSB workload, not the 64-warehouse
# TPC-C auto-scale build currently pinned in the demo submodule.
sed -i 's/^#define NUM_MACHINES[[:space:]].*/#define NUM_MACHINES\t\t\t\t1/' "$DBX_CONFIG"
sed -i 's/^#define THREADS_PER_MACHINE[[:space:]].*/#define THREADS_PER_MACHINE\t\t\t8/' "$DBX_CONFIG"
sed -i 's/^#define WARMUP[[:space:]].*/#define WARMUP\t\t\t\t\t\t0/' "$DBX_CONFIG"
sed -i 's/^#define WORKLOAD[[:space:]].*/#define WORKLOAD\t\t\t\t\tYCSB/' "$DBX_CONFIG"
sed -i 's/^#define MAX_TXN_PER_PART[[:space:]].*/#define MAX_TXN_PER_PART\t\t\t100000/' "$DBX_CONFIG"
sed -i 's/^#define SYNTH_TABLE_SIZE[[:space:]].*/#define SYNTH_TABLE_SIZE\t\t\t(1024 * 10)/' "$DBX_CONFIG"
ae_build
for binary in redis-server redis-benchmark memcached memcachetest tiny-cnn; do
    if [ ! -x "$AE_REPO_ROOT/user/build/ramdisk/$binary" ]; then
        echo "[AE] required resource-util binary missing after build: $binary" >&2
        exit 1
    fi
done

# Copy/link a source log to <bench>_<cond>.log (overwrite if present).
link_bench_log() {
    local src="$1" bench="$2" cond="$3"
    local dst="$AE_LOG_DIR/${bench}_${cond}.log"
    if [ -f "$src" ]; then
        cp "$src" "$dst"
    else
        echo "[AE] WARN: missing source log for ${bench}_${cond}: $src" >&2
    fi
}

run_single_baselines() {
    local bench marker wait_rc failed=0
    echo "### real: single baselines (one app per 1-machine boot)"
    for bench in $SINGLE_BENCHES; do
        marker="$(single_marker "$bench")"
        rm -f -- "$AE_LOG_DIR/${bench}_single.log"
        echo "### single: $bench"
        if ! ae_boot_cluster 1; then
            failed=1
            ae_kill_cluster
            continue
        fi
        if ! run_single_app "$bench"; then
            failed=1
            ae_kill_cluster
            continue
        fi
        if wait_for_bench 0 "$bench" "$marker" "single/$bench"; then
            wait_rc=0
        else
            wait_rc=$?
            failed=1
        fi
        if [ "$wait_rc" -eq 0 ]; then
            sleep 2
        fi
        cp "$(ae_machine_log 0)" "$AE_LOG_DIR/${bench}_single.log" || true
        ae_kill_cluster
    done
    return "$failed"
}

# stress condition (traditional, single machine): source single_stress_typeN.sh
run_stress_type() {
    local type="$1" bench src marker failed=0
    echo "### real: stress (traditional) type $type"
    # _demux_type_logs() may reconstruct per-benchmark logs from this archive.
    # Remove it before boot so a failed retry cannot resurrect stale metrics.
    rm -f -- "$AE_LOG_DIR/machine0-stress-type${type}.log"
    for bench in $(stress_type_benches "$type"); do
        rm -f -- "$AE_LOG_DIR/${bench}_stress.log"
    done
    ae_boot_cluster 1 || { ae_kill_cluster; return 1; }
    ae_send_command 0 "source single_stress_type${type}.sh"
    for bench in $(stress_type_benches "$type"); do
        marker="$(single_marker "$bench")"
        if ! wait_for_bench 0 "$bench" "$marker" \
            "stress/type${type}/$bench" 1; then
            failed=1
            break
        fi
    done
    ae_archive_logs 1 "$AE_LOG_DIR" "-stress-type${type}"
    src="$AE_LOG_DIR/machine0-stress-type${type}.log"
    for bench in $(stress_type_benches "$type"); do
        link_bench_log "$src" "$bench" "stress"
    done
    ae_kill_cluster
    return "$failed"
}

# p3os condition (StarfishOS, two machines): source cross_stress_typeN_m{0,1}.sh
run_cross_type() {
    local type="$1" entry bench mach src marker failed=0
    echo "### real: p3os (StarfishOS cross-machine) type $type"
    # These archives are fallback inputs to _demux_type_logs(); invalidate both
    # before boot for the same reason as the standard per-benchmark logs.
    rm -f -- "$AE_LOG_DIR/machine0-p3os-type${type}.log" \
        "$AE_LOG_DIR/machine1-p3os-type${type}.log"
    for entry in $(cross_type_bench_machines "$type"); do
        bench="${entry%%:*}"
        rm -f -- "$AE_LOG_DIR/${bench}_p3os.log"
    done
    ae_boot_cluster 2 || { ae_kill_cluster; return 1; }
    ae_send_command 0 "source cross_stress_type${type}_m0.sh"
    ae_send_command 1 "source cross_stress_type${type}_m1.sh"
    for entry in $(cross_type_bench_machines "$type"); do
        bench="${entry%%:*}"
        mach="${entry##*:}"
        marker="$(single_marker "$bench")"
        if ! wait_for_bench "$mach" "$bench" "$marker" \
            "p3os/type${type}/$bench" 2; then
            failed=1
        fi
    done
    ae_archive_logs 2 "$AE_LOG_DIR" "-p3os-type${type}"
    for entry in $(cross_type_bench_machines "$type"); do
        bench="${entry%%:*}"
        mach="${entry##*:}"
        src="$AE_LOG_DIR/machine${mach}-p3os-type${type}.log"
        link_bench_log "$src" "$bench" "p3os"
    done
    ae_kill_cluster
    return "$failed"
}

failed=0
for cond in $CONDS; do
    case "$cond" in
        single) run_single_baselines || failed=1 ;;
        stress) for t in $STRESS_TYPES; do run_stress_type "$t" || failed=1; done ;;
        p3os)   for t in $STRESS_TYPES; do run_cross_type "$t" || failed=1; done ;;
    esac
done

echo ""
echo "=== Parsing + plotting real (resource-util) ==="
plot_args=(--log-dir "$AE_LOG_DIR" --csv-dir "$CSV_DIR" --fig-dir "$FIG_DIR")
if [ "$FULL_PLOT_REQUEST" != "1" ]; then
    plot_args+=(--allow-partial)
fi
for cond in $CONDS; do
    case "$cond" in
        single)
            for bench in $SINGLE_BENCHES; do
                plot_args+=(--require-point "${bench}-single")
            done
            ;;
        stress)
            for t in $STRESS_TYPES; do
                for bench in $(stress_type_benches "$t"); do
                    plot_args+=(--require-point "${bench}-stress")
                done
            done
            ;;
        p3os)
            for t in $STRESS_TYPES; do
                for entry in $(cross_type_bench_machines "$t"); do
                    bench="${entry%%:*}"
                    plot_args+=(--require-point "${bench}-p3os")
                done
            done
            ;;
    esac
done
plot_succeeded=1
python3 "$AE_DIR/plot.py" "${plot_args[@]}" || {
    failed=1
    plot_succeeded=0
}

echo ""
echo "real CSV target: $CSV_DIR/real.csv"
if [ "$plot_succeeded" = "1" ] && [ -f "$FIG_DIR/real.png" ]; then
    echo "real figure target: $FIG_DIR/real.png"
elif [ "$plot_succeeded" = "1" ]; then
    echo "real figure not generated (no complete single/stress/p3os triplet)"
else
    echo "real outputs incomplete: parsing/plotting failed"
fi
echo "Artifact output: $OUT_DIR"
ae_finish
exit "$failed"
