#!/usr/bin/env bash
# Emergency stop for a stuck artifact-evaluation run.
set -euo pipefail

AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$AE_ROOT/common.sh"

echo "=== Forcibly stopping StarfishOS artifact runners, tmux, and QEMU ==="
ae_force_stop_artifact_runners
