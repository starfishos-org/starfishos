#!/usr/bin/env bash
# Compatibility wrapper — the one-click entry point is run_all.py.
#
#   python3 artifact-evaluation/run_all.py
#   ./artifact-evaluation/run_all.py
#   ./artifact-evaluation/run-all.sh          # this file → run_all.py
#
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Map legacy env vars into argparse flags when not already on the CLI.
extra=()
[ "${DRY_RUN:-0}" = "1" ] && extra+=(--dry-run)
[ "${SKIP_PREPARE:-0}" = "1" ] && extra+=(--no-prepare)
[ "${SKIP_BASE_BUILD:-0}" = "1" ] && extra+=(--no-build)

exec python3 "$AE_ROOT/run_all.py" "${extra[@]}" "$@"
