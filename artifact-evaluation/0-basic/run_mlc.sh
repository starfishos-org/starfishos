#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${OUT_DIR:-$SCRIPT_DIR}"
MODE="${MODE:-all}"

if [ -z "${MLC_BIN:-}" ]; then
    for candidate in \
        /home/wfn/MLC-Linux/mlc \
        /home/wfn/shm-pcc-sdk/tests/basic/global_tests/mlc; do
        if [ -x "$candidate" ]; then
            MLC_BIN="$candidate"
            break
        fi
    done
fi

if [ -z "${MLC_BIN:-}" ] || [ ! -x "$MLC_BIN" ]; then
    if [ "${ALLOW_MLC_SKIP:-1}" = "1" ]; then
        echo "Intel MLC not found; skipping (set MLC_BIN=/path/to/mlc to run)." >&2
        exit 0
    fi
    echo "Intel MLC not found. Set MLC_BIN=/path/to/mlc (or ALLOW_MLC_SKIP=1)." >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
echo "Using Intel MLC: $MLC_BIN"
read -r -a mlc_args <<< "${MLC_ARGS:-}"

case "$MODE" in
    matrix)
        "$MLC_BIN" --bandwidth_matrix -e "${mlc_args[@]}" |
            tee "$OUT_DIR/mlc_bandwidth_matrix.log"
        ;;
    peak)
        "$MLC_BIN" --peak_injection_bandwidth -e "${mlc_args[@]}" |
            tee "$OUT_DIR/mlc_peak_bandwidth.log"
        ;;
    all)
        "$MLC_BIN" --bandwidth_matrix -e "${mlc_args[@]}" |
            tee "$OUT_DIR/mlc_bandwidth_matrix.log"
        "$MLC_BIN" --peak_injection_bandwidth -e "${mlc_args[@]}" |
            tee "$OUT_DIR/mlc_peak_bandwidth.log"
        ;;
    *)
        echo "MODE must be matrix, peak, or all" >&2
        exit 2
        ;;
esac
