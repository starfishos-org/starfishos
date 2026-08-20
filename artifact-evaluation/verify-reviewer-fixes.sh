#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi
#
# Re-run only the two experiments an artifact reviewer reported as broken on
# 2026-08-20, and check the reported symptom is gone:
#
#   3-memory-allocator  the build died on the first configuration
#                       (llfree_cr_on).  attach_buddy_for_one_mem_pool() was
#                       compiled even with DSM_CXL_LF_BUDDY=ON, where nothing
#                       calls it, so -Werror=unused-function failed both LLFree
#                       configurations and only the Buddy arm ever built.
#   1-ipc-cdf           the run never terminated.  start_cluster waited for
#                       machine 0's shell before booting machine 1, but the
#                       kernel join barrier holds machine 0 until the whole
#                       cluster has joined.
#
# The two full experiments are the authority on the figures; this script only
# answers "does the thing that failed still fail". Quick mode is the cheap
# version of that question:
#
#   quick (default)  3: build all three configurations, run none  (~6 builds)
#                    1: boot the two-machine cluster and measure one mode
#   full             3: the paper sweep       1: all six measurement points
#
# Usage:
#   ./artifact-evaluation/verify-reviewer-fixes.sh [--quick | --full]
#                                                  [--only 1 | --only 3]
#                                                  [--keep-going]
#
set -euo pipefail

AE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")" && pwd)"
REPO_ROOT="$(cd "$AE_ROOT/.." && pwd)"
TS="${TS:-$(date +%Y%m%d_%H%M%S)}"
# Runtime logs belong under log/<test-name>/, never in the repo root.
LOG_ROOT="${LOG_ROOT:-$REPO_ROOT/log/verify-reviewer-fixes/$TS}"

MODE="quick"
ONLY=""
KEEP_GOING=0
PAPER_IPC_MODES="direct_empty direct cross_empty cross cross_empty_4t cross_4t"
PAPER_USER_BENCH_THREADS="1 2 4 8 16 32 64 96"
PAPER_ALLOCATOR_CPU_NUM=96

while [ $# -gt 0 ]; do
    case "$1" in
    --quick) MODE="quick" ;;
    --full) MODE="full" ;;
    --only)
        shift
        ONLY="${1:-}"
        case "$ONLY" in
        1 | 3) ;;
        *)
            echo "--only takes 1 or 3" >&2
            exit 2
            ;;
        esac
        ;;
    --keep-going) KEEP_GOING=1 ;;
    -h | --help)
        cat <<'USAGE'
Re-run only the two experiments an artifact reviewer reported as broken on
2026-08-20, and check the reported symptom is gone.

  --quick   (default) 3: build all three allocator configurations, run none
                      1: boot the two-machine cluster, measure one mode
  --full              3: the paper sweep    1: all six measurement points
  --only 1|3          run just that experiment
  --keep-going        run 1-ipc-cdf even if 3-memory-allocator failed

Logs go to log/verify-reviewer-fixes/<timestamp>/; exit status is 0 only if
every selected check passed.
USAGE
        exit 0
        ;;
    *)
        echo "Unknown argument: $1" >&2
        exit 2
        ;;
    esac
    shift
done

RESULTS=""
FAILED=0

record() {
    RESULTS="$RESULTS
$1"
}

# The AE runners leave tmux sessions and QEMU behind on purpose so a failure
# can be inspected. Nothing here inspects them, and the next experiment needs
# the session name, the CPUs and the shared memory back.
cleanup_between() {
    local session
    for session in "$USER-ae" "$USER-ipc-ae" "$USER-qemu"; do
        tmux kill-session -t "$session" 2>/dev/null || true
    done
    # tmux occasionally fails to reap a pane's QEMU; report rather than
    # pkill, which in a non-interactive shell can take out the caller's own
    # process group.
    if ps -eo cmd | grep -q '[q]emu-6.2-system-x86_64'; then
        echo "[verify] WARNING: QEMU still running after cleanup:" >&2
        ps -eo pid,etime,cmd | grep '[q]emu-6.2-system-x86_64' >&2 || true
    fi
}

# simulate.sh writes exec_log<N>.log into the repo root while a guest runs.
collect_root_logs() {
    local dest="$1"
    local f
    for f in "$REPO_ROOT"/exec_log*.log; do
        [ -e "$f" ] || continue
        mkdir -p "$dest"
        mv "$f" "$dest/"
    done
}

# Newest out/<timestamp>/ directory of an experiment, so the checks below read
# the run that just happened rather than an older one.
latest_out() {
    ls -1dt "$1"/out/*/ 2>/dev/null | head -1
}

run_experiment_3() {
    local log="$LOG_ROOT/3-memory-allocator.log"
    local rc=0

    echo "[verify] === 3-memory-allocator ($MODE) ==="
    if [ "$MODE" = "quick" ]; then
        echo "[verify] building llfree_cr_on, llfree_cr_off, buddy_cr_off; no boots"
        BUILD_ONLY=1 "$AE_ROOT/3-memory-allocator/run.sh" >"$log" 2>&1 || rc=$?
    else
        BUILD_ONLY=0 NRUNS=1 RUN_OFFSET=0 \
            USER_BENCH_THREADS="$PAPER_USER_BENCH_THREADS" \
            CPU_NUM="$PAPER_ALLOCATOR_CPU_NUM" \
            "$AE_ROOT/3-memory-allocator/run.sh" >"$log" 2>&1 || rc=$?
    fi
    collect_root_logs "$LOG_ROOT/3-memory-allocator-guest"
    cleanup_between

    if [ "$rc" -ne 0 ]; then
        record "FAIL  3-memory-allocator: run.sh exited $rc (see $log)"
        # Match the -Werror form only: every build emits benign
        # [-Wunused-function] warnings from the demo sources.
        if grep -q 'Werror=unused-function' "$log"; then
            echo "[verify] the original -Werror=unused-function failure is back:" >&2
            grep -n 'unused-function' "$log" | head -5 >&2
        else
            tail -20 "$log" >&2
        fi
        return 1
    fi

    # The reviewer's failure was the first configuration, so "it built" is only
    # an answer if all three were actually configured and built.
    local label built=0
    for label in llfree_cr_on llfree_cr_off buddy_cr_off; do
        if grep -q "=== Configuring $label:" "$log"; then
            built=$((built + 1))
        else
            record "FAIL  3-memory-allocator: configuration $label never ran (see $log)"
            return 1
        fi
    done

    if [ "$MODE" = "quick" ]; then
        record "PASS  3-memory-allocator: $built/3 configurations built (build-only)"
        return 0
    fi

    local out csv config
    out="$(latest_out "$AE_ROOT/3-memory-allocator")"
    csv="$out/csv/allocator_results.csv"
    if [ ! -s "$csv" ]; then
        record "FAIL  3-memory-allocator: no $csv"
        return 1
    fi
    for config in "LLFree+CR" "LLFree" "Buddy"; do
        if ! awk -F, -v c="$config" 'NR>1 && $1==c {found=1} END {exit !found}' "$csv"; then
            record "FAIL  3-memory-allocator: no rows for $config in $csv"
            return 1
        fi
    done
    record "PASS  3-memory-allocator: all three configurations measured ($csv)"
    return 0
}

run_experiment_1() {
    local log="$LOG_ROOT/1-ipc-cdf.log"
    local rc=0

    echo "[verify] === 1-ipc-cdf ($MODE) ==="
    if [ "$MODE" = "quick" ]; then
        # cross is the mode that needs machine 0's polling server while
        # machine 1 drives the client, so it exercises exactly the boot
        # ordering that used to deadlock.
        echo "[verify] one boot, mode 'cross' only"
        IPC_MODES="cross" "$AE_ROOT/1-ipc-cdf/run.sh" >"$log" 2>&1 || rc=$?
    else
        IPC_MODES="$PAPER_IPC_MODES" \
            "$AE_ROOT/1-ipc-cdf/run.sh" >"$log" 2>&1 || rc=$?
    fi
    collect_root_logs "$LOG_ROOT/1-ipc-cdf-guest"
    cleanup_between

    if [ "$rc" -ne 0 ]; then
        record "FAIL  1-ipc-cdf: run.sh exited $rc (see $log)"
        tail -20 "$log" >&2
        return 1
    fi

    local out
    out="$(latest_out "$AE_ROOT/1-ipc-cdf")"
    if [ -z "$out" ]; then
        record "FAIL  1-ipc-cdf: no output directory"
        return 1
    fi

    # The deadlock was machine 1 never being launched: machine 0 sat at the
    # join barrier and no shell ever appeared. Check both machines joined and
    # both reached a shell, not just that the script exited 0.
    local m0="$out/logs/machine0.log" m1="$out/logs/machine1.log"
    local missing=""
    grep -q 'DSM] machine 0 ' "$m0" 2>/dev/null || missing="$missing machine0-join"
    grep -q 'DSM] machine 1 ' "$m1" 2>/dev/null || missing="$missing machine1-join"
    grep -q 'Welcome to ChCore shell' "$m0" 2>/dev/null || missing="$missing machine0-shell"
    grep -q 'Welcome to ChCore shell' "$m1" 2>/dev/null || missing="$missing machine1-shell"
    if [ -n "$missing" ]; then
        record "FAIL  1-ipc-cdf: cluster never came up ($missing) in $out/logs"
        return 1
    fi

    local cdf="$out/csv/cdf.csv" expected_modes csv_mode
    if [ "$MODE" = "quick" ]; then
        expected_modes="cross"
    else
        expected_modes="$PAPER_IPC_MODES"
    fi
    for csv_mode in $expected_modes; do
        if ! awk -F, -v mode="$csv_mode" \
                'NR > 1 && $2 == mode { found=1 } END { exit !found }' \
                "$cdf" 2>/dev/null; then
            record "FAIL  1-ipc-cdf: no latency samples for $csv_mode in $cdf"
            return 1
        fi
    done
    if [ "$MODE" = "full" ] && [ ! -s "$out/figures/ipc_cdf.png" ]; then
        record "FAIL  1-ipc-cdf: no figure at $out/figures/ipc_cdf.png"
        return 1
    fi
    record "PASS  1-ipc-cdf: both machines booted and the run completed ($out)"
    return 0
}

cd "$REPO_ROOT"

# Fail before the first build rather than half an hour in, and before any
# output directory is created.
# shellcheck source=common.sh
source "$AE_ROOT/common.sh"
# Quick allocator-only verification performs builds but boots no guest. Every
# other selection includes a runtime experiment and needs the prepared shared
# memory devices and doorbell server.
if [ "$MODE" = "full" ] || [ "$ONLY" != "3" ]; then
    ae_check_global_prepare
fi

mkdir -p "$LOG_ROOT"
echo "[verify] mode=$MODE logs=$LOG_ROOT"

SKIP_REST=0
if [ "$ONLY" != "1" ]; then
    run_experiment_3 || FAILED=1
    # A broken build is also the reason 1-ipc-cdf would fail, so stopping here
    # keeps the summary honest -- unless the caller asked for both anyway.
    if [ "$FAILED" -ne 0 ] && [ "$KEEP_GOING" -ne 1 ] \
            && [ "$ONLY" != "3" ]; then
        SKIP_REST=1
        record "SKIP  1-ipc-cdf: 3-memory-allocator failed first (--keep-going to run anyway)"
    fi
fi

if [ "$ONLY" != "3" ] && [ "$SKIP_REST" -eq 0 ]; then
    run_experiment_1 || FAILED=1
fi

echo
echo "[verify] summary:$RESULTS"
echo "[verify] logs: $LOG_ROOT"
if [ "$FAILED" -ne 0 ]; then
    exit 1
fi
