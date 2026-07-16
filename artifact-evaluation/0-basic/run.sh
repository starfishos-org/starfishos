#!/usr/bin/env bash
# One-click entry for 0-basic: QEMU MSI microbench + host Linux MLC (Table 1).
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$DIR/../common.sh"
AE_DIR="$DIR"
ae_init_output_dirs "$AE_DIR"
export OUT_DIR LOG_DIR CSV_DIR FIG_DIR

"$DIR/run_msi.sh"

ALLOW_MLC_SKIP="${ALLOW_MLC_SKIP:-1}"
export ALLOW_MLC_SKIP
"$DIR/run_mlc.sh"

echo "Artifact output: $OUT_DIR"
