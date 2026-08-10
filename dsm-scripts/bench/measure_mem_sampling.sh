#!/usr/bin/env sh
# DOES NOT WORK UNDER KVM -- kept only as a record of what was tried.
#
# perf mem record sets exclude_guest=1, so guest memory accesses are never
# sampled; what lands in the capture is host-side work (QEMU, KVM overhead).
# An 8-machine capture produced 123 KVM RAM-level samples and none on CXL, while
# the guest had ~44% of its pages there.  (An earlier 148k/one-CXL count also
# included cache-hit samples and was invalid for that additional reason.)
# Enabling guest sampling would not fix it either: the address PEBS records
# during guest execution is a guest address that the host cannot translate.
# Measured 2026-08-05; see docs/10-cxl-access-weighting.md for the full
# elimination and for the approach that does work (in-guest accounting via
# CXL_ACCESS_PROF).
#
# Run it anyway with MEM_SAMPLING_I_KNOW_IT_IS_BROKEN=1 to sample host-side
# memory behaviour, which is all it can see.
#
# ----------------------------------------------------------------------------
#
# Sample where a run's memory accesses land: CXL versus socket-local DRAM.
#
# The device-side counters cannot answer this here: cxl_pmu reports exactly
# zero even under a direct 12GB load against the CXL nodes, and that traffic is
# equally invisible to the socket IMC boxes.  PEBS looked like the way out --
# each sample carries the physical address of the access, and sysfs gives every
# NUMA node's physical range, so samples can be bucketed by node -- but under
# KVM it cannot see the guest at all, which is what the banner above records.
#
# The parser requires perf's data source to report a RAM hit before attributing
# the physical address, so cache hits with a valid physical address are not
# mistaken for traffic that reached a memory device.
#
# A fixed period (-c) is used rather than a fixed frequency (-F): frequency mode
# samples every CPU at the same rate regardless of how much memory traffic it
# generates, which would bias the ratio whenever CPUs differ in their mix.  With
# a fixed period, samples are proportional to actual events.
#
# Usage: measure_mem_sampling.sh <outdir> <command...>
set -u

if [ "${MEM_SAMPLING_I_KNOW_IT_IS_BROKEN:-0}" != "1" ]; then
    cat >&2 <<'EOF'
measure_mem_sampling.sh: refusing to run.

perf mem record sets exclude_guest=1, so this samples the host, not the guest
workload -- it returns a precise-looking CXL share computed from the wrong
accesses.  Use the in-guest profiler instead (CXL_ACCESS_PROF in
user/demos/dbx1000/config.h); see docs/10-cxl-access-weighting.md.

Set MEM_SAMPLING_I_KNOW_IT_IS_BROKEN=1 to sample host-side behaviour anyway.
EOF
    exit 2
fi

OUTDIR="${1:?usage: measure_mem_sampling.sh <outdir> <command...>}"
shift
[ "$#" -gt 0 ] || {
    echo "usage: measure_mem_sampling.sh <outdir> <command...>" >&2
    exit 2
}
mkdir -p "$OUTDIR"

# ~1600 samples per GB of reads at period 10000, so 100000 lands a memory-heavy
# multi-minute run in the 1e5-1e6 range: tight enough for a sub-0.2pp standard
# error on the ratio, small enough to parse quickly.
PERIOD="${MEM_SAMPLING_PERIOD:-100000}"
DATA="$OUTDIR/perf.data"

echo "$PERIOD" > "$OUTDIR/period.txt"

# perf record finalises its output on SIGINT; escalate only if it does not.
stop_perf() {
    [ -n "${PERF_PID:-}" ] || return 0
    kill -INT "$PERF_PID" 2>/dev/null || return 0
    for _ in $(seq 1 40); do
        kill -0 "$PERF_PID" 2>/dev/null || { PERF_PID=""; return 0; }
        sleep 0.5
    done
    kill -KILL "$PERF_PID" 2>/dev/null || true
    PERF_PID=""
}

echo "[mem-sampling] recording with period $PERIOD -> $DATA"
perf mem record --phys-data -a -c "$PERIOD" -o "$DATA" -- sleep 86400 \
    > "$OUTDIR/perf-record.log" 2>&1 &
PERF_PID=$!
trap 'stop_perf' EXIT INT TERM

# Fail loudly rather than running the whole benchmark with no capture attached.
sleep 3
if ! kill -0 "$PERF_PID" 2>/dev/null; then
    echo "[mem-sampling] perf failed to start:" >&2
    cat "$OUTDIR/perf-record.log" >&2
    exit 1
fi

"$@"
RC=$?

stop_perf
trap - EXIT INT TERM

echo "[mem-sampling] command exited $RC; capture in $DATA"
exit "$RC"
