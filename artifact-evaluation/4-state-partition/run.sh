#!/usr/bin/env bash
#
# Artifact script for paper Figure 13: "Performance across state-partition
# choices".
#
# Runs 6 applications (LevelDB, DBx1000, PCA, Matrix Multiply, Linear
# Regression, Word Count) on a 2-machine cluster under four state-partition
# configurations, then plots performance normalized to the Private
# (All-DRAM) setup.
#
# Config -> kernel/dsm_config.cmake mapping (all five per-type modes —
# THREADCTX/PGTABLE/STACK/OBJECT/PAGE — stay "CXL" except in All_DRAM):
#
#   Config (paper label)                  DSM_MALLOC_MODE     DSM_USER_MALLOC_MODE  type modes
#   All_CXL (Share)                       CXL                 DEFAULT_CXL           CXL
#   Kernel_DRAM_User_CXL (K-mix/U-share)  MIXED_DEFAULT_DRAM  DEFAULT_CXL           CXL
#   Kernel_Page_CXL_Other_DRAM (K-mix/U-mix) MIXED_DEFAULT_DRAM DEFAULT_DRAM        CXL
#   All_DRAM (Private)                    DRAM                DEFAULT_DRAM          DRAM
#
# Usage (from repo root):
#   ./artifact-evaluation/prepare.sh          # once
#   ./artifact-evaluation/4-state-partition/run.sh
#
# Env overrides:
#   BENCHS="leveldb dbx1000 pca matrix_multiply linear_regression word_count"
#   CONFIGS="All_CXL Kernel_DRAM_User_CXL Kernel_Page_CXL_Other_DRAM All_DRAM"
#   NUM_MACHINES=2   TIMEOUT=1200   OUT_DIR=out/<timestamp>
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/4-state-partition"
TS="${TS:-$(date +%Y%m%d_%H%M%S)}"
OUT_DIR="${OUT_DIR:-$AE_DIR/out/$TS}"
AE_LOG_DIR="$OUT_DIR/logs"
NUM_MACHINES="${NUM_MACHINES:-2}"
TIMEOUT="${TIMEOUT:-1200}"
# The default is the complete 6 x 4 matrix used by the paper figure.  The
# DBx1000 compile-time configuration is reduced below for this two-machine
# state-placement experiment; the 64-warehouse auto-scale configuration would
# otherwise consume far more memory than this figure requires.
BENCHS="${BENCHS:-leveldb dbx1000 pca matrix_multiply linear_regression word_count}"
CONFIGS="${CONFIGS:-All_CXL Kernel_DRAM_User_CXL Kernel_Page_CXL_Other_DRAM All_DRAM}"

mkdir -p "$AE_LOG_DIR" "$OUT_DIR/results" "$OUT_DIR/figures"

# config -> DSM_MALLOC_MODE  DSM_USER_MALLOC_MODE  <5 type modes>
config_params() {
    case "$1" in
        All_CXL)                    echo "CXL                DEFAULT_CXL  CXL" ;;
        Kernel_DRAM_User_CXL)       echo "MIXED_DEFAULT_DRAM DEFAULT_CXL  CXL" ;;
        Kernel_Page_CXL_Other_DRAM) echo "MIXED_DEFAULT_DRAM DEFAULT_DRAM CXL" ;;
        All_DRAM)                   echo "DRAM               DEFAULT_DRAM DRAM" ;;
        *) echo "Unknown config: $1" >&2; return 1 ;;
    esac
}

# bench -> string that marks completion in machine 0's log
bench_done_pattern() {
    case "$1" in
        leveldb)  echo "MB/s" ;;
        dbx1000)  echo "thp=" ;;
        # all four phoenix apps print "finalize: <us>" as their last timing line
        pca|matrix_multiply|linear_regression|word_count) echo "finalize:" ;;
        *) echo "Unknown bench: $1" >&2; return 1 ;;
    esac
}

# dbx1000's vendored config is the 8-machine TPC-C setup; sync its
# compile-time NUM_MACHINES to this experiment's cluster size and launch it
# with threads bound across all machines (12 cores per machine).
DBX_CONFIG="$AE_REPO_ROOT/user/demos/dbx1000/config.h"
DBX_TIMEOUT="${DBX_TIMEOUT:-3600}"
DBX_DRAM_SIZE="${DBX_DRAM_SIZE:-24G}"
DBX_NUM_WH="${DBX_NUM_WH:-1}"
DBX_WARMUP="${DBX_WARMUP:-10000}"
DBX_MAX_TXN="${DBX_MAX_TXN:-10000}"

TMP_DIR="$(mktemp -d)"
cp "$DBX_CONFIG" "$TMP_DIR/config.h"

dbx_bind_cpu_list() {
    local n="$1" i parts=()
    for i in $(seq 0 $((n - 1))); do
        if [ "$i" -eq 0 ]; then
            parts+=("0-8")
        else
            parts+=("$((i * 12))-$((i * 12 + 7))")
        fi
    done
    (IFS=,; echo "${parts[*]}")
}

cleanup() {
    ae_kill_cluster
    cp "$TMP_DIR/config.h" "$DBX_CONFIG"
    rm -rf "$TMP_DIR"
    ae_restore_build_configs
}
trap cleanup EXIT

cd "$AE_REPO_ROOT"
ae_ensure_clean_tmux
ae_check_global_prepare
ae_save_build_configs

for cfg in $CONFIGS; do
    read -r malloc_mode user_malloc_mode type_mode <<< "$(config_params "$cfg")"

    # Private (All_DRAM) is the single-machine ideal baseline: with all
    # state forced into private DRAM, cross-machine sharing is impossible by
    # design (2-machine runs crash in virt_to_page), so it runs on 1 machine
    # — matching the original experiments (old logs used ./build/simulate.sh).
    if [ "$cfg" = "All_DRAM" ]; then
        cfg_machines=1
    else
        cfg_machines="$NUM_MACHINES"
    fi

    echo ""
    echo "########################################################"
    echo "### Config: $cfg ($cfg_machines machine(s))"
    echo "###   DSM_MALLOC_MODE=$malloc_mode  DSM_USER_MALLOC_MODE=$user_malloc_mode"
    echo "###   THREADCTX/PGTABLE/STACK/OBJECT/PAGE=$type_mode"
    echo "########################################################"

    ae_set_dsm_var DSM_MALLOC_MODE "$malloc_mode"
    ae_set_dsm_var DSM_USER_MALLOC_MODE "$user_malloc_mode"
    for t in THREADCTX PGTABLE STACK OBJECT PAGE; do
        ae_set_dsm_var "DSM_${t}_MODE" "$type_mode"
    done
    # dbx1000's compile-time NUM_MACHINES/PART_CNT must match the cluster size
    sed -i "s/^#define NUM_MACHINES[[:space:]].*/#define NUM_MACHINES\t\t\t$cfg_machines/" "$DBX_CONFIG"
    sed -i "s/^#define NUM_WH[[:space:]].*/#define NUM_WH\t\t\t\t\t\t$DBX_NUM_WH/" "$DBX_CONFIG"
    sed -i "s/^#define WARMUP[[:space:]].*/#define WARMUP\t\t\t\t\t\t$DBX_WARMUP/" "$DBX_CONFIG"
    sed -i "s/^#define MAX_TXN_PER_PART[[:space:]].*/#define MAX_TXN_PER_PART\t\t\t$DBX_MAX_TXN/" "$DBX_CONFIG"
    sed -i "s/^#define ITEM_I_DATA_LEN[[:space:]].*/#define ITEM_I_DATA_LEN\t\t\t50/" "$DBX_CONFIG"
    DBX_BIND_LIST="$(dbx_bind_cpu_list "$cfg_machines")"
    ae_build

    for bench in $BENCHS; do
        pattern="$(bench_done_pattern "$bench")"
        logfile="$AE_LOG_DIR/${bench}_${cfg}.log"
        echo "=== [$cfg] running $bench on $cfg_machines machine(s) (done pattern: '$pattern') ==="
        if [ "$bench" = "dbx1000" ]; then
            if ! AE_EXTRA_ENV="DRAM_SIZE=$DBX_DRAM_SIZE" ae_boot_cluster "$cfg_machines"; then
                ae_record_error "boot failed for $bench under $cfg"
                continue
            fi
            ae_send_command 0 "write dbx1000_bind_cpu.txt $DBX_BIND_LIST"
            sleep 2
            ae_send_command 0 "rundb.bin"
            bench_timeout="$DBX_TIMEOUT"
        elif [ "$bench" = "matrix_multiply" ]; then
            # run_matrix_multiply.sh hardcodes the 2-machine setup (16 threads
            # bound to 0-10,12-23); on 1 machine threads bound to CPUs >= 12
            # never run and the app hangs at its barrier. Use the original
            # single-machine parameters (8 threads, CPUs 0-11) instead.
            if ! ae_boot_cluster "$cfg_machines"; then
                ae_record_error "boot failed for $bench under $cfg"
                continue
            fi
            if [ "$cfg_machines" -eq 1 ]; then
                ae_send_command 0 "write matrix_multiply_bind_cpu.txt 0-11"
                sleep 2
                ae_send_command 0 "matrix_multiply.bin -l 2000 -r 2000 -c 0 -t 8"
            else
                ae_send_command 0 "source run_${bench}.sh"
            fi
            bench_timeout="$TIMEOUT"
        else
            if ! ae_boot_cluster "$cfg_machines"; then
                ae_record_error "boot failed for $bench under $cfg"
                continue
            fi
            ae_send_command 0 "source run_${bench}.sh"
            bench_timeout="$TIMEOUT"
        fi
        if ae_wait_in_log 0 "$pattern" "$bench_timeout" "$bench done"; then
            sleep 3   # let trailing output (e.g. summary lines) flush
            cp "$(ae_machine_log 0)" "$logfile"
        else
            # rc 1 (timeout) or 3 (guest error/crash) — the specific reason is
            # already recorded above; save the log and skip to the next test.
            cp "$(ae_machine_log 0)" "$logfile" || true
            echo "[WARN] $bench under $cfg did not complete; skipping to next test" >&2
            ae_record_error "$bench under $cfg did not produce a complete result"
        fi
        ae_kill_cluster
    done
done

ae_restore_build_configs

echo ""
echo "=== Parsing logs and generating figure ==="
python3 "$AE_DIR/plot.py" --log-dir "$AE_LOG_DIR" --out-dir "$OUT_DIR"

echo "Artifact output: $OUT_DIR"
ae_finish
