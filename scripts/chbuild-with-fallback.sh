#!/usr/bin/env bash
# Run one or more chbuild steps; on any failure fall back to distclean+defconfig+build.
#
# Usage:
#   ./scripts/chbuild-with-fallback.sh build
#   ./scripts/chbuild-with-fallback.sh clean build
#   ./scripts/chbuild-with-fallback.sh --no-fallback build   # chbuild only
#
# Environment:
#   CHBUILD_TIMEOUT   wall-clock limit per step (default: 3600)
#   CHBUILD_LOG       if set, capture chbuild output to this file
#   CHBUILD_QUIET     if 1, do not print chbuild output on the terminal
#   CHBUILD_PROGRESS  if 1, show live build status on one updating line (default 1)
#   CHBUILD_LOCAL     if 1, run ./chbuild --local (skip docker)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

NO_FALLBACK=0
if [ "${1:-}" = "--no-fallback" ]; then
    NO_FALLBACK=1
    shift
fi

if [ "$#" -eq 0 ]; then
    set -- build
fi

TIMEOUT="${CHBUILD_TIMEOUT:-3600}"
LOG="${CHBUILD_LOG:-}"
PROGRESS="${CHBUILD_PROGRESS:-1}"
LOCAL="${CHBUILD_LOCAL:-0}"

_chbuild=(./chbuild)
if [ "$LOCAL" = "1" ]; then
    _chbuild=(./chbuild --local)
fi

_strip_ansi() {
    sed 's/\x1b\[[0-9;]*m//g'
}

_run_chbuild_invocation() {
    local step="$1"
    if [ "$LOCAL" = "1" ]; then
        timeout "$TIMEOUT" "${_chbuild[@]}" "$step"
        return $?
    fi
    if command -v script >/dev/null 2>&1; then
        # chbuild defaults to docker; without a tty the docker CLI can hang after
        # the build finishes when stdout is piped for progress logging.
        script -qfec \
            "timeout $TIMEOUT $(printf '%q ' "${_chbuild[@]}")$(printf '%q' "$step")" \
            /dev/null
        return $?
    fi
    timeout "$TIMEOUT" "${_chbuild[@]}" "$step"
}

_show_progress_line() {
    local clean="$1"
    local tty_dev
    tty_dev="$(tty 2>/dev/null || true)"
    if [ -n "$tty_dev" ] && [ -w "$tty_dev" ]; then
        printf '\r\033[2K%s' "$clean" >"$tty_dev"
    else
        printf '\r\033[2K%s' "$clean" >&2
    fi
}

_run_step_with_inline_progress() {
    local step="$1"
    local logfile="${LOG:-/tmp/chbuild_$$.log}"
    local line clean rc build_pid

    mkdir -p "$(dirname "$logfile")"
    : >"$logfile"

    set +e
    _run_chbuild_invocation "$step" >>"$logfile" 2>&1 &
    build_pid=$!

    if command -v tail >/dev/null 2>&1 && tail --help 2>&1 | grep -q -- '--pid'; then
        tail -f --pid="$build_pid" -n 0 "$logfile" 2>/dev/null | while IFS= read -r line || [ -n "$line" ]; do
            clean="$(printf '%s' "$line" | _strip_ansi)"
            if [ "${#clean}" -gt 120 ]; then
                clean="...${clean: -117}"
            fi
            _show_progress_line "$clean"
        done
    else
        while kill -0 "$build_pid" 2>/dev/null; do
            line="$(tail -n 1 "$logfile" 2>/dev/null || true)"
            if [ -n "$line" ]; then
                clean="$(printf '%s' "$line" | _strip_ansi)"
                if [ "${#clean}" -gt 120 ]; then
                    clean="...${clean: -117}"
                fi
                _show_progress_line "$clean"
            fi
            sleep 0.5
        done
    fi

    wait "$build_pid"
    rc=$?
    set -e
    tty_dev="$(tty 2>/dev/null || true)"
    if [ -n "$tty_dev" ] && [ -w "$tty_dev" ]; then
        printf '\n' >"$tty_dev"
    else
        printf '\n' >&2
    fi
    return "$rc"
}

_run_chbuild_step() {
    local step="$1"

    if [ -n "$LOG" ] && [ "${CHBUILD_QUIET:-0}" = "1" ]; then
        if ! _run_chbuild_invocation "$step" >>"$LOG" 2>&1; then
            return $?
        fi
        return 0
    fi

    if [ "$PROGRESS" = "1" ]; then
        if ! _run_step_with_inline_progress "$step"; then
            return $?
        fi
        return 0
    fi

    if [ -n "$LOG" ]; then
        if ! _run_chbuild_invocation "$step" 2>&1 | tee "$LOG"; then
            return $?
        fi
        return 0
    fi

    if ! _run_chbuild_invocation "$step"; then
        return $?
    fi
    return 0
}

_run_chbuild_steps() {
    local step
    for step in "$@"; do
        if ! _run_chbuild_step "$step"; then
            return $?
        fi
    done
    return 0
}

_emit_log_tail() {
    if [ -n "$LOG" ] && [ -f "$LOG" ]; then
        tail -30 "$LOG" >&2 || true
    fi
}

if _run_chbuild_steps "$@"; then
    exit 0
fi

rc=$?
_emit_log_tail

if [ "$NO_FALLBACK" = "1" ]; then
    echo "=== chbuild failed (rc=$rc); no fallback (--no-fallback) ===" >&2
    exit "$rc"
fi

echo "=== chbuild failed (rc=$rc); falling back to distclean + defconfig + build ===" >&2
if [ -n "$LOG" ]; then
    export CHBUILD_LOG="${LOG%.log}_quickbuild.log"
fi
exec "$REPO_ROOT/scripts/chbuild-with-fallback.sh" --no-fallback distclean defconfig x86_64 build
