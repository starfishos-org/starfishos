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
#   ae_kill_cluster              - tear down the tmux session + reap stray QEMU
#   ae_stop_tmux_and_reap [sess] - kill one tmux session + reap stray QEMU
#   ae_ensure_clean_tmux         - kill all AE/QEMU tmux sessions + stray QEMU
#   ae_ensure_cxlfs_device       - recreate CXLFS when ramdisk build changed
#   ae_init_output_dirs DIR      - mkdir out/<timestamp>/{logs,csv,figures}
#   ae_set_dsm_var VAR VAL       - edit kernel/dsm_config.cmake
#   ae_set_dotconfig KEY TYPE VAL- edit .config (e.g. CHCORE_KERNEL_TEST BOOL ON)
#   ae_save_build_configs / ae_restore_build_configs
#   ae_prepare_microbench_guest_cpu / ae_prepare_paper_guest_cpu
#   ae_set_paper_guest_cpu_config
#
# Conventions:
#   AE_SESSION  - tmux session name (default: $USER-ae)
#   AE_LOG_DIR  - where per-machine logs are copied (set by caller)
#   Machine i's live log is $AE_MACHINE_LOG_DIR/exec_log<i>.log (written by
#   build/simulate.sh itself); ae_boot_cluster removes stale ones first.

AE_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AE_SESSION="${AE_SESSION:-${USER}-ae}"
AE_DSM_CONFIG="$AE_REPO_ROOT/kernel/dsm_config.cmake"
AE_DEMOS_CONFIG="$AE_REPO_ROOT/user/demos/config.cmake"
AE_DOTCONFIG="$AE_REPO_ROOT/.config"
AE_CHCORE_INI="$AE_REPO_ROOT/chcore.ini"
AE_BOOT_TIMEOUT="${AE_BOOT_TIMEOUT:-600}"
AE_MACHINE_LOG_DIR="${AE_MACHINE_LOG_DIR:-$AE_REPO_ROOT/logs}"
AE_RUN_LOCK_FD=""

# Hold one per-user lock for the caller's entire shell lifetime.  The default
# path is outside the checkout so runners from different clones still
# serialize access to QEMU, tmux, ivshmem, and host-level baseline tuning.
ae_acquire_run_lock() {
    local purpose="${1:-artifact-evaluation}"
    local lock_dir
    if [ -n "${AE_RUN_LOCK_DIR:-}" ]; then
        lock_dir="$AE_RUN_LOCK_DIR"
    elif [ -d "/run/user/$UID" ] \
        && [ "$(stat -Lc '%u' -- "/run/user/$UID" 2>/dev/null || true)" = "$UID" ]; then
        lock_dir="/run/user/$UID/starfishos-ae-lock"
    else
        lock_dir="/tmp/starfishos-ae-lock-$UID"
    fi
    local lock_file="$lock_dir/runner.lock"
    local owner holder

    if [ -n "$AE_RUN_LOCK_FD" ]; then
        return 0
    fi
    if ! command -v flock >/dev/null 2>&1; then
        echo "[AE] required tool missing: flock" >&2
        return 1
    fi
    if [ -L "$lock_dir" ]; then
        echo "[AE] refusing symlink runner lock directory: $lock_dir" >&2
        return 1
    fi
    (umask 077; mkdir -p -- "$lock_dir") || return 1
    if [ -L "$lock_dir" ] || [ ! -d "$lock_dir" ]; then
        echo "[AE] invalid runner lock directory: $lock_dir" >&2
        return 1
    fi
    owner="$(stat -Lc '%u' -- "$lock_dir" 2>/dev/null || true)"
    if [ "$owner" != "$UID" ]; then
        echo "[AE] refusing runner lock directory not owned by uid $UID: $lock_dir" >&2
        return 1
    fi
    chmod 700 -- "$lock_dir" || return 1
    exec {AE_RUN_LOCK_FD}<>"$lock_file" || return 1
    if ! flock -n "$AE_RUN_LOCK_FD"; then
        holder="$(tr '\n' ' ' < "$lock_file" 2>/dev/null || true)"
        echo "[AE] another artifact runner already holds $lock_file" >&2
        [ -n "$holder" ] && echo "[AE] lock holder: $holder" >&2
        eval "exec ${AE_RUN_LOCK_FD}>&-"
        AE_RUN_LOCK_FD=""
        return 1
    fi

    : > "$lock_file"
    printf 'pid=%s purpose=%s repo=%s started=%s\n' \
        "$BASHPID" "$purpose" "$AE_REPO_ROOT" "$(date -Is)" \
        >&"$AE_RUN_LOCK_FD"
    echo "[AE] acquired per-user runner lock: $lock_file"
}

# ── global environment check ────────────────────────────────────────────────

ae_doorbell_running() {
    local socket="/tmp/ivshmem-doorbell-$USER"
    local pid_file="/tmp/ivshmem-server-$USER.pid"
    local pid

    [ -S "$socket" ] || return 1
    [ -f "$pid_file" ] || return 1
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    [ -L "/proc/$pid/exe" ] || return 1
    [ "$(basename "$(readlink -f "/proc/$pid/exe")")" = "ivshmem-server" ]
}

# Recreate the per-user CXLFS ivshmem device when its stamped ramdisk build-id
# no longer matches user/build/ramdisk.cpio.
ae_ensure_cxlfs_device() {
    "$AE_REPO_ROOT/dsm-scripts/prepare_cxlfs_dev.sh" ensure || return 1
}

# Standard per-experiment artifact layout (one directory per run):
#   <experiment>/out/<timestamp>/logs/     runtime QEMU and benchmark logs
#   <experiment>/out/<timestamp>/csv/      parsed tables and intermediate data
#   <experiment>/out/<timestamp>/figures/  plots (png)
ae_init_output_dirs() {
    local ae_dir="$1"
    TS="${TS:-$(date +%Y%m%d_%H%M%S)}"
    OUT_DIR="${OUT_DIR:-$ae_dir/out/$TS}"
    LOG_DIR="${LOG_DIR:-$OUT_DIR/logs}"
    CSV_DIR="${CSV_DIR:-$OUT_DIR/csv}"
    FIG_DIR="${FIG_DIR:-$OUT_DIR/figures}"
    mkdir -p "$LOG_DIR" "$CSV_DIR" "$FIG_DIR"
    echo "[AE] Output directory: $OUT_DIR"
}

ae_ensure_doorbell() {
    if ae_doorbell_running; then
        return 0
    fi
    echo "[AE] ivshmem doorbell is absent or stale; restarting it."
    "$AE_REPO_ROOT/dsm-scripts/start_ivshmem_server.sh" || return 1
    ae_doorbell_running || {
        echo "ivshmem doorbell restart did not produce a live server/socket." >&2
        return 1
    }
}

ae_check_global_prepare() {
    local cxl_file="/dev/shm/ivshmem-$USER"
    local hostfs_file="/dev/shm/ivshmem-hostfs-$USER"
    local cxlfs_file="/dev/shm/ivshmem-cxlfs-$USER"
    local numa_files=(
        "/dev/shm/numa0.0-$USER" "/dev/shm/numa0.1-$USER"
        "/dev/shm/numa1.0-$USER" "/dev/shm/numa1.1-$USER"
        "/dev/shm/numa2.0-$USER" "/dev/shm/numa2.1-$USER"
        "/dev/shm/numa3.0-$USER" "/dev/shm/numa3.1-$USER"
    )
    local f

    echo "=== Checking global AE environment ==="
    for f in "$cxl_file" "$hostfs_file" "$cxlfs_file" "${numa_files[@]}"; do
        if [ ! -e "$f" ]; then
            echo "Missing global AE resource: $f" >&2
            echo "Run ./artifact-evaluation/prepare.sh once before this test." >&2
            return 1
        fi
    done
    ae_ensure_doorbell
}

ae_drop_host_caches() {
    local sync_timeout="${AE_SYNC_TIMEOUT:-120}"

    if [ "${AE_DROP_CACHES:-1}" != "1" ]; then
        echo "[AE] AE_DROP_CACHES=${AE_DROP_CACHES:-0}; skip host sync/cache drop."
        return 0
    fi

    echo "[AE] Syncing host filesystems (timeout ${sync_timeout}s)."
    if command -v timeout >/dev/null 2>&1; then
        if ! timeout "$sync_timeout" sync; then
            echo "[AE] WARNING: host sync timed out/failed; skipping cache drop." >&2
            return 0
        fi
    elif ! sync; then
        echo "[AE] WARNING: host sync failed; skipping cache drop." >&2
        return 0
    fi

    echo "[AE] Dropping host page cache (non-interactive sudo)."
    if ! sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' >/dev/null 2>&1; then
        echo "[AE] sudo -n unavailable; continuing without dropping host caches." >&2
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

# ae_set_demo_var CHCORE_DEMOS_REDIS ON
ae_set_demo_var() {
    local var="$1" val="$2"
    sed -i "s/^chcore_config(${var} BOOL [A-Z]* /chcore_config(${var} BOOL ${val} /" "$AE_DEMOS_CONFIG"
    grep -q "^chcore_config(${var} BOOL ${val} " "$AE_DEMOS_CONFIG" || {
        echo "Failed to set ${var}=${val} in $AE_DEMOS_CONFIG" >&2
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

# Guest vCPU profiles used across AE scripts.  simulate.sh defaults to
# chcore.ini (96 on the paper testbed) when CPU_NUM is unset; microbenchmarks
# should call ae_prepare_microbench_guest_cpu before booting QEMU.
AE_PAPER_GUEST_CPU_NUM="${AE_PAPER_GUEST_CPU_NUM:-96}"
AE_MICROBENCH_GUEST_CPU_NUM="${AE_MICROBENCH_GUEST_CPU_NUM:-12}"

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

# Runtime QEMU SMP count (simulate.sh: CPU_NUM overrides chcore.ini).
ae_export_guest_cpu_num() {
    local val="$1"
    export CPU_NUM="$val"
    echo "[AE] Guest QEMU CPUs: $val"
}

# Paper-scale cluster tests: 96 vCPUs unless the caller overrides CPU_NUM.
ae_prepare_paper_guest_cpu() {
    ae_export_guest_cpu_num "${CPU_NUM:-$AE_PAPER_GUEST_CPU_NUM}"
}

# ipc-cdf / sched-notify: small guest to avoid rr_sched budget issues at boot.
ae_prepare_microbench_guest_cpu() {
    ae_export_guest_cpu_num "${CPU_NUM:-$AE_MICROBENCH_GUEST_CPU_NUM}"
}

# Compile-time ceiling for builds that need the full paper CPU namespace.
ae_set_paper_guest_cpu_config() {
    local val="${1:-$AE_PAPER_GUEST_CPU_NUM}"
    ae_set_ini_cpu_num "$val"
    ae_set_dotconfig CHCORE_PLAT_CPU_NUM STRING "$val"
}

AE_CONFIG_BACKUP_DIR=""

ae_save_build_configs() {
    AE_CONFIG_BACKUP_DIR="$(mktemp -d)"
    cp "$AE_DSM_CONFIG" "$AE_CONFIG_BACKUP_DIR/dsm_config.cmake"
    cp "$AE_DEMOS_CONFIG" "$AE_CONFIG_BACKUP_DIR/demos-config.cmake"
    cp "$AE_DOTCONFIG" "$AE_CONFIG_BACKUP_DIR/dotconfig"
    cp "$AE_CHCORE_INI" "$AE_CONFIG_BACKUP_DIR/chcore.ini"
}

ae_restore_build_configs() {
    if [ -n "$AE_CONFIG_BACKUP_DIR" ] && [ -d "$AE_CONFIG_BACKUP_DIR" ]; then
        local failed=0
        cp "$AE_CONFIG_BACKUP_DIR/dsm_config.cmake" "$AE_DSM_CONFIG" || failed=1
        cp "$AE_CONFIG_BACKUP_DIR/demos-config.cmake" "$AE_DEMOS_CONFIG" || failed=1
        cp "$AE_CONFIG_BACKUP_DIR/dotconfig" "$AE_DOTCONFIG" || failed=1
        cp "$AE_CONFIG_BACKUP_DIR/chcore.ini" "$AE_CHCORE_INI" || failed=1
        if [ "$failed" -ne 0 ]; then
            echo "[AE] failed to restore build configs; backup retained at $AE_CONFIG_BACKUP_DIR" >&2
            return 1
        fi
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

    # A killed/interrupted docker chbuild leaves /$USER-chbuild behind and
    # blocks the next ./chbuild invocation (README troubleshooting).
    docker rm -f "${USER}-chbuild" >/dev/null 2>&1 || true

    if "$AE_REPO_ROOT/scripts/chbuild-with-fallback.sh" "$@"; then
        return 0
    else
        local rc=$?
    fi
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
              distclean x86_64 build); then
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
    echo "$AE_MACHINE_LOG_DIR/exec_log$1.log"
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
AE_ERROR_PATTERN="${AE_ERROR_PATTERN:-General Protection Fault|Kernel panic|kernel panic|panic:|BUG:|BUG_ON|Unhandled .*[Ee]xception|Unhandled .*fault|pool=NULL for va|KERNEL FAULT|Trap No\\. |Persistent data verification failed|do_page_fault: invalid user access|do_page_fault: user NULL dereference}"

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

# A frozen serial log usually means a guest/boot hang. Set this to 0 for an
# intentionally silent workload that should wait for the caller's full timeout.
AE_LOG_STALL_S="${AE_LOG_STALL_S:-120}"

# ae_wait_in_log <machine> <success-pattern> <timeout-s> <label> [machine-count]
# The optional machine count checks fatal signatures and pane liveness on
# every machine 0..N-1 while still looking for the success marker on the
# selected machine's log.
# Returns:
#   0  success pattern seen
#   1  timed out / log stalled (recorded via ae_record_timeout)
#   3  guest error / crash / tmux died (recorded via ae_record_error)
# On 1 or 3 the caller should save the log and continue to the next test.
ae_wait_in_log() {
    local machine="$1" pattern="$2" timeout="$3" label="$4"
    local watch_count="${5:-0}"
    local logfile err watch_machine watch_log watch_first watch_last
    logfile="$(ae_machine_log "$machine")"
    if [ "$watch_count" = "0" ]; then
        watch_first="$machine"
        watch_last="$machine"
    elif [[ "$watch_count" =~ ^[1-9][0-9]*$ ]]; then
        watch_first=0
        watch_last=$((watch_count - 1))
    else
        ae_record_error "$label: invalid machine count for log watch: $watch_count"
        return 3
    fi
    local elapsed=0 stall_limit="${AE_LOG_STALL_S:-120}"
    local last_sz cur_sz stalled=0
    last_sz=$(stat -c%s "$logfile" 2>/dev/null || echo 0)
    while [ "$elapsed" -lt "$timeout" ]; do
        for ((watch_machine = watch_first; watch_machine <= watch_last; watch_machine++)); do
            watch_log="$(ae_machine_log "$watch_machine")"
            err="$(_ae_error_grep "$watch_log")"
            if [ -n "$err" ]; then
                ae_record_error "$label: guest error on machine $watch_machine -> ${err## } (log: $watch_log)"
                tail -40 "$watch_log" >&2 || true
                return 3
            fi
            if ! tmux list-panes -t "$AE_SESSION:$watch_machine" >/dev/null 2>&1; then
                ae_record_error "$label: tmux window $AE_SESSION:$watch_machine died before success marker (log: $watch_log)"
                tail -40 "$watch_log" >&2 || true
                return 3
            fi
        done
        # Accept completion only after a final all-machine health scan so a
        # secondary panic cannot be hidden by machine 0's success marker.
        if grep -aq "$pattern" "$logfile" 2>/dev/null; then
            [ -n "$label" ] && echo "$label"
            return 0
        fi
        if [ "$stall_limit" -gt 0 ] 2>/dev/null; then
            cur_sz=$(stat -c%s "$logfile" 2>/dev/null || echo 0)
            if [ "$cur_sz" = "$last_sz" ]; then
                stalled=$((stalled + 2))
                if [ "$stalled" -ge "$stall_limit" ]; then
                    ae_record_timeout "$label (log stalled ${stalled}s; pattern '$pattern' not seen in $logfile)"
                    echo "[AE][STALL-SKIP] $label — no new log bytes for ${stalled}s" >&2
                    tail -40 "$logfile" >&2 || true
                    return 1
                fi
            else
                last_sz="$cur_sz"
                stalled=0
            fi
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done
    ae_record_timeout "$label (pattern '$pattern' not seen within ${timeout}s in $logfile)"
    tail -60 "$logfile" >&2 || true
    return 1
}

# The welcome banner and late boot messages share the serial console, so their
# bytes can be interleaved even though the shell is already accepting input.
# Accept either the banner or a real line-start shell prompt as readiness.
ae_wait_for_shell() {
    local machine="$1"
    ae_wait_in_log "$machine" \
        'Welcome to ChCore shell!\|^[$][[:space:]]*' \
        "$AE_BOOT_TIMEOUT" "machine $machine shell ready"
}

# ae_boot_cluster <num_machines> [cpu_num]
# Boots machines 0..N-1 in tmux windows, waits for DSM join + shell on all.
# Extra simulate.sh env (e.g. DRAM_SIZE=24G) can be passed via AE_EXTRA_ENV.
_ae_boot_cluster_once() {
    local n="$1"
    local cpu_num="${2:-}"
    local i env_prefix="${AE_EXTRA_ENV:+$AE_EXTRA_ENV }"
    local wait_shell_each="${AE_WAIT_SHELL_PER_MACHINE:-0}"

    [ -n "$cpu_num" ] && env_prefix="${env_prefix}CPU_NUM=$cpu_num "

    # PHOENIX_SCHED_TIMING makes every guest stop immediately after its DSM
    # join message until all MACHINE_NUM peers reach the TSC calibration
    # barrier.  Waiting for machine 0's shell before launching machine 1 would
    # therefore deadlock by construction.  Callers which need serialized late
    # boot must disable that optional instrumentation before building.
    if [ "$wait_shell_each" = "1" ] \
        && grep -Eq '^set\(PHOENIX_SCHED_TIMING[[:space:]]+"?ON"?\)' \
            "$AE_DSM_CONFIG"; then
        echo "[AE] AE_WAIT_SHELL_PER_MACHINE=1 is incompatible with PHOENIX_SCHED_TIMING=ON" >&2
        return 1
    fi

    ae_ensure_clean_tmux || return 1
    ae_ensure_doorbell || return 1
    ae_ensure_cxlfs_device || return 1

    echo "=== Resetting DSM metadata ==="
    (cd "$AE_REPO_ROOT" && make clean-dsm-meta >/dev/null)

    for i in $(seq 0 $((n - 1))); do
        rm -f "$(ae_machine_log "$i")"
    done
    mkdir -p "$AE_MACHINE_LOG_DIR"
    rm -f "$AE_MACHINE_LOG_DIR/exec_log.log"

    echo "=== Booting $n machine(s) ==="
    tmux new-session -d -s "$AE_SESSION" -n 0 \
        "cd '$AE_REPO_ROOT' && ${env_prefix}MACHINE_NUM=$n ./build/simulate.sh 0"
    ae_wait_in_log 0 "DSM] machine 0 " "$AE_BOOT_TIMEOUT" "DSM machine 0 joined" || return 1
    if [ "$wait_shell_each" = "1" ]; then
        ae_wait_for_shell 0 || return 1
    fi

    for i in $(seq 1 $((n - 1))); do
        tmux new-window -t "$AE_SESSION" -n "$i" \
            "cd '$AE_REPO_ROOT' && ${env_prefix}MACHINE_NUM=$n ./build/simulate.sh $i"
        ae_wait_in_log "$i" "DSM] machine $i " "$AE_BOOT_TIMEOUT" "DSM machine $i joined" || return 1
        if [ "$wait_shell_each" = "1" ]; then
            ae_wait_for_shell "$i" || return 1
        fi
    done

    if [ "$wait_shell_each" != "1" ]; then
        for i in $(seq 0 $((n - 1))); do
            ae_wait_for_shell "$i" || return 1
        done
    fi
}

# Retry intermittent secondary-machine boot/shell stalls. Failures from a
# superseded attempt are removed from the final summary; the last attempt is
# retained when all retries fail.
ae_boot_cluster() {
    local n="$1" cpu_num="${2:-}"
    local max_attempts="${AE_BOOT_RETRIES:-2}"
    local attempt before_to before_err

    for attempt in $(seq 1 "$max_attempts"); do
        before_to="${#AE_TIMEOUT_ERRORS[@]}"
        before_err="${#AE_RUN_ERRORS[@]}"
        if _ae_boot_cluster_once "$n" "$cpu_num"; then
            return 0
        fi
        if [ "$attempt" -ge "$max_attempts" ]; then
            # Returning a failed boot must never leave a partial cluster alive:
            # callers may immediately rebuild for the next point.  Serial logs
            # remain on disk for callers which archive diagnostics afterward.
            ae_stop_tmux_and_reap "$AE_SESSION" || true
            return 1
        fi

        echo "[AE] boot attempt $attempt/$max_attempts failed; restarting doorbell and retrying." >&2
        AE_TIMEOUT_ERRORS=("${AE_TIMEOUT_ERRORS[@]:0:before_to}")
        AE_RUN_ERRORS=("${AE_RUN_ERRORS[@]:0:before_err}")
        ae_stop_tmux_and_reap "$AE_SESSION" || true
        "$AE_REPO_ROOT/dsm-scripts/kill_ivshmem_server.sh" || true
        ae_ensure_doorbell || return 1
    done
    return 1
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
    ae_stop_tmux_and_reap "$AE_SESSION"
}

# Kill every known AE / benchmark tmux session so a timed-out experiment
# cannot leave QEMU holding ivshmem and break the next one.
ae_kill_all_ae_sessions() {
    local s pane_start seen=" "
    for s in \
        "${USER}-ae" \
        "${USER}-ipc-ae" \
        "${USER}-sched-notify-ae" \
        "${USER}-recover-fs-ae" \
        "${USER}-msi-basic-ae" \
        "${USER}-qemu"
    do
        if tmux has-session -t "$s" 2>/dev/null; then
            echo "[AE] killing tmux session $s"
            tmux kill-session -t "$s" 2>/dev/null || true
        fi
        seen+="$s "
    done

    # A caller may override SESSION/AE_SESSION, so fixed names alone are not
    # enough for interrupt cleanup.  Kill only sessions whose pane was started
    # from this checkout and launches the ChCore simulator; unrelated tmux
    # work is left untouched.
    while IFS=$'\t' read -r s pane_start; do
        [ -n "$s" ] || continue
        [[ "$seen" == *" $s "* ]] && continue
        case "$pane_start" in
            *"$AE_REPO_ROOT"*"simulate.sh"*)
                echo "[AE] killing artifact tmux session $s"
                tmux kill-session -t "$s" 2>/dev/null || true
                seen+="$s "
                ;;
        esac
    done < <(tmux list-panes -a -F '#S\t#{pane_start_command}' 2>/dev/null | awk -F '\t' '!seen[$1]++')
}

# Emergency path for an interrupted one-click run.  tmux owns a separate
# process tree and each runner holds a flock parent, so killing only QEMU does
# not necessarily release runner.lock.  Restrict the match to this checkout's
# AE entry points and this user's AE lock; never kill unrelated processes.
# AE_FORCE_STOP_SKIP_PID may name the current run_all.py process so its signal
# handler can finish this cleanup itself.
ae_force_stop_artifact_runners() {
    local pid args skip_pid="${AE_FORCE_STOP_SKIP_PID:-}"

    ae_kill_all_ae_sessions
    ae_reap_leftover_qemu KILL

    while read -r pid args; do
        [ -n "$pid" ] || continue
        [ "$pid" = "$$" ] && continue
        [ -n "$skip_pid" ] && [ "$pid" = "$skip_pid" ] && continue
        case "$args" in
            *"$AE_REPO_ROOT/artifact-evaluation/run_all.py"*|\
            *"$AE_REPO_ROOT/artifact-evaluation/"*"/run.sh"*|\
            *"starfishos-ae-lock"*"runner.lock"*)
                echo "[AE] forcibly stopping artifact runner pid $pid"
                kill -KILL "$pid" 2>/dev/null || true
                ;;
        esac
    done < <(ps -u "$USER" -o pid=,args= 2>/dev/null)
}

# True when any ChCore guest QEMU (-name chcore-*) is still running.
ae_has_chcore_qemu() {
    ps -u "$USER" -o args= 2>/dev/null \
        | awk '$1 ~ /(^|\/)qemu-6[.]2-system-x86_64$/ && /-name[[:space:]]+chcore-/ { found=1 } END { exit !found }'
}

# Reap ChCore QEMU instances that outlived their tmux pane (e.g. after
# timeout or manual tmux kill).  Match only our guests (-name chcore-*).
# Optional signal: TERM (default) or KILL.
ae_reap_leftover_qemu() {
    local signal="${1:-TERM}" pid
    while read -r pid; do
        [ -n "$pid" ] || continue
        echo "[AE] reaping leftover ChCore QEMU pid $pid ($signal)"
        kill "-$signal" "$pid" 2>/dev/null || true
    done < <(ps -u "$USER" -o pid=,args= 2>/dev/null \
        | awk '$2 ~ /(^|\/)qemu-6[.]2-system-x86_64$/ && /-name[[:space:]]+chcore-/ { print $1 }')
    sleep 1
}

# Block until no ChCore QEMU remains, escalating to SIGKILL if needed.
ae_wait_qemu_gone() {
    local timeout="${1:-30}" elapsed=0

    while [ "$elapsed" -lt "$timeout" ]; do
        ae_has_chcore_qemu || return 0
        sleep 1
        elapsed=$((elapsed + 1))
    done

    echo "[AE] QEMU still running after ${timeout}s; reaping" >&2
    ae_reap_leftover_qemu TERM
    elapsed=0
    while [ "$elapsed" -lt 10 ]; do
        ae_has_chcore_qemu || return 0
        sleep 1
        elapsed=$((elapsed + 1))
    done

    ae_reap_leftover_qemu KILL
    sleep 1
    if ae_has_chcore_qemu; then
        echo "[AE] ERROR: leftover ChCore QEMU could not be reaped" >&2
        return 1
    fi
    return 0
}

# Kill one tmux session and reap any QEMU it left behind.  Use between
# per-mode reboots (ipc-cdf) as well as normal ae_kill_cluster teardown.
ae_stop_tmux_and_reap() {
    local session="${1:-$AE_SESSION}"

    if tmux has-session -t "$session" 2>/dev/null; then
        echo "[AE] killing tmux session $session"
        tmux kill-session -t "$session" 2>/dev/null || true
        sleep 1
    fi
    ae_reap_leftover_qemu TERM
    ae_wait_qemu_gone "${AE_QEMU_REAP_TIMEOUT:-30}"
}

# Call before booting QEMU so a prior run cannot hold ivshmem or the session
# name.  Safe to invoke at script entry and again inside ae_boot_cluster.
ae_ensure_clean_tmux() {
    echo "=== Ensuring clean tmux / QEMU state ==="
    ae_kill_all_ae_sessions
    ae_reap_leftover_qemu
    ae_wait_qemu_gone "${AE_QEMU_REAP_TIMEOUT:-30}"
}

# Release the large per-user StarfishOS backing files before a host-level
# baseline such as Tigon preallocates its own VM memory.  Do not kill an
# unexpected guest here: the caller holds the runner lock, and a live ChCore
# QEMU indicates an external/manual user that must be investigated instead.
ae_release_memdev_backing() {
    if ae_has_chcore_qemu; then
        echo "[AE] refusing to release memdev backing files while ChCore QEMU is running" >&2
        return 1
    fi

    # Preflight before stopping even the per-user doorbell, then repeat inside
    # the removal command after it stops, so the complete fixed path set is
    # checked before any backing file is deleted under the official lock.
    "$AE_REPO_ROOT/dsm-scripts/clean_memdev.sh" \
        --check --sudo-fuser || return 1
    "$AE_REPO_ROOT/dsm-scripts/kill_ivshmem_server.sh" || return 1
    "$AE_REPO_ROOT/dsm-scripts/clean_memdev.sh" --sudo-fuser || return 1
    echo "[AE] Released this user's StarfishOS /dev/shm backing files."
    echo "[AE] Run artifact-evaluation/prepare.sh before the next StarfishOS experiment."
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
        docker rm -f "${USER}-chbuild" >/dev/null 2>&1 || true
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
    docker rm -f "${USER}-chbuild" >/dev/null 2>&1 || true
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
