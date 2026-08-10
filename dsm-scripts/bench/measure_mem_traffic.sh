#!/usr/bin/env sh
# DOES NOT WORK ON THIS HOST -- kept only as a record of what was tried.
#
# cxl_pmu_mem{0,1}.0 counts exactly zero here even under a direct 12GB read and
# write against the CXL nodes, with perf reporting the events 100% enabled, so
# this script reports "CXL 0%" no matter what the workload does.  The socket IMC
# counters do not see that traffic either.  Measured 2026-08-05; see
# docs/10-cxl-access-weighting.md for the full elimination and for the approach
# that does work (in-guest accounting via CXL_ACCESS_PROF).
#
# Run it anyway with MEM_TRAFFIC_I_KNOW_IT_IS_BROKEN=1 if you want to re-test
# whether a firmware or driver update has fixed the device PMU.
#
# ----------------------------------------------------------------------------
#
# Measure how much traffic a run sends to CXL versus to local DRAM.
#
# The guests run under KVM, so the guest kernel never sees an access to an
# already-mapped page and cannot count them.  The host can: the shared region
# lives on the CPU-less CXL nodes (devices mem0/mem1) and guest-local DRAM lives
# on the socket nodes, and those are reached over different hardware paths.
#
#   CXL   = cxl_pmu_mem{0,1}.0 m2s_req_memrd + m2s_rwd_memwr (+ partial writes)
#   DRAM  = uncore_imc_0..7    cas_count_read + cas_count_write
#
# Both count 64-byte transactions, so the two sides are directly comparable.
#
# These are cache-miss counts, not instruction counts: an access that hits in
# CPU cache never reaches either counter.  That is the right quantity for "what
# did CXL cost us", but it is not "how many rows the workload touched".
#
# The counters are host-wide, so anything else running on this machine lands in
# the DRAM column.  An idle baseline is sampled first so that background traffic
# can be subtracted; the CXL column reads a clean zero when nothing is running.
#
# Usage: measure_mem_traffic.sh <outdir> <command...>
set -eu

if [ "${MEM_TRAFFIC_I_KNOW_IT_IS_BROKEN:-0}" != "1" ]; then
    cat >&2 <<'EOF'
measure_mem_traffic.sh: refusing to run.

cxl_pmu reads zero on this host even under a direct load against the CXL nodes,
so this script produces a confident "CXL 0%" that is simply wrong.  Use the
in-guest profiler instead (CXL_ACCESS_PROF in user/demos/dbx1000/config.h);
see docs/10-cxl-access-weighting.md.

Set MEM_TRAFFIC_I_KNOW_IT_IS_BROKEN=1 to re-test the device PMU anyway.
EOF
    exit 2
fi

OUTDIR="${1:?usage: measure_mem_traffic.sh <outdir> <command...>}"
shift
[ "$#" -gt 0 ] || {
    echo "usage: measure_mem_traffic.sh <outdir> <command...>" >&2
    exit 2
}
mkdir -p "$OUTDIR"

BASELINE_SECS="${MEM_TRAFFIC_BASELINE_SECS:-20}"
INTERVAL_MS="${MEM_TRAFFIC_INTERVAL_MS:-1000}"

# Enumerate the boxes that exist rather than hard-coding a count, so this still
# works on a host with a different number of controllers or CXL devices.
events=""

# Read find's output through a here-document instead of using a shell glob.
# Bash leaves an unmatched glob literal in place, while Zsh rejects it by
# default; sysfs paths cannot contain newlines, so this is safe here.
while IFS= read -r d; do
    [ -n "$d" ] || continue
    [ -d "$d" ] || continue
    pmu=$(basename "$d")
    for ev in m2s_req_memrd m2s_rwd_memwr m2s_rwd_memwrptl; do
        [ -f "$d/events/$ev" ] && events="${events}${pmu}/${ev}/,"
    done
done <<EOF
$(find /sys/bus/event_source/devices -maxdepth 1 -type d -name 'cxl_pmu_mem*.0' -print)
EOF

while IFS= read -r d; do
    [ -n "$d" ] || continue
    [ -d "$d" ] || continue
    pmu=$(basename "$d")
    for ev in cas_count_read cas_count_write; do
        [ -f "$d/events/$ev" ] && events="${events}${pmu}/${ev}/,"
    done
done <<EOF
$(find /sys/bus/event_source/devices -maxdepth 1 -type d -name 'uncore_imc_[0-9]*' -print)
EOF
events="${events%,}"
[ -n "$events" ] || { echo "no usable memory PMUs on this host" >&2; exit 1; }

echo "$events" | tr ',' '\n' > "$OUTDIR/events.txt"
echo "[mem-traffic] $(echo "$events" | tr ',' '\n' | wc -l) counters"

echo "[mem-traffic] sampling idle baseline for ${BASELINE_SECS}s"
perf stat -a -e "$events" -x, -o "$OUTDIR/baseline.csv" -- sleep "$BASELINE_SECS"
# `perf -x,` writes no elapsed-time line, and the per-row runtime column is
# summed over the aggregated per-socket PMU instances (4 sockets => 4x wall
# time), so record the real duration here instead of inferring it.
echo "$BASELINE_SECS" > "$OUTDIR/baseline_secs.txt"

# Ask perf to finish, then escalate.  A plain `wait` can block indefinitely
# here: perf only flushes its output file when it decides to exit, so give it a
# bounded grace period and then kill it outright.
stop_perf() {
    [ -n "${PERF_PID:-}" ] || return 0
    kill -TERM "$PERF_PID" 2>/dev/null || return 0
    for _ in $(seq 1 20); do
        kill -0 "$PERF_PID" 2>/dev/null || { PERF_PID=""; return 0; }
        sleep 0.5
    done
    kill -KILL "$PERF_PID" 2>/dev/null || true
    PERF_PID=""
}

echo "[mem-traffic] starting interval capture"
perf stat -a -e "$events" -x, -I "$INTERVAL_MS" -o "$OUTDIR/traffic.csv" &
PERF_PID=$!
# Stop the capture on any exit path, otherwise a stray system-wide perf keeps
# running after this script is interrupted.
trap 'stop_perf' EXIT INT TERM

set +e
"$@"
RC=$?
set -e

stop_perf
trap - EXIT INT TERM

echo "[mem-traffic] command exited $RC; counters in $OUTDIR"
exit "$RC"
