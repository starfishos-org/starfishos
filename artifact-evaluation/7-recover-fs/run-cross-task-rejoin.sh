#!/usr/bin/env bash
# Run a cross-machine process, replace one participant QEMU with the same
# machine ID, and require coordinated task termination and reclamation.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CROSS_TASK_ONLY=1 exec "$SCRIPT_DIR/run.sh" "$@"
