#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi
# Emergency stop for a stuck artifact-evaluation run.
set -euo pipefail

AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")" && pwd)"
source "$AE_ROOT/common.sh"

echo "=== Forcibly stopping StarfishOS artifact runners, tmux, and QEMU ==="
ae_force_stop_artifact_runners
