#!/usr/bin/env bash
#
# Artifact script for the paper "process-migration" breakdown figures
# (process-migration-data-large.eps / process-migration-data-small.eps).
#
# For each benchmark it boots a 2-machine cluster, starts the workload on
# machine 0, checkpoints it with `test_cfork_prepare.bin` (emitting the
# prepare/checkpoint timing breakdown + per-object copy times on machine 0),
# then restores it on machine 1 with `test_cfork_restore.bin` (emitting the
# restore breakdown on machine 1).  Both machine logs are concatenated into
# one <Benchmark>.log and parsed into the paper CSV schema.
#
# Requires CHCORE_SSI_SLS=ON (the cfork/checkpoint path) — already the default
# in this tree — and the cfork test binaries in hostfs
# (test_cfork_prepare.bin, test_cfork_restore.bin, and each workload's .bin).
# The timing print itself is unconditional (PERF_TIMING_CFORK in
# kernel/include/dsm/perf_timing.h), so no special rebuild is needed.
#
# NOTE: the per-benchmark ready/prepare/restore markers below mirror
# dsm-scripts/tests/{config,cfork_prepare,cfork_restore}.exp; validate them
# against a live run and adjust if a workload uses a different completion line.
#
# Usage (from repo root):
#   ./artifact-evaluation/prepare.sh          # once
#   ./artifact-evaluation/8-process-migration/run.sh
#
# Env overrides:
#   BENCHES="float linpack matmul pyaes pca db1000"
#   NUM_MACHINES=2   TIMEOUT=120   OUT_DIR=out/<timestamp>
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

AE_DIR="$AE_REPO_ROOT/artifact-evaluation/8-process-migration"
TS="${TS:-$(date +%Y%m%d_%H%M%S)}"
OUT_DIR="${OUT_DIR:-$AE_DIR/out/$TS}"
AE_LOG_DIR="$OUT_DIR/logs"
NUM_MACHINES="${NUM_MACHINES:-2}"
TIMEOUT="${TIMEOUT:-120}"

# Benchmarks (lowercase = config.exp key; the CSV/figure label is capitalized).
BENCHES="${BENCHES:-float linpack matmul pyaes pca db1000}"

mkdir -p "$AE_LOG_DIR" "$OUT_DIR/results" "$OUT_DIR/figures"

# key -> "<binary> <args>" (mirrors dsm-scripts/tests/config.exp).
bench_cmd() {
    case "$1" in
        float)  echo "test_float.bin 100000000 2" ;;
        linpack) echo "test_linpack.bin 4000 2" ;;
        matmul) echo "test_matmul.bin 3000 2" ;;
        pyaes)  echo "test_pyaes.bin 10000000 100 2" ;;
        pca)    echo "pca.bin -c 1000 -r 1000 -t 4" ;;
        db1000) echo "rundb.bin -t8 -r1.0 -w0.0 -z0.6" ;;
        *) echo "Unknown bench: $1" >&2; return 1 ;;
    esac
}

# key -> the binary name passed to test_cfork_{prepare,restore}.bin.
bench_binary() {
    case "$1" in
        float)  echo "test_float.bin" ;;
        linpack) echo "test_linpack.bin" ;;
        matmul) echo "test_matmul.bin" ;;
        pyaes)  echo "test_pyaes.bin" ;;
        pca)    echo "pca.bin" ;;
        db1000) echo "rundb.bin" ;;
    esac
}

# key -> a line printed once the workload is up and ready to checkpoint.
bench_ready_marker() {
    case "$1" in
        float)  echo "Running /test_float.bin" ;;
        linpack) echo "Running /test_linpack.bin" ;;
        matmul) echo "Running /test_matmul.bin" ;;
        pyaes)  echo "Running /test_pyaes.bin" ;;
        pca)    echo "inter library:" ;;
        db1000) echo "workload initialized" ;;
    esac
}

# key -> the capitalized label used as the CSV Benchmark / figure x-tick.
bench_label() {
    case "$1" in
        float)  echo "Float" ;;
        linpack) echo "Linpack" ;;
        matmul) echo "Matmul" ;;
        pyaes)  echo "Pyaes" ;;
        pca)    echo "PCA" ;;
        db1000) echo "DB1000" ;;
    esac
}

# Markers emitted by print_perf_cfork_time / print_perf_restore_time.
PREPARE_DONE="prepare copy time object:"
RESTORE_DONE="perf_restore_time\[RESTORE_START_ALL_THREADS\]"

ae_check_global_prepare || exit 1
ae_ensure_base_build || exit 1

run_one_bench() {
    local key="$1"
    local cmd binary ready label
    cmd="$(bench_cmd "$key")"
    binary="$(bench_binary "$key")"
    ready="$(bench_ready_marker "$key")"
    label="$(bench_label "$key")"

    echo ""
    echo "############################################################"
    echo "### process-migration: $label  ($cmd)"
    echo "############################################################"

    ae_boot_cluster "$NUM_MACHINES" || { ae_kill_cluster; return 1; }

    # 1. start the workload on machine 0 and wait until it is ready.
    ae_send_command 0 "$cmd &"
    ae_wait_in_log 0 "$ready" "$TIMEOUT" "$label workload ready" || { ae_kill_cluster; return 1; }

    # 2. checkpoint / prepare on machine 0 (emits PREPARE/CKPT/copy times).
    ae_send_command 0 "test_cfork_prepare.bin $binary &"
    ae_wait_in_log 0 "$PREPARE_DONE" "$TIMEOUT" "$label prepare done" || { ae_kill_cluster; return 1; }

    # 3. restore on machine 1 (emits RESTORE times).
    ae_send_command 1 "test_cfork_restore.bin $binary &"
    ae_wait_in_log 1 "$RESTORE_DONE" "$TIMEOUT" "$label restore done" || { ae_kill_cluster; return 1; }

    # 4. concatenate both machine logs into <Label>.log for the parser.
    {
        cat "$(ae_machine_log 0)"
        cat "$(ae_machine_log 1)"
    } > "$AE_LOG_DIR/$label.log"
    ae_archive_logs "$NUM_MACHINES" "$AE_LOG_DIR" "-$label"

    ae_kill_cluster
}

for key in $BENCHES; do
    run_one_bench "$key" || echo "[AE] $key failed; continuing" >&2
done

echo ""
echo "=== Parsing + plotting process-migration ==="
python3 "$AE_DIR/parse_and_plot.py" --log-dir "$AE_LOG_DIR" --out-dir "$OUT_DIR" \
    --benches $(for k in $BENCHES; do bench_label "$k"; done)

echo ""
echo "process-migration figures under: $OUT_DIR/figures"
ae_finish
