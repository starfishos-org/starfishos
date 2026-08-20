#!/usr/bin/env bash
# Keep Bash and Zsh behavior aligned for arrays, word splitting, globs, and regex matches.
if [ -n "${ZSH_VERSION:-}" ]; then
    setopt KSH_ARRAYS SH_WORD_SPLIT NO_NOMATCH BASH_REMATCH
fi
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")/../.." && pwd)"
source "$REPO_ROOT/artifact-evaluation/common.sh"
AE_DIR="$REPO_ROOT/artifact-evaluation/1-ipc-cdf"
TS="${TS:-$(date +%Y%m%d_%H%M%S)}"
OUT_DIR="${OUT_DIR:-$AE_DIR/out/$TS}"
LOG_DIR="${LOG_DIR:-$OUT_DIR/logs}"
CSV_DIR="${CSV_DIR:-$OUT_DIR/csv}"
FIG_DIR="${FIG_DIR:-$OUT_DIR/figures}"
SESSION="${SESSION:-${USER}-ipc-ae}"
NUM_MACHINES=2
# chcore.ini may request 96 vCPUs, but the IPC artifact only needs a small
# guest and large CPU counts have triggered rr_sched budget BUGs during boot.
IPC_CPU_NUM="${IPC_CPU_NUM:-${AE_MICROBENCH_GUEST_CPU_NUM:-12}}"
TIMEOUT="${TIMEOUT:-600}"
INPUT_TIMEOUT="${INPUT_TIMEOUT:-30}"
KEEP_QEMU="${KEEP_QEMU:-0}"
SKIP_BUILD="${SKIP_BUILD:-0}"
# The paper figure's measurement points, one boot each.  plot.py refuses a
# dataset that misses any of them, so a thinned selection has to tell it that
# the gap is deliberate (see --allow-partial below).  Thin this only to
# re-check the boot path or a single mode; a figure needs all six.
PAPER_IPC_MODES="direct_empty direct cross_empty cross cross_empty_4t cross_4t"
IPC_MODES="${IPC_MODES:-$PAPER_IPC_MODES}"
PROJECT_CONFIG="$REPO_ROOT/.config"

# Validate the scope before creating output, editing instrumentation flags, or
# starting a potentially expensive build.
for requested in $IPC_MODES; do
    case " $PAPER_IPC_MODES " in
    *" $requested "*) ;;
    *)
        echo "Unknown IPC mode '$requested'; known modes: $PAPER_IPC_MODES" >&2
        exit 1
        ;;
    esac
done

CLIENT_SRC="$REPO_ROOT/user/system-servers/polling/polling_client_test.c"
SERVER_SRC="$REPO_ROOT/user/system-servers/polling/polling_server.c"
RESP_SRC="$REPO_ROOT/user/system-servers/polling/polling_resp.c"

mkdir -p "$LOG_DIR" "$CSV_DIR" "$FIG_DIR"
echo "[AE] Output directory: $OUT_DIR"

TMP_DIR="$(mktemp -d)"
cp "$CLIENT_SRC" "$TMP_DIR/polling_client_test.c"
cp "$SERVER_SRC" "$TMP_DIR/polling_server.c"
cp "$RESP_SRC" "$TMP_DIR/polling_resp.c"

restore_sources() {
    cp "$TMP_DIR/polling_client_test.c" "$CLIENT_SRC"
    cp "$TMP_DIR/polling_server.c" "$SERVER_SRC"
    cp "$TMP_DIR/polling_resp.c" "$RESP_SRC"
    rm -rf "$TMP_DIR"
}

cleanup() {
    restore_sources
    if [ "$KEEP_QEMU" != "1" ]; then
        ae_stop_tmux_and_reap "$SESSION" || true
    fi
}
trap cleanup EXIT

set_define() {
    local file="$1"
    local flag="$2"
    local value="$3"
    sed -i "s/^#define ${flag} [01]/#define ${flag} ${value}/" "$file"
}

disable_kernel_tests() {
    if [ ! -f "$PROJECT_CONFIG" ]; then
        echo "Missing ChCore config: $PROJECT_CONFIG" >&2
        return 1
    fi

    sed -i 's/^CHCORE_KERNEL_TEST:BOOL=.*/CHCORE_KERNEL_TEST:BOOL=OFF/' \
        "$PROJECT_CONFIG"
    grep -q '^CHCORE_KERNEL_TEST:BOOL=OFF$' "$PROJECT_CONFIG"
    echo "=== CHCORE_KERNEL_TEST=OFF for IPC artifact ==="
}

kernel_tests_disabled_in_image() {
    strings "$REPO_ROOT/build/kernel.img" 2>/dev/null \
        | grep -q 'kernel tests start' && return 1
    return 0
}

wait_for_log_text() {
    local machine="$1"
    local pattern="$2"
    local label="$3"
    local start_line="${4:-1}"
    local logfile="$LOG_DIR/machine${machine}.log"
    local elapsed=0
    while [ "$elapsed" -lt "$TIMEOUT" ]; do
        # Host-side watchdog: covers both machines continuously, so it catches
        # a fault on the machine this call is not watching.
        if ae_watchdog_tripped; then
            echo "Aborting while waiting for $label: $(ae_watchdog_reason)" >&2
            return 1
        fi
        if ! tmux has-session -t "$SESSION" 2>/dev/null; then
            echo "tmux session $SESSION exited while waiting for $label" >&2
            tail -120 "$logfile" >&2 || true
            return 1
        fi
        if ! tmux list-panes -t "$SESSION:$machine" >/dev/null 2>&1; then
            echo "tmux window $SESSION:$machine died while waiting for $label" >&2
            tail -120 "$logfile" >&2 || true
            return 1
        fi
        if tail -n "+$start_line" "$logfile" 2>/dev/null | grep -Eq "$AE_ERROR_PATTERN"; then
            echo "Guest error while waiting for $label" >&2
            tail -120 "$logfile" >&2 || true
            return 1
        fi
        # QEMU's serial output can interleave concurrent guest messages, so
        # capture-pane may split a banner across lines and its scrollback is
        # not a reliable readiness source.  The tee'd serial log is complete.
        if tail -n "+$start_line" "$logfile" 2>/dev/null | grep -q "$pattern"; then
            ae_final_watchdog_health "$label final health" || return 1
            echo "$label"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    echo "Timed out waiting for $label" >&2
    tail -120 "$logfile" >&2 || true
    return 1
}

# Report whether a banner is present without gating on it: guest serial output
# from two cores can interleave mid-token, so absence proves nothing.
note_log_text() {
    local machine="$1"
    local pattern="$2"
    local label="$3"
    local start_line="${4:-1}"
    local logfile="$LOG_DIR/machine${machine}.log"

    if tail -n "+$start_line" "$logfile" 2>/dev/null | grep -q "$pattern"; then
        echo "$label: seen"
    else
        echo "$label: not in the log (serial interleaving); continuing"
    fi
}

cluster_alive() {
    tmux has-session -t "$SESSION" 2>/dev/null \
        && tmux list-panes -t "$SESSION:0" >/dev/null 2>&1 \
        && tmux list-panes -t "$SESSION:1" >/dev/null 2>&1
}

done_count() {
    local logfile="$1"
    grep -c "polling_client: done" "$logfile" 2>/dev/null || true
}

guest_faulted() {
    local logfile="$1"
    local line_offset="$2"

    tail -n "+$line_offset" "$logfile" 2>/dev/null | grep -Eq \
        'BUG: do_page_fault|CMD: /polling_client\.bin'
}

cluster_logs_healthy() {
    ae_final_watchdog_health "IPC mode final health"
}

send_client_command() {
    local machine="$1" command="$2" logfile="$3"
    local prev_sz=-1 cur_sz quiet=0 waited=0 try sent

    if ! cluster_alive; then
        echo "IPC cluster is not alive; cannot send command to machine $machine" >&2
        return 1
    fi

    # Serial output can still be draining when the shell banner appears.
    while [ "$quiet" -lt 2 ] && [ "$waited" -lt 60 ]; do
        cur_sz=$(stat -c%s "$logfile" 2>/dev/null || echo 0)
        if [ "$cur_sz" = "$prev_sz" ]; then
            quiet=$((quiet + 1))
        else
            quiet=0
        fi
        prev_sz="$cur_sz"
        sleep 2
        waited=$((waited + 2))
    done

    for try in 1 2 3; do
        tmux send-keys -t "$SESSION:$machine" "" Enter
        sleep 1
        tmux send-keys -t "$SESSION:$machine" "$command" Enter
        sleep 3
        sent="$(grep -acF "$command" "$logfile" 2>/dev/null || true)"
        if [ "${sent:-0}" -gt 0 ]; then
            return 0
        fi
        echo "Command not echoed on machine $machine (try $try); resending" >&2
    done
    return 1
}

run_client() {
    local machine="$1"
    local mode="$2"
    local command="$3"
    local logfile="$LOG_DIR/machine${machine}.log"
    local before
    local after
    local elapsed=0
    local first_line
    local command_seen=0

    before="$(done_count "$logfile")"
    first_line=$(($(wc -l < "$logfile") + 1))
    echo "=== Running $mode on machine $machine ==="
    send_client_command "$machine" "$command" "$logfile" || {
        echo "Guest shell did not receive command for $mode" >&2
        tail -80 "$logfile" >&2 || true
        return 1
    }

    while [ "$elapsed" -lt "$TIMEOUT" ]; do
        if ae_watchdog_tripped; then
            echo "Aborting $mode: $(ae_watchdog_reason)" >&2
            return 1
        fi
        if [ "$command_seen" = "0" ] &&
           tail -n "+$first_line" "$logfile" 2>/dev/null | grep -Fq "$command"; then
            command_seen=1
        fi
        if [ "$command_seen" = "0" ] && [ "$elapsed" -ge "$INPUT_TIMEOUT" ]; then
            echo "Guest shell did not receive command for $mode" >&2
            tail -80 "$logfile" >&2 || true
            return 1
        fi
        after="$(done_count "$logfile")"
        if [ "$after" -gt "$before" ]; then
            # The completion marker is accepted only after a synchronous
            # all-machine scan, which closes the watchdog polling race.
            cluster_logs_healthy || return 1
            grep "\\[SUMMARY\\]" "$logfile" | tail -2 || true
            return 0
        fi
        if guest_faulted "$logfile" "$first_line"; then
            echo "Guest fault while running $mode" >&2
            tail -120 "$logfile" >&2 || true
            return 1
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done

    echo "Timed out while running $mode" >&2
    tail -80 "$logfile" >&2 || true
    return 1
}

check_global_prepare() {
    ae_check_global_prepare
}

reset_dsm_metadata() {
    echo "=== Resetting DSM metadata before QEMU boot ==="
    ae_ensure_cxlfs_device || return 1
    make clean-dsm-meta
}

stop_cluster() {
    ae_stop_tmux_and_reap "$SESSION"
}

simulate_cmd() {
    local machine="$1"
    local logfile="$2"
    printf "cd '%s' && CPU_NUM=%s MACHINE_NUM=%s ./build/simulate.sh %s 2>&1 | tee -a '%s'" \
        "$REPO_ROOT" "$IPC_CPU_NUM" "$NUM_MACHINES" "$machine" "$logfile"
}

start_cluster() {
    local machine0_log="$LOG_DIR/machine0.log"
    local machine1_log="$LOG_DIR/machine1.log"
    local machine0_start
    local machine1_start

    stop_cluster || return 1
    if ae_has_chcore_qemu; then
        echo "Refusing to boot IPC cluster: leftover ChCore QEMU still running" >&2
        return 1
    fi
    reset_dsm_metadata
    ae_start_log_watchdog "$NUM_MACHINES" "ipc cluster"

    echo "=== Booting two QEMU machines for IPC artifact (cpu=${IPC_CPU_NUM}) ==="
    machine0_start=$(($(wc -l < "$machine0_log") + 1))
    tmux new-session -d -s "$SESSION" -n 0 "$(simulate_cmd 0 "$machine0_log")"
    wait_for_log_text 0 "DSM] machine 0 " "DSM machine 0 joined" "$machine0_start" || return 1

    # Machine 1 must be launched as soon as machine 0 has joined the cluster,
    # not after machine 0 reaches its shell: the kernel join barrier
    # (dsm_wait_for_cluster_cpu_topology) holds every machine at the banner
    # until the whole cluster has joined, so waiting for machine 0's shell
    # first deadlocks the boot -- machine 0 waits for machine 1, which has not
    # been started yet.  Gate on the per-machine join line here and only wait
    # for the shells once both machines are in (same order as
    # 2-sched-notify-latency/run.sh and dsm-scripts/simulate_ncluster.sh).
    machine1_start=$(($(wc -l < "$machine1_log") + 1))
    tmux new-window -t "$SESSION" -n 1 "$(simulate_cmd 1 "$machine1_log")"
    wait_for_log_text 1 "DSM] machine 1 " "DSM machine 1 joined" "$machine1_start" || return 1

    # Kernel malloc tests (when CHCORE_KERNEL_TEST=ON) run before the shell,
    # so the shell banner is the readiness gate for both machines.
    #
    # "User Init: booting polling server" used to gate the wait as an
    # intermediate milestone, and it cannot: the guest's own serial stream
    # interleaves it with lwip's output from another core, byte by byte, so
    # the log holds e.g.
    #
    #   User Init: booting polling serve[lwip] Host at 192.168.0.3 ...
    #   r
    #
    # and no substring match can find it.  Machine 1 lost that race on
    # 2026-08-20 and failed a run whose cluster had in fact come up.  User
    # init launches the polling server before it starts the shell, so the
    # shell banner already implies the milestone; note whether the banner
    # survived, but never fail on it.
    wait_for_log_text 0 "Welcome to ChCore shell" "Machine 0 shell ready" "$machine0_start" || return 1
    wait_for_log_text 1 "Welcome to ChCore shell" "Machine 1 shell ready" "$machine1_start" || return 1
    note_log_text 0 "booting polling server" "Machine 0 polling server banner" "$machine0_start"
    note_log_text 1 "booting polling server" "Machine 1 polling server banner" "$machine1_start"
}

run_mode() {
    local machine="$1"
    local mode="$2"
    local command="$3"

    if ! cluster_alive; then
        echo "IPC cluster is not alive before $mode" >&2
        return 1
    fi
    run_client "$machine" "$mode" "$command"
}

# One boot per measured mode.  A client process that has already exited leaves
# guest state behind (cap-group teardown, IPC connection/notification recycling
# and cross-machine mappings are not fully reclaimed), and this experiment
# reports latency distributions, which is exactly the metric that residual
# state perturbs.  Rebooting is the only reliable reset: every boot runs
# `make clean-dsm-meta`, which re-zeroes the whole shared-memory region.
#
# Both machines are always booted together: the cross modes need machine 0's
# polling server up while machine 1 runs the client, and start_cluster waits
# for each machine's "booting polling server" banner before the shell.
# machine{0,1}.log accumulate across boots ("tee -a"); start_cluster and
# run_client scope every wait to the current boot via line offsets.
run_point() {
    local machine="$1" mode="$2" command="$3" rc=0

    if ! start_cluster; then
        stop_cluster || true
        return 1
    fi
    run_mode "$machine" "$mode" "$command" || rc=$?
    stop_cluster || true
    return "$rc"
}

cd "$REPO_ROOT"

ae_ensure_clean_tmux

check_global_prepare

: > "$LOG_DIR/machine0.log"
: > "$LOG_DIR/machine1.log"

echo "=== Enabling IPC instrumentation for this artifact run ==="
set_define "$CLIENT_SRC" ENABLE_BREAKDOWN 1
set_define "$SERVER_SRC" ENABLE_SRV_TIMING 1
set_define "$RESP_SRC" ENABLE_SRV_TIMING 1
disable_kernel_tests

if [ "$SKIP_BUILD" = "1" ]; then
    if ! kernel_tests_disabled_in_image; then
        echo "SKIP_BUILD=1 but build/kernel.img still contains kernel malloc tests." >&2
        echo "Rebuild once (omit SKIP_BUILD) so CHCORE_KERNEL_TEST=OFF takes effect." >&2
        exit 1
    fi
    echo "=== Skipping build (SKIP_BUILD=1) ==="
else
    ae_build_with_config_restore
    if ! kernel_tests_disabled_in_image; then
        echo "kernel.img still contains kernel tests after rebuild" >&2
        exit 1
    fi
fi

# Each mode is an independent measurement point on a freshly booted cluster.
IPC_MODE_TABLE=(
    "0|direct_empty|polling_client.bin -d -e -t 1 -m direct_empty"
    "0|direct|polling_client.bin -d -t 1 -m direct"
    "1|cross_empty|polling_client.bin -s 0 -e -t 1 -m cross_empty"
    "1|cross|polling_client.bin -s 0 -t 1 -m cross"
    "1|cross_empty_4t|polling_client.bin -s 0 -e -t 4 -m cross_empty_4t"
    "1|cross_4t|polling_client.bin -s 0 -t 4 -m cross_4t"
)

failed=0
ran_count=0
selected_server_modes=()
for entry in "${IPC_MODE_TABLE[@]}"; do
    entry_machine="${entry%%|*}"
    entry_rest="${entry#*|}"
    entry_mode="${entry_rest%%|*}"
    entry_cmd="${entry_rest#*|}"
    case " $IPC_MODES " in
    *" $entry_mode "*) ;;
    *) continue ;;
    esac
    ran_count=$((ran_count + 1))
    if [ "$entry_machine" = "1" ]; then
        selected_server_modes+=("$entry_mode")
    fi
    run_point "$entry_machine" "$entry_mode" "$entry_cmd" || failed=1
done
stop_cluster

if [ "$ran_count" -eq 0 ]; then
    echo "IPC_MODES selected no mode: '$IPC_MODES'" >&2
    exit 1
fi

echo "=== Parsing logs and generating figures ==="
plot_args=(--log-dir "$LOG_DIR" --csv-dir "$CSV_DIR" --fig-dir "$FIG_DIR")
for server_mode in "${selected_server_modes[@]}"; do
    # Server timing blocks do not contain their mode name, so preserve the
    # exact order of the remote modes that produced them.
    plot_args+=(--server-mode "$server_mode")
done
if [ "$ran_count" -ne "${#IPC_MODE_TABLE[@]}" ]; then
    echo "[AE] thinned IPC_MODES; plotting only the measured modes."
    plot_args+=(--allow-partial)
fi
python3 "$AE_DIR/plot.py" "${plot_args[@]}"

echo "Artifact output: $OUT_DIR"
if [ "$failed" -ne 0 ]; then
    echo "One or more IPC modes failed; see $LOG_DIR" >&2
    exit 1
fi
