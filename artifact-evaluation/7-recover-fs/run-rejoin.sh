#!/usr/bin/env bash
# Kill one QEMU and boot a replacement with the same logical machine ID while
# the other cluster member and the shared CXL backing remain alive.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REJOIN_ONLY=1 exec "$SCRIPT_DIR/run.sh" "$@"
