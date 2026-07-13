#!/usr/bin/env bash
#
# Shared helpers for artifact-evaluation test scripts.
# Source this file from an experiment's run.sh:
#
#   source "$(dirname "${BASH_SOURCE[0]}")/../common.sh"
#
# Provides:
#   ae_check_global_prepare      - verify prepare.sh has been run
#   ae_boot_cluster N            - boot an N-machine cluster in tmux
#   ae_send_command M "cmd"      - send a shell command to machine M
#   ae_wait_in_log M "pat" T     - wait for pattern in machine M's log
#   ae_kill_cluster              - tear down the tmux session
#   ae_set_dsm_var VAR VAL       - edit kernel/dsm_config.cmake
#   ae_set_dotconfig KEY TYPE VAL- edit .config (e.g. CHCORE_KERNEL_TEST BOOL ON)
#   ae_save_build_configs / ae_restore_build_configs
#
# Conventions:
#   AE_SESSION  - tmux session name (default: $USER-ae)
#   AE_LOG_DIR  - where per-machine logs are copied (set by caller)
#   Machine i's live log is $AE_REPO_ROOT/exec_log<i>.log (written by
#   build/simulate.sh itself); ae_boot_cluster removes stale ones first.

AE_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AE_SESSION="${AE_SESSION:-${USER}-ae}"
AE_DSM_CONFIG="$AE_REPO_ROOT/kernel/dsm_config.cmake"
AE_DOTCONFIG="$AE_REPO_ROOT/.config"
AE_CHCORE_INI="$AE_REPO_ROOT/chcore.ini"
AE_BOOT_TIMEOUT="${AE_BOOT_TIMEOUT:-600}"

# ── global environment check ────────────────────────────────────────────────

ae_check_global_prepare() {
    local cxl_file="/dev/shm/ivshmem-$USER"
    local hostfs_file="/dev/shm/ivshmem-hostfs-$USER"
    local doorbell_socket="/tmp/ivshmem-doorbell-$USER"
    local server_pid_file="/tmp/ivshmem-server-$USER.pid"
    local numa_files=(
        "/dev/shm/numa0.0-$USER" "/dev/shm/numa0.1-$USER"
        "/dev/shm/numa1.0-$USER" "/dev/shm/numa1.1-$USER"
        "/dev/shm/numa2.0-$USER" "/dev/shm/numa2.1-$USER"
        "/dev/shm/numa3.0-$USER" "/dev/shm/numa3.1-$USER"
    )
    local f

    echo "=== Checking global AE environment ==="
    for f in "$cxl_file" "$hostfs_file" "$doorbell_socket" "${numa_files[@]}"; do
        if [ ! -e "$f" ]; then
            echo "Missing global AE resource: $f" >&2
            echo "Run ./artifact-evaluation/prepare.sh once before this test." >&2
            return 1
        fi
    done
    if [ ! -f "$server_pid_file" ] || ! ps -p "$(cat "$server_pid_file")" >/dev/null 2>&1; then
        echo "ivshmem doorbell server is not running." >&2
        echo "Run ./artifact-evaluation/prepare.sh before this test." >&2
        return 1
    fi
}

# ── build-config editing ────────────────────────────────────────────────────

# ae_set_dsm_var DSM_CXL_LF_BUDDY ON
ae_set_dsm_var() {
    local var="$1" val="$2"
    sed -i "s/^set(${var} .*)$/set(${var} \"${val}\")/" "$AE_DSM_CONFIG"
    grep -q "^set(${var} \"${val}\")" "$AE_DSM_CONFIG" || {
        echo "Failed to set ${var}=${val} in $AE_DSM_CONFIG" >&2
        return 1
    }
}

# ae_set_dotconfig CHCORE_KERNEL_TEST BOOL ON
ae_set_dotconfig() {
    local key="$1" type="$2" val="$3"
    sed -i "s/^${key}:${type}=.*/${key}:${type}=${val}/" "$AE_DOTCONFIG"
    grep -q "^${key}:${type}=${val}$" "$AE_DOTCONFIG" || {
        echo "Failed to set ${key}=${val} in $AE_DOTCONFIG" >&2
        return 1
    }
}

# ae_set_ini_cpu_num 96
# chcore.ini's cpu_num is passed by chbuild as a -D command-line arg, which
# overrides the .config value — so the ini must be edited too when changing
# the compile-time CPU count.
ae_set_ini_cpu_num() {
    local val="$1"
    sed -i "s/^cpu_num *=.*/cpu_num = ${val}/" "$AE_CHCORE_INI"
    grep -q "^cpu_num = ${val}$" "$AE_CHCORE_INI" || {
        echo "Failed to set cpu_num=${val} in $AE_CHCORE_INI" >&2
        return 1
    }
}

AE_CONFIG_BACKUP_DIR=""

ae_save_build_configs() {
    AE_CONFIG_BACKUP_DIR="$(mktemp -d)"
    cp "$AE_DSM_CONFIG" "$AE_CONFIG_BACKUP_DIR/dsm_config.cmake"
    cp "$AE_DOTCONFIG" "$AE_CONFIG_BACKUP_DIR/dotconfig"
    cp "$AE_CHCORE_INI" "$AE_CONFIG_BACKUP_DIR/chcore.ini"
}

ae_restore_build_configs() {
    if [ -n "$AE_CONFIG_BACKUP_DIR" ] && [ -d "$AE_CONFIG_BACKUP_DIR" ]; then
        cp "$AE_CONFIG_BACKUP_DIR/dsm_config.cmake" "$AE_DSM_CONFIG"
        cp "$AE_CONFIG_BACKUP_DIR/dotconfig" "$AE_DOTCONFIG"
        cp "$AE_CONFIG_BACKUP_DIR/chcore.ini" "$AE_CHCORE_INI"
        rm -rf "$AE_CONFIG_BACKUP_DIR"
        AE_CONFIG_BACKUP_DIR=""
    fi
}

# Build with a hard timeout (a wedged docker/cmake would otherwise hang the
# whole experiment). Override with AE_BUILD_TIMEOUT (seconds).
ae_build() {
    local build_timeout="${AE_BUILD_TIMEOUT:-3600}"
    local log="${AE_LOG_DIR:-/tmp}/build_$$.log"
    echo "=== Building ChCore (timeout ${build_timeout}s) ==="
    (cd "$AE_REPO_ROOT" && timeout "$build_timeout" ./chbuild build) > "$log" 2>&1
    local rc=$?
    if [ "$rc" -eq 124 ]; then
        ae_record_timeout "chbuild build exceeded ${build_timeout}s (log: $log)"
        return 1
    elif [ "$rc" -ne 0 ]; then
        echo "Build failed (rc=$rc); see $log" >&2
        tail -30 "$log" >&2
        return 1
    fi
    echo "Build OK."
}

# ── QEMU cluster management ─────────────────────────────────────────────────

ae_machine_log() {
    echo "$AE_REPO_ROOT/exec_log$1.log"
}

# ── timeout-error accounting ────────────────────────────────────────────────
# Every timeout is recorded here; experiments call ae_finish at the end so a
# run with timeouts still parses its logs but exits non-zero.

AE_TIMEOUT_ERRORS=()

ae_record_timeout() {
    local what="$1"
    AE_TIMEOUT_ERRORS+=("$what")
    echo "[AE][TIMEOUT-ERROR] $what" >&2
}

# ae_finish [exit-on-timeouts]
# Print a timeout summary; exit 2 if any step timed out.
ae_finish() {
    if [ "${#AE_TIMEOUT_ERRORS[@]}" -gt 0 ]; then
        echo "" >&2
        echo "[AE] ${#AE_TIMEOUT_ERRORS[@]} step(s) TIMED OUT in this run:" >&2
        local t
        for t in "${AE_TIMEOUT_ERRORS[@]}"; do
            echo "[AE]   - $t" >&2
        done
        exit 2
    fi
}

# ae_wait_in_log <machine> <pattern> <timeout-s> <label>
ae_wait_in_log() {
    local machine="$1" pattern="$2" timeout="$3" label="$4"
    local logfile
    logfile="$(ae_machine_log "$machine")"
    local elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        if grep -q "$pattern" "$logfile" 2>/dev/null; then
            [ -n "$label" ] && echo "$label"
            return 0
        fi
        if ! tmux has-session -t "$AE_SESSION" 2>/dev/null; then
            echo "tmux session $AE_SESSION died while waiting for: $label" >&2
            tail -60 "$logfile" >&2 || true
            return 1
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    ae_record_timeout "$label (pattern '$pattern' not seen within ${timeout}s in $logfile)"
    tail -60 "$logfile" >&2 || true
    return 1
}

# ae_boot_cluster <num_machines> [cpu_num]
# Boots machines 0..N-1 in tmux windows, waits for DSM join + shell on all.
# Extra simulate.sh env (e.g. DRAM_SIZE=24G) can be passed via AE_EXTRA_ENV.
ae_boot_cluster() {
    local n="$1"
    local cpu_num="${2:-}"
    local i env_prefix="${AE_EXTRA_ENV:+$AE_EXTRA_ENV }"

    [ -n "$cpu_num" ] && env_prefix="${env_prefix}CPU_NUM=$cpu_num "

    ae_kill_cluster

    echo "=== Resetting DSM metadata ==="
    (cd "$AE_REPO_ROOT" && make clean-dsm-meta >/dev/null)

    for i in $(seq 0 $((n - 1))); do
        rm -f "$(ae_machine_log "$i")"
    done
    rm -f "$AE_REPO_ROOT/exec_log.log"

    echo "=== Booting $n machine(s) ==="
    tmux new-session -d -s "$AE_SESSION" -n 0 \
        "cd '$AE_REPO_ROOT' && ${env_prefix}MACHINE_NUM=$n ./build/simulate.sh 0"
    ae_wait_in_log 0 "DSM] machine 0 " "$AE_BOOT_TIMEOUT" "DSM machine 0 joined" || return 1

    for i in $(seq 1 $((n - 1))); do
        tmux new-window -t "$AE_SESSION" -n "$i" \
            "cd '$AE_REPO_ROOT' && ${env_prefix}MACHINE_NUM=$n ./build/simulate.sh $i"
        ae_wait_in_log "$i" "DSM] machine $i " "$AE_BOOT_TIMEOUT" "DSM machine $i joined" || return 1
    done

    for i in $(seq 0 $((n - 1))); do
        ae_wait_in_log "$i" "Welcome to ChCore shell!" "$AE_BOOT_TIMEOUT" "machine $i shell ready" || return 1
    done
}

# ae_send_command <machine> <command>
# A bare Enter is sent first: right after boot the guest tty may still hold
# terminal escape-sequence responses (e.g. "1;2c" from a Device Attributes
# query); submitting them as a bogus command flushes the input line so the
# real command lands clean.
ae_send_command() {
    local machine="$1" cmd="$2"
    tmux send-keys -t "$AE_SESSION:$machine" "" Enter
    sleep 1
    tmux send-keys -t "$AE_SESSION:$machine" "$cmd" Enter
}

ae_kill_cluster() {
    if tmux has-session -t "$AE_SESSION" 2>/dev/null; then
        tmux kill-session -t "$AE_SESSION"
    fi
}

# ae_archive_logs <num_machines> <dest_dir> [suffix]
ae_archive_logs() {
    local n="$1" dest="$2" suffix="${3:-}"
    local i src
    mkdir -p "$dest"
    for i in $(seq 0 $((n - 1))); do
        src="$(ae_machine_log "$i")"
        [ -f "$src" ] && cp "$src" "$dest/machine${i}${suffix}.log"
    done
}
