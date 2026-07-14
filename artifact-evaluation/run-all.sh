#!/usr/bin/env bash
#
# One-click artifact evaluation: run every experiment sequentially and gather
# all figures into artifact-evaluation/out/<timestamp>/.
#
# Usage (from repo root):
#   ./artifact-evaluation/run-all.sh                # everything
#   ./artifact-evaluation/run-all.sh state-partition   # a subset
#
# This orchestrates the two multi-config sweep experiments that share
# common.sh (each rebuilds ChCore across several configs). The self-
# contained numbered evaluations (0-basic, 1-ipc-cdf, 2-sched-notify-
# latency, 3-memory-allocator, 7-recover-fs) have their own entry points
# and are run individually — see this directory's README.
#
# Experiments (in run order):
#   state-partition         Fig 13 (4-state-partition)
#   dbx1000-cross-warehouse reviewer-requested cross-warehouse sweep
#                           (5-dbx1000-cross-warehouse)
#
# Each experiment rebuilds ChCore with its own configs (and restores them),
# so they must run one at a time — this script enforces that. A failing
# experiment does not stop the rest; the summary at the end lists per-
# experiment status, and figures of successful runs are copied into the
# top-level output directory.
set -uo pipefail

AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$AE_ROOT/.." && pwd)"
TS="$(date +%Y%m%d_%H%M%S)"
TOP_OUT="$AE_ROOT/out/$TS"
FIG_DIR="$TOP_OUT/figures"
LOG_DIR="$TOP_OUT/logs"

ALL_EXPERIMENTS=(state-partition dbx1000-cross-warehouse)
EXPERIMENTS=("${@:-}")
if [ "${#EXPERIMENTS[@]}" -eq 0 ] || [ -z "${EXPERIMENTS[0]}" ]; then
    EXPERIMENTS=("${ALL_EXPERIMENTS[@]}")
fi

mkdir -p "$FIG_DIR" "$LOG_DIR"

declare -A STATUS
declare -A OUTDIR

run_one() {
    local name="$1" dir cmd_env="" budget
    # Per-experiment wall-clock budget (seconds); a stuck experiment is
    # killed and reported as TIMEOUT instead of blocking the whole AE run.
    case "$name" in
        state-partition)         dir="4-state-partition";         budget="${BUDGET_STATE:-21600}" ;;
        dbx1000-cross-warehouse) dir="5-dbx1000-cross-warehouse"; budget="${BUDGET_CROSSWH:-14400}" ;;
        *) echo "Unknown experiment: $name" >&2; STATUS[$name]="unknown"; return 1 ;;
    esac

    local log="$LOG_DIR/$name.log"
    echo ""
    echo "############################################################"
    echo "### [$(date +%H:%M:%S)] Running $name  (budget ${budget}s, log: $log)"
    echo "############################################################"

    (cd "$REPO_ROOT" && env $cmd_env timeout --kill-after=60 "$budget" \
        "./artifact-evaluation/$dir/run.sh") > "$log" 2>&1
    local rc=$?
    if [ "$rc" -eq 0 ]; then
        STATUS[$name]="OK"
    elif [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        STATUS[$name]="TIMEOUT(${budget}s)"
        echo "[run-all][TIMEOUT-ERROR] $name exceeded its ${budget}s budget and was killed" >&2
        # the killed run.sh's EXIT trap may not have fired; clean leftovers
        tmux kill-session -t "${USER}-ae" 2>/dev/null || true
        tmux kill-session -t "${USER}-ipc-ae" 2>/dev/null || true
    elif [ "$rc" -eq 2 ]; then
        STATUS[$name]="FAILED(step-timeouts)"
        echo "[run-all] $name finished but some steps timed out; see $log" >&2
    else
        STATUS[$name]="FAILED(rc=$rc)"
        echo "[run-all] $name FAILED; last 20 log lines:" >&2
        tail -20 "$log" >&2 || true
    fi

    # Locate the experiment's newest out dir and copy its figures
    local out
    out="$(ls -td "$AE_ROOT/$dir"/out/*/ 2>/dev/null | head -1 || true)"
    if [ -n "$out" ]; then
        OUTDIR[$name]="$out"
        if [ -d "$out/figures" ]; then
            mkdir -p "$FIG_DIR/$name"
            cp "$out"/figures/* "$FIG_DIR/$name/" 2>/dev/null || true
        fi
    fi
}

echo "=== One-click AE run: ${EXPERIMENTS[*]} ==="
echo "=== Global preparation ==="
if ! "$AE_ROOT/prepare.sh" > "$LOG_DIR/prepare.log" 2>&1; then
    echo "Global prepare.sh failed; see $LOG_DIR/prepare.log" >&2
    exit 1
fi

for exp in "${EXPERIMENTS[@]}"; do
    run_one "$exp"
done

echo ""
echo "############################################################"
echo "### Summary"
echo "############################################################"
overall=0
for exp in "${EXPERIMENTS[@]}"; do
    printf "%-26s %-22s %s\n" "$exp" "${STATUS[$exp]:-skipped}" "${OUTDIR[$exp]:-}"
    [ "${STATUS[$exp]:-}" = "OK" ] || overall=1
done
echo ""
echo "All figures gathered under: $FIG_DIR"
ls -R "$FIG_DIR" 2>/dev/null || true
exit "$overall"
