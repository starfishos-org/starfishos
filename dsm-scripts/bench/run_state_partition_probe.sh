#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi
#
# Probe driver for paper Figure 13 (artifact-evaluation/4-state-partition).
#
# Same boot/build/dispatch path as that experiment's run.sh, with three
# differences that the question "why is the ordering not what we expected"
# needs and the figure run does not:
#
#   1. every machine's serial log is kept, not just machine 0's.  The whole
#      point is where pages come from, and under a DRAM-first placement
#      machine 0 owns the pages while machines 1..N-1 are the ones migrating
#      them, so machine 0's log is the one log that shows nothing.
#   2. REPS runs the same point back to back without rebuilding, so
#      run-to-run spread can be separated from the placement effect.
#   3. no plotting and no --require-point: a point that fails is reported and
#      the sweep continues.
#
# Reads the DSM_MIGRATE_STATS counters ("[DMS] m=..." lines) if the kernel was
# built with them; see kernel/CMakeLists.txt.
#
# Usage (from repo root):
#   CONFIGS="Kernel_Page_CXL_Other_DRAM" BENCHS="word_count" REPS=2 \
#       ./dsm-scripts/bench/run_state_partition_probe.sh
#
# Env: CONFIGS BENCHS REPS MACHINES WORKERS_PER_MACHINE CPU_NUM TIMEOUT TS
#      MATRIX_LEN (matrix_multiply side length, default 2000)
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")/../.." && pwd)/artifact-evaluation/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/4-state-partition"
ae_init_output_dirs "$AE_DIR"

CONFIGS="${CONFIGS:-All_CXL Kernel_DRAM_User_CXL Kernel_Page_CXL_Other_DRAM}"
BENCHS="${BENCHS:-word_count matrix_multiply}"
REPS="${REPS:-1}"
MACHINES="${MACHINES:-8}"
WORKERS_PER_MACHINE="${WORKERS_PER_MACHINE:-8}"
CPU_NUM="${CPU_NUM:-12}"
TIMEOUT="${TIMEOUT:-1200}"
MATRIX_LEN="${MATRIX_LEN:-2000}"

# config -> DSM_MALLOC_MODE  DSM_USER_MALLOC_MODE  <5 per-type modes>
# Identical to 4-state-partition/run.sh; kept here rather than sourced because
# that file defines it inside a script that also runs the sweep.
config_params() {
    case "$1" in
        All_CXL)                    echo "CXL                DEFAULT_CXL  CXL" ;;
        Kernel_DRAM_User_CXL)       echo "MIXED_DEFAULT_DRAM DEFAULT_CXL  CXL" ;;
        Kernel_Page_CXL_Other_DRAM) echo "MIXED_DEFAULT_DRAM DEFAULT_DRAM CXL" ;;
        All_DRAM)                   echo "DRAM               DEFAULT_DRAM DRAM" ;;
        *) echo "Unknown config: $1" >&2; return 1 ;;
    esac
}

bench_bind_file() {
    case "$1" in
        pca)                echo "pca_bind_cpu.txt" ;;
        matrix_multiply)    echo "matrix_multiply_bind_cpu.txt" ;;
        linear_regression)  echo "linear_regression_bind_cpu.txt" ;;
        word_count)         echo "word_count_bind_cpu.txt" ;;
        *) echo "Unknown bench: $1" >&2; return 1 ;;
    esac
}

bench_command() {
    local bench="$1" workers="$2"
    case "$bench" in
        pca)                echo "pca.bin -c 1000 -r 1000 -t $workers" ;;
        matrix_multiply)
            echo "matrix_multiply.bin -l $MATRIX_LEN -r $MATRIX_LEN -c 0 -t $workers" ;;
        linear_regression)  echo "linear_regression.bin -f key_file_100MB.txt -t $workers" ;;
        word_count)         echo "word_count.bin -f word_100MB.txt -t $workers" ;;
        *) echo "Unknown bench: $1" >&2; return 1 ;;
    esac
}

worker_bind_cpu_list() {
    local n="$1" i parts=()
    for i in $(seq 0 $((n - 1))); do
        parts+=("$((i * CPU_NUM))-$((i * CPU_NUM + WORKERS_PER_MACHINE - 1))")
    done
    (IFS=,; echo "${parts[*]}")
}

ae_acquire_run_lock "state-partition-probe" || exit 1

cleanup() {
    local rc=$?
    trap - EXIT
    ae_kill_cluster || true
    ae_restore_build_configs || true
    exit "$rc"
}
trap cleanup EXIT

cd "$AE_REPO_ROOT"
ae_ensure_clean_tmux
ae_check_global_prepare
ae_save_build_configs
ae_set_paper_guest_cpu_config "$CPU_NUM"
ae_export_guest_cpu_num "$CPU_NUM"
ae_set_dsm_var DSM_CXL_LF_BUDDY OFF

WORKERS=$((WORKERS_PER_MACHINE * MACHINES))
BIND_LIST="$(worker_bind_cpu_list "$MACHINES")"
SUMMARY="$OUT_DIR/points.tsv"
printf 'config\tbench\trep\tstatus\tlibrary_us\n' > "$SUMMARY"

for cfg in $CONFIGS; do
    read -r malloc_mode user_malloc_mode type_mode <<< "$(config_params "$cfg")"
    ae_set_dsm_var DSM_MALLOC_MODE "$malloc_mode"
    ae_set_dsm_var DSM_USER_MALLOC_MODE "$user_malloc_mode"
    for t in THREADCTX PGTABLE STACK OBJECT PAGE; do
        ae_set_dsm_var "DSM_${t}_MODE" "$type_mode"
    done

    # Private is the single-machine ideal for this panel: one guest wide
    # enough for the panel's whole worker set, not a cluster.  Same total
    # workers on the same CPU pattern, so it stays comparable.
    if [ "$cfg" = "All_DRAM" ]; then
        boot_machines=1
        boot_cpus=$((MACHINES * CPU_NUM))
    else
        boot_machines="$MACHINES"
        boot_cpus="$CPU_NUM"
    fi

    echo ""
    echo "########## config $cfg: $malloc_mode / $user_malloc_mode / $type_mode"
    ae_set_paper_guest_cpu_config "$boot_cpus"
    ae_build || { ae_record_error "build failed for $cfg"; continue; }

    for bench in $BENCHS; do
        for rep in $(seq 1 "$REPS"); do
            point="${bench}_${cfg}_m${MACHINES}_r${rep}"
            echo "=== $point: $WORKERS workers on $boot_machines machine(s) x $boot_cpus vCPUs ==="
            if ! ae_boot_cluster "$boot_machines" "$boot_cpus"; then
                ae_record_error "boot failed for $point"
                printf '%s\t%s\t%s\tboot-failed\t\n' "$cfg" "$bench" "$rep" >> "$SUMMARY"
                continue
            fi
            ae_send_command 0 "write $(bench_bind_file "$bench") $BIND_LIST"
            sleep 2
            ae_send_command 0 "$(bench_command "$bench" "$WORKERS")"
            probe_status=ok
            if ! AE_LOG_STALL_S=0 ae_wait_in_log 0 "finalize:" "$TIMEOUT" \
                    "$point" "$boot_machines"; then
                probe_status=failed
                ae_record_error "$point did not complete"
            fi
            sleep 3
            # Every machine, always: the migrating machines are the interesting
            # ones and they are never machine 0 under a DRAM-first placement.
            ae_archive_logs "$boot_machines" "$LOG_DIR" "-${point}"
            lib="$(grep -aoE '^library: [0-9]+' "$(ae_machine_log 0)" \
                   | tail -1 | grep -oE '[0-9]+' || true)"
            printf '%s\t%s\t%s\t%s\t%s\n' \
                "$cfg" "$bench" "$rep" "$probe_status" "${lib:-}" >> "$SUMMARY"
            ae_kill_cluster
        done
    done
done

ae_restore_build_configs
echo ""
echo "=== summary ==="
column -t -s $'\t' "$SUMMARY"
echo "Artifact output: $OUT_DIR"
ae_finish
