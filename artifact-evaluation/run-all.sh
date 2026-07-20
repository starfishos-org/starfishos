#!/usr/bin/env bash
# Compatibility wrapper — the one-click entry point is run_all.py.
#
#   python3 artifact-evaluation/run_all.py
#   ./artifact-evaluation/run_all.py
#   ./artifact-evaluation/run-all.sh          # this file → run_all.py
#
set -euo pipefail
AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly TIGON_ADMIN_HELPER=/usr/local/libexec/starfishos-tigon

restricted_tigon_policy_available() {
    local action

    [ -x "$TIGON_ADMIN_HELPER" ] || return 1
    command -v sudo >/dev/null 2>&1 || return 1
    for action in start reset stop status; do
        if ! sudo -n -l "$TIGON_ADMIN_HELPER" "$action" >/dev/null 2>&1; then
            return 1
        fi
    done
    return 0
}

# Map legacy env vars into argparse flags when not already on the CLI.
extra=()
[ "${DRY_RUN:-0}" = "1" ] && extra+=(--dry-run)
[ "${SKIP_PREPARE:-0}" = "1" ] && extra+=(--no-prepare)
[ "${SKIP_BASE_BUILD:-0}" = "1" ] && extra+=(--no-build)

if restricted_tigon_policy_available; then
    # run_all.py still owns authoritative CLI parsing.  This marker has no
    # side effects; the auto-scale script starts Tigon only around its Tigon
    # baseline after all arguments and scope selectors have been validated.
    export STARFISHOS_RESTRICTED_TIGON=1
    export TIGON_SETUP=0
fi

exec python3 "$AE_ROOT/run_all.py" "${extra[@]}" "$@"
