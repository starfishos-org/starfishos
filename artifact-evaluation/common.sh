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
# Appends the entry if the key is not present yet (e.g. a chcore_config added
# after .config was generated).
ae_set_dotconfig() {
    local key="$1" type="$2" val="$3"
    if grep -q "^${key}:${type}=" "$AE_DOTCONFIG"; then
        sed -i "s/^${key}:${type}=.*/${key}:${type}=${val}/" "$AE_DOTCONFIG"
    else
        echo "${key}:${type}=${val}" >> "$AE_DOTCONFIG"
    fi
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

# Run chbuild via scripts/chbuild-with-fallback.sh (quick-build on any failure).
# Override AE_BUILD_TIMEOUT (seconds) for the per-step wall clock.
ae_chbuild_with_fallback() {
    local build_timeout="${AE_BUILD_TIMEOUT:-3600}"
    local log="${AE_LOG_DIR:-/tmp}/build_$$.log"
    mkdir -p "$(dirname "$log")"
    export CHBUILD_TIMEOUT="$build_timeout"
    export CHBUILD_LOG="$log"
    export CHBUILD_PROGRESS=1

    if "$AE_REPO_ROOT/scripts/chbuild-with-fallback.sh" "$@"; then
        return 0
    fi

    local rc=$?
    if [ "$rc" -eq 124 ]; then
        ae_record_timeout "chbuild exceeded ${build_timeout}s (log: $log)"
    else
        echo "Build failed (rc=$rc); see $log" >&2
        tail -30 "$log" >&2 || true
    fi
    return "$rc"
}

# Build with a hard timeout (a wedged docker/cmake would otherwise hang the
# whole experiment). Override with AE_BUILD_TIMEOUT (seconds).
ae_build() {
    echo "=== Building ChCore (timeout ${AE_BUILD_TIMEOUT:-3600}s, live progress below) ==="
    ae_chbuild_with_fallback build || return 1
    echo "Build OK."
}

# Prefer incremental chbuild; on failure run quick-build.sh, restore .config from
# a snapshot, optionally AE_BUILD_POST_RESTORE_HOOK, then rebuild (with fallback).
# Optional first argument: executable that must exist after a successful build.
ae_build_with_config_restore() {
    local verify_bin="${1:-}"
    local config_snapshot
    config_snapshot="$(mktemp)"
    cp "$AE_DOTCONFIG" "$config_snapshot"

    _verify_binary() {
        if [ -n "$verify_bin" ] && [ ! -x "$verify_bin" ]; then
            echo "Expected binary missing: $verify_bin" >&2
            return 1
        fi
        return 0
    }

    _try_incremental() {
        ae_chbuild_with_fallback --no-fallback build && _verify_binary
    }

    _try_with_fallback() {
        ae_chbuild_with_fallback build && _verify_binary
    }

    echo "=== Building ChCore (timeout ${AE_BUILD_TIMEOUT:-3600}s, live progress below) ==="
    if _try_incremental; then
        rm -f "$config_snapshot"
        echo "Build OK."
        return 0
    fi

    echo "=== Falling back to distclean + defconfig + build ===" >&2
    if ! (cd "$AE_REPO_ROOT" && \
          CHBUILD_PROGRESS=1 \
          ./scripts/chbuild-with-fallback.sh --no-fallback \
              distclean defconfig x86_64 build); then
        rm -f "$config_snapshot"
        return 1
    fi

    echo "=== Restoring artifact configuration after quick-build ==="
    cp "$config_snapshot" "$AE_DOTCONFIG"
    rm -f "$config_snapshot"
    if [ -n "${AE_BUILD_POST_RESTORE_HOOK:-}" ]; then
        eval "$AE_BUILD_POST_RESTORE_HOOK"
    fi

    _try_with_fallback || return 1
    echo "Build OK."
    return 0
}

# ── QEMU cluster management ─────────────────────────────────────────────────

ae_machine_log() {
    echo "$AE_REPO_ROOT/exec_log$1.log"
}

# ── run-error / timeout accounting ──────────────────────────────────────────
# Timeouts and detected guest errors are recorded here; experiments call
# ae_finish at the end so a run with failures still parses whatever logs it
# has but exits non-zero.

AE_TIMEOUT_ERRORS=()
AE_RUN_ERRORS=()

# Fatal guest-side signatures. When any appears in a machine log while we are
# waiting for a benchmark's success marker, the benchmark is considered
# failed: we stop waiting immediately, record it, and the caller moves on to
# the next test. Kept conservative to avoid matching benign WARN lines.
#   General Protection Fault / Trap No. / #GP     -> CPU exception
#   panic / Kernel panic / BUG:                    -> kernel abort
#   Unhandled ... [Ee]xception / fault             -> unhandled trap
#   pool=NULL for va / KERNEL FAULT                 -> known fatal paths
AE_ERROR_PATTERN="${AE_ERROR_PATTERN:-General Protection Fault|Kernel panic|kernel panic|panic:|BUG:|BUG_ON|Unhandled .*[Ee]xception|Unhandled .*fault|pool=NULL for va|KERNEL FAULT|Trap No\\. }"

ae_record_timeout() {
    local what="$1"
    AE_TIMEOUT_ERRORS+=("$what")
    echo "[AE][TIMEOUT-ERROR] $what" >&2
}

ae_record_error() {
    local what="$1"
    AE_RUN_ERRORS+=("$what")
    echo "[AE][RUN-ERROR] $what" >&2
}

# ae_finish
# Print a failure summary; exit non-zero if any step errored or timed out.
ae_finish() {
    local n_to="${#AE_TIMEOUT_ERRORS[@]}" n_err="${#AE_RUN_ERRORS[@]}"
    if [ "$n_err" -gt 0 ]; then
        echo "" >&2
        echo "[AE] $n_err step(s) FAILED with a detected error:" >&2
        local t
        for t in "${AE_RUN_ERRORS[@]}"; do echo "[AE]   - $t" >&2; done
    fi
    if [ "$n_to" -gt 0 ]; then
        echo "" >&2
        echo "[AE] $n_to step(s) TIMED OUT:" >&2
        local t
        for t in "${AE_TIMEOUT_ERRORS[@]}"; do echo "[AE]   - $t" >&2; done
    fi
    if [ "$n_err" -gt 0 ] || [ "$n_to" -gt 0 ]; then
        exit 2
    fi
}

# grep helper: BSD-style [[:<:]] word boundaries aren't in GNU grep, so use -E
# with our alternation directly (word-boundary intent kept via the phrasing).
_ae_error_grep() {
    grep -aE "$AE_ERROR_PATTERN" "$1" 2>/dev/null | head -1
}

# ae_wait_in_log <machine> <success-pattern> <timeout-s> <label>
# Returns:
#   0  success pattern seen
#   1  timed out          (recorded via ae_record_timeout)
#   3  guest error / crash / tmux died (recorded via ae_record_error)
# On 1 or 3 the caller should save the log and continue to the next test.
ae_wait_in_log() {
    local machine="$1" pattern="$2" timeout="$3" label="$4"
    local logfile err
    logfile="$(ae_machine_log "$machine")"
    local elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        if grep -aq "$pattern" "$logfile" 2>/dev/null; then
            [ -n "$label" ] && echo "$label"
            return 0
        fi
        err="$(_ae_error_grep "$logfile")"
        if [ -n "$err" ]; then
            ae_record_error "$label: guest error detected -> ${err## } (log: $logfile)"
            tail -40 "$logfile" >&2 || true
            return 3
        fi
        if ! tmux has-session -t "$AE_SESSION" 2>/dev/null; then
            ae_record_error "$label: tmux session $AE_SESSION died before success marker (log: $logfile)"
            tail -40 "$logfile" >&2 || true
            return 3
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
# Robust against two console failure modes:
#  1. Right after boot the guest tty may hold terminal escape-sequence
#     responses (e.g. "1;2c"); a bare Enter first flushes the input line.
#  2. During heavy output floods the emulated serial line drops input chars,
#     silently eating the command — so wait for the console to quiesce,
#     verify the command echoed back in the machine log, and resend if not.
ae_send_command() {
    local machine="$1" cmd="$2"
    local logfile prev_sz=-1 cur_sz quiet=0 waited=0 try sent
    logfile="$(ae_machine_log "$machine")"

    # wait for console quiesce (log size stable twice in a row, max 60s)
    while [ "$quiet" -lt 2 ] && [ "$waited" -lt 60 ]; do
        cur_sz=$(stat -c%s "$logfile" 2>/dev/null || echo 0)
        if [ "$cur_sz" = "$prev_sz" ]; then quiet=$((quiet + 1)); else quiet=0; fi
        prev_sz="$cur_sz"
        sleep 2
        waited=$((waited + 2))
    done

    for try in 1 2 3; do
        tmux send-keys -t "$AE_SESSION:$machine" "" Enter
        sleep 1
        tmux send-keys -t "$AE_SESSION:$machine" "$cmd" Enter
        sleep 3
        sent="$(grep -acF "$cmd" "$logfile" 2>/dev/null || true)"
        if [ "${sent:-0}" -gt 0 ]; then
            return 0
        fi
        echo "[WARN] command not echoed on machine $machine (try $try); resending" >&2
    done
    echo "[WARN] command may not have reached machine $machine: $cmd" >&2
    return 0
}

ae_kill_cluster() {
    if tmux has-session -t "$AE_SESSION" 2>/dev/null; then
        tmux kill-session -t "$AE_SESSION"
    fi
}

# Kill every known AE / benchmark tmux session so a timed-out experiment
# cannot leave QEMU holding ivshmem and break the next one.
ae_kill_all_ae_sessions() {
    local s
    for s in \
        "${USER}-ae" \
        "${USER}-ipc-ae" \
        "${USER}-sched-notify-ae" \
        "${USER}-recover-fs-ae" \
        "${USER}-msi-basic-ae" \
        "${USER}-qemu"
    do
        tmux kill-session -t "$s" 2>/dev/null || true
    done
}

# First-time OS prepare (equivalent to `make prepare`'s build step).
# Shared-memory / hostfs / doorbell are handled by prepare.sh (idempotent).
# This only runs chbuild/quick-build when the tree has never been built:
#   missing .config  OR  missing build/kernel.img
# SKIP_BASE_BUILD=1 skips this. FORCE_BASE_BUILD=1 always re-runs quick-build.
ae_ensure_base_build() {
    local skip_build="${SKIP_BASE_BUILD:-0}"
    local force_build="${FORCE_BASE_BUILD:-0}"
    local kernel_img="$AE_REPO_ROOT/build/kernel.img"
    cd "$AE_REPO_ROOT"

    if [ "$skip_build" = "1" ]; then
        echo "[AE] SKIP_BASE_BUILD=1 — not checking OS image"
        return 0
    fi

    if [ "$force_build" = "1" ]; then
        echo "[AE] FORCE_BASE_BUILD=1 — running scripts/quick-build.sh (make prepare build step)"
        ./scripts/quick-build.sh
        return $?
    fi

    if [ -f "$AE_DOTCONFIG" ] && [ -f "$kernel_img" ]; then
        echo "[AE] OS already prepared (.config + build/kernel.img present); skip first-time build"
        return 0
    fi

    echo "[AE] First-time OS prepare (same as make prepare's build step)"
    if [ ! -f "$AE_DOTCONFIG" ]; then
        echo "[AE]   missing .config"
    fi
    if [ ! -f "$kernel_img" ]; then
        echo "[AE]   missing $kernel_img"
    fi
    # quick-build.sh: distclean + defconfig x86_64 + build
    ./scripts/quick-build.sh
    if [ ! -f "$kernel_img" ]; then
        echo "[AE] quick-build finished but $kernel_img is still missing" >&2
        return 1
    fi
    echo "[AE] First-time OS prepare done: $kernel_img"
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
