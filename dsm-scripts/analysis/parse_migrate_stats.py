#!/usr/bin/env python3
"""Summarize the DSM_MIGRATE_STATS counters in a set of machine logs.

The kernel prints one cumulative line per machine roughly twice a second:

    [DMS] m=1 alloc=164794 direct=486148 c21=0 c22=136 c23=3396 \
          c23pg=55928 c23raced=0 c23cyc=14794593318 c22cyc=339325452

Counters are monotonic within a boot, so the last line of a log is that
machine's total.  A point's total is the sum over its machines.

Usage:
    parse_migrate_stats.py <log-dir> [--csv out.csv]

Log files are expected to be named machine<N>-<point>.log, which is what
dsm-scripts/bench/run_state_partition_probe.sh writes.  Anything else is
grouped under its own filename.
"""
import argparse
import csv
import re
import sys
from collections import defaultdict
from pathlib import Path

DMS_RE = re.compile(
    r"\[DMS\] m=(?P<m>\d+) "
    r"alloc=(?P<alloc>\d+) direct=(?P<direct>\d+) "
    r"c21=(?P<c21>\d+) c22=(?P<c22>\d+) c23=(?P<c23>\d+) "
    r"c23pg=(?P<c23pg>\d+) c23raced=(?P<c23raced>\d+) "
    r"c23cyc=(?P<c23cyc>\d+) c22cyc=(?P<c22cyc>\d+)"
)
FIELDS = ["alloc", "direct", "c21", "c22", "c23", "c23pg", "c23raced",
          "c23cyc", "c22cyc"]
# Phoenix prints the measured region as "library: <us>"; pca prints it twice
# (it sums two map_reduce calls into one total), so the last one is the total.
LIB_RE = re.compile(r"^library: (\d+)", re.M)
NAME_RE = re.compile(r"^machine(?P<m>\d+)-(?P<point>.+)\.log$")


def last_counters(text):
    """Final cumulative counter line of a log, or None if never printed."""
    last = None
    for match in DMS_RE.finditer(text):
        last = match
    if last is None:
        return None
    return {f: int(last.group(f)) for f in FIELDS}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log_dir", type=Path)
    ap.add_argument("--csv", type=Path,
                    help="also write the per-point table here")
    args = ap.parse_args()

    per_point = defaultdict(lambda: defaultdict(int))
    machines = defaultdict(set)
    library = {}
    missing = []

    for path in sorted(args.log_dir.glob("*.log")):
        name = NAME_RE.match(path.name)
        point = name.group("point") if name else path.stem
        mid = name.group("m") if name else "?"
        text = path.read_text(errors="replace")

        counters = last_counters(text)
        if counters is None:
            missing.append(path.name)
        else:
            machines[point].add(mid)
            for field in FIELDS:
                per_point[point][field] += counters[field]

        # The workload only runs on machine 0, so that is where the timing is.
        lib = LIB_RE.findall(text)
        if lib:
            library[point] = int(lib[-1])

    if not per_point:
        print(f"no [DMS] lines under {args.log_dir}; was the kernel built "
              f"with DSM_MIGRATE_STATS?", file=sys.stderr)
        return 1

    header = ["point", "machines", "library_us"] + FIELDS + ["cyc_per_pg"]
    rows = []
    for point in sorted(per_point):
        totals = per_point[point]
        pages = totals["c23pg"]
        rows.append([
            point,
            len(machines[point]),
            library.get(point, ""),
            *[totals[f] for f in FIELDS],
            round(totals["c23cyc"] / pages, 1) if pages else "",
        ])

    widths = [max(len(str(r[i])) for r in ([header] + rows))
              for i in range(len(header))]
    for row in [header] + rows:
        print("  ".join(str(c).ljust(w) for c, w in zip(row, widths)).rstrip())

    if args.csv:
        with args.csv.open("w", newline="") as fh:
            writer = csv.writer(fh)
            writer.writerow(header)
            writer.writerows(rows)
        print(f"\nwrote {args.csv}")

    if missing:
        print(f"\n{len(missing)} log(s) with no counter line: "
              f"{', '.join(missing[:5])}"
              f"{' ...' if len(missing) > 5 else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
