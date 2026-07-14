#!/bin/bash
#
# E2E benchmark: kernel get_pages/kmalloc tests + user malloc_benchmark.bin
#
# Each kernel test run   = 1 fresh QEMU session.
# Each user bench thread count = 1 fresh QEMU session (avoids rpmalloc state pollution).
# All logs archived under LOG_DIR (default: logs/malloc/).
#
# Usage: ./dsm-scripts/malloc/bench_malloc_e2e.sh [output.csv]
#
# Env vars:
#   NRUNS=3            number of repeat runs per config
#   RUN_OFFSET=0       add to run index (resume from run N+1)
#   APPEND_CSV=0       set 1 to append instead of overwrite CSV
#   USER_BENCH_THREADS thread counts to sweep (default: 1 2 4 8 16 32 64 96)
#   USER_BENCH_TIMEOUT seconds to wait per user bench QEMU session (default: 300)
#   LOG_DIR            directory for all raw logs
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DSM_CONFIG="$ROOT_DIR/kernel/dsm_config.cmake"
CSV_OUT="${1:-$ROOT_DIR/bench_malloc_results.csv}"
LOG_DIR="${LOG_DIR:-$ROOT_DIR/logs/malloc}"
SESSION="$USER-qemu"
NRUNS="${NRUNS:-3}"
RUN_OFFSET="${RUN_OFFSET:-0}"
APPEND_CSV="${APPEND_CSV:-0}"

USER_BENCH_THREADS="${USER_BENCH_THREADS:-1 2 4 8 16 32 64 96}"
USER_BENCH_FIXED_ARGS="0 0 0 10 100 1000 16 256"
USER_BENCH_DONE="throughput(ops/s):"
USER_BENCH_TIMEOUT="${USER_BENCH_TIMEOUT:-600}"
KERNEL_BENCH_TIMEOUT="${KERNEL_BENCH_TIMEOUT:-900}"

KERNEL_TEST_CMD="test_malloc"
KERNEL_TEST_DONE="kernel tests done"

mkdir -p "$LOG_DIR"

# ── helpers ──────────────────────────────────────────────────────────────

set_cmake_var() {
    local var="$1" val="$2"
    sed -i "s/^set(${var} .*)$/set(${var} \"${val}\")/" "$DSM_CONFIG"
}

CONFIG_SAVED=0
save_config() {
    cp "$DSM_CONFIG" "$LOG_DIR/dsm_config.cmake.bak"
    CONFIG_SAVED=1
}
restore_config() {
    if [ "$CONFIG_SAVED" = "1" ]; then
        cp "$LOG_DIR/dsm_config.cmake.bak" "$DSM_CONFIG"
        CONFIG_SAVED=0
    fi
}
trap restore_config EXIT

wait_in_log() {
    local logfile="$1" pattern="$2" timeout="${3:-300}"
    local elapsed=0
    while ! grep -q "$pattern" "$logfile" 2>/dev/null; do
        sleep 2; elapsed=$((elapsed + 2))
        if [ "$elapsed" -ge "$timeout" ]; then
            echo "[E2E] TIMEOUT waiting for '$pattern' in $logfile" >&2
            return 1
        fi
    done
}

# Start one QEMU session via simulate_ncluster.sh, send <cmd>, wait for
# <done_str> to appear in exec_log0.log, save log to <logfile>, kill QEMU.
# Returns 0 on success, 1 on timeout.
run_qemu_session() {
    local logfile="$1" cmd="$2" done_str="$3" timeout="${4:-600}"
    # simulate_ncluster.sh writes the matched completion line to its second
    # argument. The E2E runner already archives the full guest log, so a
    # separate per-run .match checkpoint is redundant.
    local matchfile="/dev/null"

    rm -f "$ROOT_DIR/exec_log0.log" "$ROOT_DIR/exec_log.log"

    # Use setsid so simulate_ncluster.sh gets its own process group.
    # We can then kill the whole group cleanly on timeout.
    setsid bash -c "cd '$ROOT_DIR' && \
        ./dsm-scripts/simulate_ncluster.sh 1 \
            '$matchfile' \
            '$cmd' \
            '$done_str'" \
        > "$LOG_DIR/_sim_$$.log" 2>&1 &
    local sim_pid=$!

    local ok=0
    wait_in_log "$ROOT_DIR/exec_log0.log" "$done_str" "$timeout" && ok=1 || ok=0

    # Archive log regardless of success
    if [ -f "$ROOT_DIR/exec_log0.log" ]; then
        cp "$ROOT_DIR/exec_log0.log" "$logfile"
    elif [ -f "$ROOT_DIR/exec_log.log" ]; then
        cp "$ROOT_DIR/exec_log.log" "$logfile"
    fi

    # Kill the entire process group (setsid makes pgid == sim_pid)
    kill -- -"$sim_pid" 2>/dev/null || true
    wait "$sim_pid" 2>/dev/null || true
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    rm -f "$LOG_DIR/_sim_$$.log"
    sleep 2

    return $(( 1 - ok ))
}

# ── build + run ───────────────────────────────────────────────────────────

build_current_config() {
    local label="$1"
    local config_snapshot

    echo "========================================"
    echo "[E2E] Building config: $label"
    echo "========================================"
    config_snapshot="$(mktemp)"
    cp "$ROOT_DIR/.config" "$config_snapshot"

    if (cd "$ROOT_DIR" && ./chbuild clean && ./chbuild build) 2>&1 | tail -10; then
        rm -f "$config_snapshot"
        return 0
    fi

    echo "[E2E] chbuild failed; retrying with scripts/quick-build.sh" >&2
    if ! (cd "$ROOT_DIR" && ./scripts/quick-build.sh) 2>&1 | tail -10; then
        rm -f "$config_snapshot"
        echo "[E2E] QUICK BUILD FAILED for config: $label" >&2
        return 1
    fi

    # quick-build.sh runs defconfig, so restore the allocator's CPU/test
    # selection and rebuild the affected targets before running benchmarks.
    cp "$config_snapshot" "$ROOT_DIR/.config"
    rm -f "$config_snapshot"
    if ! (cd "$ROOT_DIR" && ./chbuild build) 2>&1 | tail -10; then
        echo "[E2E] BUILD FAILED after restoring config: $label" >&2
        return 1
    fi
}

run_kernel_benchmarks() {
    local label="$1"
    for run in $(seq 1 "$NRUNS"); do
        local abs_run=$(( run + RUN_OFFSET ))
        echo ""
        echo "[E2E] === Kernel config=$label  Run=$abs_run/$NRUNS ==="

        local klog="$LOG_DIR/${label}_run${abs_run}_kernel.log"
        echo "[E2E]   kernel test → $(basename "$klog")"
        if run_qemu_session "$klog" "$KERNEL_TEST_CMD" "$KERNEL_TEST_DONE" \
                "$KERNEL_BENCH_TIMEOUT"; then
            echo "[E2E]   kernel test done."
        else
            echo "[E2E]   kernel test TIMEOUT/FAIL — log saved, continuing." >&2
        fi
    done
}

run_user_benchmarks() {
    local label="$1"
    for run in $(seq 1 "$NRUNS"); do
        local abs_run=$(( run + RUN_OFFSET ))
        echo ""
        echo "[E2E] === User config=$label  Run=$abs_run/$NRUNS ==="
        for threads in $USER_BENCH_THREADS; do
            local ulog="$LOG_DIR/${label}_run${abs_run}_user_t${threads}.log"
            echo "[E2E]   user bench threads=$threads → $(basename "$ulog")"
            if run_qemu_session "$ulog" \
                    "malloc_benchmark.bin $threads $USER_BENCH_FIXED_ARGS" \
                    "$USER_BENCH_DONE" \
                    "$USER_BENCH_TIMEOUT"; then
                echo "[E2E]   threads=$threads done."
            else
                echo "[E2E]   threads=$threads TIMEOUT — log saved, stopping sweep for this run." >&2
                break   # higher counts will also fail; next run starts fresh
            fi
        done
    done
}

build_and_run() {
    local label="$1"
    build_current_config "$label"
    run_kernel_benchmarks "$label"
    run_user_benchmarks "$label"
}

# ── parse log files → CSV rows ────────────────────────────────────────────

parse_kernel_log() {
    local logfile="$1" config_label="$2" run="$3"
    [ -f "$logfile" ] || return 0

    python3 - "$logfile" "$config_label" "$run" <<'PYEOF'
import sys, re

logfile = sys.argv[1]
config  = sys.argv[2]
run     = sys.argv[3]

with open(logfile) as f:
    lines = f.readlines()

pat_kmalloc = re.compile(
    r'\[TEST\]\s+(?P<mem>DRAM|CXL)\s+kmalloc avg throughput'
    r'\s+\(parallel=(?P<par>\d+)\):\s+(?P<ops>\d+)\s+ops/s'
)
pat_gp_tp = re.compile(
    r'\[TEST\]\s+(?P<phase>get_pages|free_pages) throughput'
    r'\s+\((?P<mem>DRAM|CXL),\s*(?P<bytes>\d+)B\):\s+(?P<ops>\d+)\s+ops/s'
)
pat_random = re.compile(
    r'\[TEST\]\s+(?P<mem>DRAM|CXL)\s+random get_pages/free_pages 4KiB\+2MiB'
    r'\s+iters=(?P<iters>\d+)\s+throughput=(?P<tp>\d+)\s+loop-iters/s'
    r'\s+get_4k=(?P<g4>\d+)\s+get_2m=(?P<g2>\d+)'
    r'\s+free_4k=(?P<f4>\d+)\s+free_2m=(?P<f2>\d+)'
)
pat_par_ctx = re.compile(r'\[TEST\]\s+start malloc test parallel=(?P<par>\d+)')

def sz_label(b):
    b = int(b)
    if b == 4096: return "4KB"
    if b >= 2*1024*1024-1: return "2MB"
    return f"{b}B"

cur_par = 1
for line in lines:
    m = pat_par_ctx.search(line)
    if m:
        cur_par = int(m.group('par'))
        continue
    m = pat_kmalloc.search(line)
    if m:
        print(f"{config},{m.group('mem')},kmalloc,{m.group('par')},{run},{m.group('ops')}")
        continue
    m = pat_gp_tp.search(line)
    if m:
        phase = "alloc" if m.group('phase') == "get_pages" else "free"
        sz    = sz_label(m.group('bytes'))
        print(f"{config},{m.group('mem')},get_pages({sz})-{phase},{cur_par},{run},{m.group('ops')}")
        continue
    m = pat_random.search(line)
    if m:
        print(f"{config},{m.group('mem')},random_get_free_4K2M,{cur_par},{run},{m.group('tp')}")
        continue
PYEOF
}

parse_user_log() {
    local logfile="$1" config_label="$2" run="$3"
    [ -f "$logfile" ] || return 0

    python3 - "$logfile" "$config_label" "$run" <<'PYEOF'
import sys, re

logfile = sys.argv[1]
config  = sys.argv[2]
run     = sys.argv[3]

with open(logfile) as f:
    lines = f.readlines()

pat_ub_hdr = re.compile(
    r'(?P<name>\S+)\s+(?P<threads>\d+)\s+threads\s+(?P<mode>random|fixed)'
    r'\s+(?P<sz_mode>\S+)\s+size\s+\[(?P<min>\d+),(?P<max>\d+)\]'
)
pat_ub_tp = re.compile(r'throughput\(ops/s\):\s*(?P<tp>\d+)')

ub_ctx = {}
for line in lines:
    m = pat_ub_hdr.search(line)
    if m:
        ub_ctx = {'threads': m.group('threads'),
                  'sz_mode': m.group('sz_mode'),
                  'min': m.group('min'), 'max': m.group('max')}
        continue
    m = pat_ub_tp.search(line)
    if m and ub_ctx:
        sz_info = f"{ub_ctx['sz_mode']}[{ub_ctx['min']}-{ub_ctx['max']}]"
        print(f"{config},user,user_malloc({sz_info}),{ub_ctx['threads']},{run},{m.group('tp')}")
        ub_ctx = {}
        continue
PYEOF
}

# ── main ──────────────────────────────────────────────────────────────────

bench_malloc_main() {
echo "[E2E] Log directory : $LOG_DIR"
echo "[E2E] CSV output    : $CSV_OUT"
echo "[E2E] Runs          : $NRUNS  (offset=$RUN_OFFSET)"
echo "[E2E] User threads  : $USER_BENCH_THREADS"
echo "[E2E] Bench timeout : ${USER_BENCH_TIMEOUT}s per QEMU session"
echo ""
echo "[E2E] Saving original config ..."
save_config

if [ "$APPEND_CSV" = "1" ]; then
    echo "[E2E] Appending to existing CSV: $CSV_OUT"
else
    echo "config,memory,test,parallel,run,ops_per_sec" > "$CSV_OUT"
fi

# Config 1: LLFree + CR=ON
set_cmake_var DSM_CXL_LF_BUDDY ON
set_cmake_var SLAB_CRASH_RECOVERY ON
build_and_run "llfree_cr_on"

# Config 2: LLFree + CR=OFF
set_cmake_var DSM_CXL_LF_BUDDY ON
set_cmake_var SLAB_CRASH_RECOVERY OFF
build_and_run "llfree_cr_off"

# Config 3: Buddy + CR=OFF
set_cmake_var DSM_CXL_LF_BUDDY OFF
set_cmake_var SLAB_CRASH_RECOVERY OFF
build_and_run "buddy_cr_off"

echo "[E2E] Restoring original config ..."
restore_config

# ── Parse all saved logs → CSV ────────────────────────────────────────────
echo ""
echo "[E2E] Parsing all logs from $LOG_DIR ..."
for entry in "LLFree+CR:llfree_cr_on" "LLFree:llfree_cr_off" "Buddy:buddy_cr_off"; do
    cfg="${entry%%:*}"
    lbl="${entry##*:}"
    for run in $(seq 1 "$NRUNS"); do
        abs_run=$(( run + RUN_OFFSET ))
        parse_kernel_log "$LOG_DIR/${lbl}_run${abs_run}_kernel.log" "$cfg" "$abs_run" >> "$CSV_OUT"
        for threads in $USER_BENCH_THREADS; do
            parse_user_log "$LOG_DIR/${lbl}_run${abs_run}_user_t${threads}.log" "$cfg" "$abs_run" >> "$CSV_OUT"
        done
    done
done

echo ""
echo "========================================"
echo "[E2E] All done!  CSV : $CSV_OUT"
echo "[E2E] Logs        : $LOG_DIR/"
echo "========================================"
echo ""
echo "Preview (first 20 rows):"
head -21 "$CSV_OUT" | column -t -s,
echo "..."
echo "Total data rows: $(( $(wc -l < "$CSV_OUT") - 1 ))"
}

if [ "${BENCH_MALLOC_LIB_ONLY:-0}" != "1" ]; then
    bench_malloc_main
fi
