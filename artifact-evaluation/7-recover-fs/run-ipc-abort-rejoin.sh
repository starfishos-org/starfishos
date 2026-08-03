#!/usr/bin/env bash
# Keep an app alive while its remote polling service machine is replaced.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IPC_ABORT_ONLY=1 exec "$SCRIPT_DIR/run.sh" "$@"
