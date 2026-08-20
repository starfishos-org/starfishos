#!/bin/bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi
# Unified N-cluster launcher for DSM simulation.
#
# Interactive mode (launch N QEMU instances and attach tmux):
#   ./dsm-scripts/simulate_ncluster.sh <N> [--build]
#
# Automated test mode (launch, run command, wait for result):
#   ./dsm-scripts/simulate_ncluster.sh <N> <logname> "<cmd>" "<expected_pattern>" [--build] [--timeout=SEC]
#
# Examples:
#   ./dsm-scripts/simulate_ncluster.sh 2
#   ./dsm-scripts/simulate_ncluster.sh 2 mm "source run_matrix_multiply.sh" "matrix multiply finished"
#   ./dsm-scripts/simulate_ncluster.sh 2 graph "pagerank /host/twitter-2010.bin 41652230 50 2" "exec_time=" --timeout=600
#
# CPU_NUM env var (default 12) is forwarded to each machine's simulate.sh,
# same as the standalone simulate_Nclusters.sh scripts.

set -e

# ---- parse arguments ----
N=""
LOGNAME=""
CMD=""
EXPECTED=""
BUILD=false
TIMEOUT=120
# Seconds to wait for a machine's shell before giving up. A guest that runs
# work during boot -- CHCORE_KERNEL_TEST=ON runs the whole kernel allocator
# suite before user init starts the shell -- legitimately needs more than the
# default, and the caller is the one that knows how long its workload takes.
SHELL_TIMEOUT="${SIM_SHELL_TIMEOUT:-120}"

positional=()
for arg in "$@"; do
    case "$arg" in
        --build)    BUILD=true ;;
        --timeout=*) TIMEOUT="${arg#--timeout=}" ;;
        *)          positional+=("$arg") ;;
    esac
done

N="${positional[0]:-}"
LOGNAME="${positional[1]:-}"
CMD="${positional[2]:-}"
EXPECTED="${positional[3]:-}"

if [[ -z "$N" ]] || ! [[ "$N" =~ ^[1-8]$ ]]; then
    echo "Usage: $0 <N> [logname] [cmd] [expected_pattern] [--build] [--timeout=SEC]"
    echo "  N: number of QEMU instances (1-8)"
    exit 1
fi

SESSION="$USER-qemu"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_DIR"

# ---- helper ----
# Abort as soon as the host watchdog reports a guest failure, instead of
# waiting out the remaining timeout.
watchdog_abort() {
    local what="$1"
    echo ""
    echo "ABORTED: $(sim_watchdog_reason)"
    echo "  while: $what"
    echo "Logs: $(for ((i=0; i<N; i++)); do echo -n "exec_log${i}.log "; done)"
    # Same as the timeout path: leave tmux/QEMU up for inspection.
    echo "Attach: tmux a -t $SESSION"
    exit 1
}

wait_for_pattern() {
    local file=$1
    local pattern=$2
    local timeout=$3
    local label=$4
    echo -n "  Waiting for $label..."
    for ((t=0; t<timeout; t++)); do
        if sim_watchdog_tripped; then
            echo " FAILED"
            watchdog_abort "$label"
        fi
        if grep -q "$pattern" "$file" 2>/dev/null; then
            echo " OK (${t}s)"
            return 0
        fi
        sleep 1
    done
    echo " TIMEOUT (${timeout}s)"
    return 1
}

# ---- optional build ----
if [[ "$BUILD" == "true" ]]; then
    ./scripts/chbuild-with-fallback.sh build
fi

# ---- cleanup ----
# shellcheck source=../artifact-evaluation/common.sh
source "$REPO_DIR/artifact-evaluation/common.sh"
ae_ensure_clean_tmux

# Never prompt for sudo in an automated run. Set AE_DROP_CACHES=0 to skip
# sync/cache dropping on shared hosts.
ae_drop_host_caches

# ---- prepare ----
"$SCRIPT_DIR/start_ivshmem_server.sh"
# config_memdev.sh cxl also refreshes CXLFS when the ramdisk build changed.
"$SCRIPT_DIR/config_memdev.sh" cxl
sleep 3

# ---- clear old logs ----
for ((i=0; i<N; i++)); do rm -f "exec_log${i}.log"; done
rm -f exec_log.log

# ---- host-side log watchdog ----
# Runs on the host (not in QEMU) for the whole run and tails every machine's
# exec_log; on the first fatal guest signature it writes WATCHDOG_FLAG, which
# the wait loops above check every second so the run aborts right away.
# SIM_LOG_WATCHDOG=0 disables it.
WATCHDOG_DIR="$REPO_DIR/logs/watchdog"
WATCHDOG_FLAG="$WATCHDOG_DIR/simulate_ncluster.flag"
WATCHDOG_PID=""

sim_watchdog_tripped() {
    [ "${SIM_LOG_WATCHDOG:-1}" = "1" ] && [ -f "$WATCHDOG_FLAG" ]
}

sim_watchdog_reason() {
    local machine line log
    [ -f "$WATCHDOG_FLAG" ] || { printf 'host watchdog: unknown failure'; return 0; }
    machine="$(sed -n 's/^machine=//p' "$WATCHDOG_FLAG" | head -1)"
    log="$(sed -n 's/^log=//p' "$WATCHDOG_FLAG" | head -1)"
    line="$(sed -n 's/^line=//p' "$WATCHDOG_FLAG" | head -1)"
    printf 'host watchdog: machine %s -> %s (log: %s)' \
        "${machine:-?}" "${line:-unknown}" "${log:-?}"
}

sim_cluster_healthy() {
    local i line

    [ "${SIM_LOG_WATCHDOG:-1}" = "1" ] || return 0
    if sim_watchdog_tripped; then
        return 1
    fi

    # Close the race where the expected marker and a fatal line arrive before
    # the host watchdog's next poll.  Success is valid only after every guest
    # log has passed a synchronous final scan.
    for ((i=0; i<N; i++)); do
        line="$(_ae_error_grep "exec_log${i}.log" || true)"
        if [ -n "$line" ]; then
            mkdir -p "$WATCHDOG_DIR"
            {
                printf 'label=simulate_ncluster final scan\n'
                printf 'machine=%s\n' "$i"
                printf 'log=%s\n' "$REPO_DIR/exec_log${i}.log"
                printf 'matched=final synchronous scan\n'
                printf 'line=%s\n' "$line"
            } > "$WATCHDOG_FLAG"
            return 1
        fi
    done
    return 0
}

stop_watchdog() {
    if [ -n "$WATCHDOG_PID" ]; then
        kill "$WATCHDOG_PID" 2>/dev/null || true
        wait "$WATCHDOG_PID" 2>/dev/null || true
        WATCHDOG_PID=""
    fi
}
trap stop_watchdog EXIT

# A plain single-instance interactive run keeps QEMU in the foreground for the
# user to inspect, so there is nothing to abort; everything else is watched.
if [ "${SIM_LOG_WATCHDOG:-1}" = "1" ] && ! [[ $N -eq 1 && -z "$CMD" ]]; then
    mkdir -p "$WATCHDOG_DIR"
    rm -f "$WATCHDOG_FLAG"
    python3 "$SCRIPT_DIR/log_watchdog.py" \
        --log-dir "$REPO_DIR" \
        --count "$N" \
        --flag-file "$WATCHDOG_FLAG" \
        --status-log "$WATCHDOG_DIR/simulate_ncluster.log" \
        --pattern "$AE_ERROR_PATTERN" \
        --label "simulate_ncluster ${N}x${LOGNAME:+ $LOGNAME}" &
    WATCHDOG_PID=$!
fi

# tmux windows do not inherit this shell's environment reliably, so forward the
# host NUMA binding knobs (see scripts/qemu/qemu_wrapper.sh) explicitly.
NUMA_ENV="CHCORE_QEMU_NUMA_BIND=${CHCORE_QEMU_NUMA_BIND:-0}"
if [ -n "${CHCORE_QEMU_NUMA_NODES:-}" ]; then
    NUMA_ENV="$NUMA_ENV CHCORE_QEMU_NUMA_NODES=${CHCORE_QEMU_NUMA_NODES}"
fi

RUN_CMD="MACHINE_NUM=$N CPU_NUM=\${CPU_NUM:-12} $NUMA_ENV ./build/simulate.sh"

# ======== Single instance, interactive (no tmux) ========
if [[ $N -eq 1 && -z "$CMD" ]]; then
    MACHINE_NUM=1 CPU_NUM=${CPU_NUM:-12} ./build/simulate.sh 0 | tee exec_log.log
    "$SCRIPT_DIR/kill_ivshmem_server.sh"
    exit 0
fi

# ======== Automated test mode ========
if [[ -n "$CMD" ]]; then
    echo "=== Launch $N QEMU instances (automated) ==="
    tmux new-session -d -s "$SESSION" -n vm0 "$RUN_CMD 0 | tee exec_log0.log"
    # Gate the peers on machine 0's DSM join banner, NOT on its shell prompt.
    # Machine 0 prints the banner once it has published dsm_meta (which is what
    # the peers need before they start), and then blocks in
    # dsm_wait_for_cluster_cpu_topology() until every machine has joined -- so
    # for N > 1 it never reaches a shell until the peers are already running.
    wait_for_pattern "exec_log0.log" "DSM] machine 0 " 120 "machine 0 DSM metadata ready" || {
        echo "FAILED: machine 0 did not publish DSM metadata"
        tail -10 "exec_log0.log" 2>/dev/null
        exit 1
    }
    # Serialize on each peer's own join banner rather than a fixed sleep, the
    # same way artifact-evaluation/common.sh:_ae_boot_cluster_once does: a
    # machine prints its banner and then parks in the barrier, so this is a
    # real synchronization point and it names the machine that failed.
    echo "=== Wait for DSM join ==="
    for ((i=1; i<N; i++)); do
        tmux new-window -t "$SESSION" -n "vm${i}" "$RUN_CMD $i | tee exec_log${i}.log"
        wait_for_pattern "exec_log${i}.log" "DSM] machine $i " 180 "machine $i DSM join" || {
            echo "FAILED: machine $i did not join DSM"
            tail -10 "exec_log${i}.log" 2>/dev/null
            exit 1
        }
    done

    # Wait for shell ready (only reachable once the whole cluster has joined)
    for ((i=0; i<N; i++)); do
        wait_for_pattern "exec_log${i}.log" "Welcome to ChCore shell!" "$SHELL_TIMEOUT" "machine $i shell" || {
            echo "FAILED: shell not ready on machine $i"
            exit 1
        }
    done

    # Send command
    echo "=== Send: $CMD ==="
    tmux send-keys -t "$SESSION:vm0" "$CMD" Enter

    # Wait for expected pattern
    echo -n "=== Waiting for '$EXPECTED'..."
    for ((t=0; t<TIMEOUT; t++)); do
        if sim_watchdog_tripped; then
            echo " FAILED"
            echo "----- Last 40 lines of exec_log0.log -----"
            tail -40 exec_log0.log 2>/dev/null || true
            watchdog_abort "waiting for '$EXPECTED'"
        fi
        if grep -q "$EXPECTED" exec_log0.log 2>/dev/null; then
            echo " OK!"
            break
        fi
        sleep 1
    done

    echo ""
    echo "========================================="
    rc=0
    if grep -q "$EXPECTED" exec_log0.log 2>/dev/null \
       && sim_cluster_healthy; then
        echo "SUCCESS"
        echo "----- Key output -----"
        grep -E "$EXPECTED" exec_log0.log 2>/dev/null || true
        if [[ -n "$LOGNAME" ]]; then
            grep -E "$EXPECTED" exec_log0.log 2>/dev/null >> "$LOGNAME" || true
        fi
    else
        echo "FAILED or TIMEOUT"
        echo "----- Last 40 lines of exec_log0.log -----"
        tail -40 exec_log0.log 2>/dev/null
        rc=1
    fi
    echo "========================================="
    echo "Logs: $(for ((i=0; i<N; i++)); do echo -n "exec_log${i}.log "; done)"
    echo "Attach: tmux a -t $SESSION"
    # Leave tmux/QEMU up for inspection (same as before); propagate failure
    # so Makefile targets like run-mm-test do not treat timeouts as success.
    exit "$rc"
fi

# ======== Interactive mode (tmux panes) ========
echo "=== Launch $N QEMU instances (interactive) ==="
tmux new-session -d -s "$SESSION" -n window0 "$RUN_CMD 0 | tee exec_log0.log"
# Same reason as the automated path: machine 0 blocks in
# dsm_wait_for_cluster_cpu_topology() until the peers join, so waiting for its
# shell here would deadlock.  The join banner already means dsm_meta is ready.
wait_for_pattern "exec_log0.log" "DSM] machine 0 " 120 "machine 0 DSM metadata ready" || {
    echo "FAILED: machine 0 did not publish DSM metadata"
    tail -10 "exec_log0.log" 2>/dev/null
    exit 1
}
for ((i=1; i<N; i++)); do
    tmux split-window -t "$SESSION:window0" "$RUN_CMD $i | tee exec_log${i}.log"
    tmux select-layout -t "$SESSION:window0" tiled
    wait_for_pattern "exec_log${i}.log" "DSM] machine $i " 180 "machine $i DSM join" || {
        echo "FAILED: machine $i did not join DSM"
        tail -10 "exec_log${i}.log" 2>/dev/null
        exit 1
    }
done

# The cluster is now handed over to the user, who watches the panes directly;
# a background watchdog would only scribble over the attached session.
stop_watchdog

tmux select-pane -t "$SESSION:window0.0"
tmux attach -t "$SESSION"

"$SCRIPT_DIR/kill_ivshmem_server.sh"
