#!/usr/bin/env python3
"""Total CXL versus DRAM traffic from a measure_mem_traffic.sh capture.

Reports both the whole-capture totals and a per-second series, because a run
spends most of its wall time loading the table and only a few seconds in the
measured window -- the totals alone hide which phase the traffic came from.

Host background traffic is subtracted from the DRAM column using the idle
baseline; the CXL column needs no correction (it reads zero when idle).
"""
import argparse
import csv
import sys
from pathlib import Path

LINE_BYTES = 64  # a raw m2s_*/cas_count_* transaction moves one cache line
MIB = 1 << 20


def side(event):
    """CXL device counters versus socket memory-controller counters."""
    return "cxl" if event.startswith("cxl_pmu_") else "dram"


def to_bytes(value, unit):
    """perf applies each event's .scale, so the two PMUs arrive in different
    units: the IMC events carry a MiB scale and are already converted, while the
    CXL events have no scale file and stay as raw transaction counts."""
    if unit == "MiB":
        return value * MIB
    if unit:
        return None  # an unexpected unit would silently corrupt the totals
    return value * LINE_BYTES


def read_rows(path):
    """Yield (timestamp_or_None, event, bytes) from a perf -x, file.

    Interval mode prepends a timestamp column and plain mode does not, so the
    event column is located by content -- an event name always contains '/' --
    rather than by a fixed index.
    """
    for row in csv.reader(path.read_text().splitlines()):
        if not row or row[0].startswith("#"):
            continue
        ev_idx = next((i for i, f in enumerate(row) if "/" in f), None)
        if ev_idx is None or ev_idx < 2:
            continue
        raw = row[ev_idx - 2].strip()
        if not raw or raw.startswith("<"):  # <not counted> / <not supported>
            continue
        try:
            value = float(raw)
        except ValueError:
            continue
        nbytes = to_bytes(value, row[ev_idx - 1].strip())
        if nbytes is None:
            continue
        ts = None
        if ev_idx >= 3:
            try:
                ts = float(row[0])
            except ValueError:
                ts = None
        yield ts, row[ev_idx].strip(), nbytes


def main(outdir):
    d = Path(outdir)

    base_rate = {"cxl": 0.0, "dram": 0.0}
    base_path = d / "baseline.csv"
    secs_path = d / "baseline_secs.txt"
    if base_path.exists() and secs_path.exists():
        # Written by measure_mem_traffic.sh: `perf -x,` emits no elapsed-time
        # line, and the runtime column is summed over the aggregated per-socket
        # PMU instances, so it cannot stand in for wall time.
        try:
            secs = float(secs_path.read_text().strip())
        except ValueError:
            secs = 0.0
        totals = {"cxl": 0.0, "dram": 0.0}
        for _, ev, b in read_rows(base_path):
            totals[side(ev)] += b
        if secs > 0:
            base_rate = {k: v / secs for k, v in totals.items()}

    series = {}
    for ts, ev, b in read_rows(d / "traffic.csv"):
        if ts is None:
            continue
        series.setdefault(ts, {"cxl": 0.0, "dram": 0.0})[side(ev)] += b

    if not series:
        print("no interval samples in traffic.csv", file=sys.stderr)
        return 1

    stamps = sorted(series)
    # perf's interval timestamps are relative to capture start.  The first row
    # therefore covers 0..stamps[0], not a zero-width point at stamps[0].
    # Using last-first would omit that entire first interval from baseline
    # subtraction and overstate the corrected traffic.
    durations = {}
    previous = 0.0
    for ts in stamps:
        durations[ts] = ts - previous
        previous = ts
    if any(duration <= 0 for duration in durations.values()):
        print("non-increasing interval timestamps in traffic.csv", file=sys.stderr)
        return 1
    span = stamps[-1]

    raw = {k: sum(s[k] for s in series.values()) for k in ("cxl", "dram")}
    corrected = {
        k: max(raw[k] - base_rate[k] * span, 0.0) for k in ("cxl", "dram")
    }

    print(f"capture span: {span:.0f}s  ({len(stamps)} samples)")
    print(f"idle baseline: cxl {base_rate['cxl'] / 1e9:.3f} GB/s  "
          f"dram {base_rate['dram'] / 1e9:.3f} GB/s")
    print()
    print(f"{'':10} {'raw GB':>12} {'minus idle GB':>15} {'share':>8}")
    tot = corrected["cxl"] + corrected["dram"]
    for k in ("cxl", "dram"):
        share = 100.0 * corrected[k] / tot if tot else 0.0
        print(f"{k.upper():10} {raw[k] / 1e9:>12.1f} {corrected[k] / 1e9:>15.1f} "
              f"{share:>7.1f}%")
    print()

    # Per-second series, so the load phase and the measured window can be told
    # apart. Printed coarsely to stay readable over a multi-minute run.
    print(f"{'t(s)':>6} {'CXL GB/s':>10} {'DRAM GB/s':>10}")
    every = max(1, len(stamps) // 40)
    for i, ts in enumerate(stamps):
        if i % every:
            continue
        s = series[ts]
        duration = durations[ts]
        dram = max(s["dram"] - base_rate["dram"] * duration, 0.0)
        print(f"{ts - stamps[0]:>6.0f} "
              f"{s['cxl'] / duration / 1e9:>10.2f} "
              f"{dram / duration / 1e9:>10.2f}")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("outdir", help="capture directory from measure_mem_traffic.sh")
    sys.exit(main(parser.parse_args().outdir))
