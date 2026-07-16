#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE="$AE_DIR/linux_sched_notify_microbench.c"
BUILD_DIR="${BUILD_DIR:-/tmp/chcore-cxl-linux-sched-notify-$USER}"
BINARY="$BUILD_DIR/linux_sched_notify_microbench.bin"
OUT_DIR="${OUT_DIR:-$AE_DIR/linux-results}"
LOG_DIR="${LOG_DIR:-$OUT_DIR/logs}"
CSV_DIR="${CSV_DIR:-$OUT_DIR/csv}"
FIG_DIR="${FIG_DIR:-$OUT_DIR/figures}"
NRUNS="${NRUNS:-3}"
SAMPLES="${SAMPLES:-1000}"
SOURCE_CPU="${SOURCE_CPU:-}"
REMOTE_CPU="${REMOTE_CPU:-}"
CC="${CC:-cc}"

mkdir -p "$BUILD_DIR" "$LOG_DIR" "$CSV_DIR" "$FIG_DIR"

echo "=== Building Linux sched/notify microbenchmark ==="
"$CC" -O2 -std=gnu11 -Wall -Wextra -pthread "$SOURCE" -o "$BINARY"

args=("$SAMPLES")
if [ -n "$SOURCE_CPU" ] || [ -n "$REMOTE_CPU" ]; then
    if [ -z "$SOURCE_CPU" ] || [ -z "$REMOTE_CPU" ]; then
        echo "SOURCE_CPU and REMOTE_CPU must be specified together" >&2
        exit 2
    fi
    args+=("$SOURCE_CPU" "$REMOTE_CPU")
fi

for run in $(seq 1 "$NRUNS"); do
    run_dir="$LOG_DIR/run$run"
    mkdir -p "$run_dir"
    echo "=== Linux run $run/$NRUNS: samples=$SAMPLES ==="
    "$BINARY" "${args[@]}" | tee "$run_dir/machine0.log"
done

echo "=== Parsing Linux samples and drawing figure ==="
python3 "$AE_DIR/plot.py" --log-dir "$LOG_DIR" --csv-dir "$CSV_DIR" --fig-dir "$FIG_DIR"
echo "Linux artifact output: logs=$LOG_DIR csv=$CSV_DIR figures=$FIG_DIR"
