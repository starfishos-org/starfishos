#!/usr/bin/env python3
"""Split sampled memory accesses between CXL and local DRAM.

Input is a `perf mem record --phys-data` capture (see measure_mem_sampling.sh).
PEBS can attach the physical address of an access, and sysfs gives the physical
range of every NUMA node, so memory-level samples with an address can be
attributed to CXL (the CPU-less nodes) or to socket-local DRAM.

Physical addresses are also present on some cache-hit samples, so the parser
explicitly requires a RAM level in perf's data source.  What is counted here is
therefore sampled traffic that actually reached a memory device, which is the
quantity CXL residency cannot distinguish.

This is a sample, not a census: the ratio is what it estimates well.  A 95%
Wilson interval is reported so that precision remains meaningful even when no
CXL samples (or no DRAM samples) are observed.
"""
import argparse
import math
import re
import subprocess
import sys
from bisect import bisect_right
from collections import defaultdict
from pathlib import Path

BLOCK_SIZE_PATH = "/sys/devices/system/memory/block_size_bytes"
# perf script prints "<comm> <pid> <time>:" before the event fields; comm itself
# may contain spaces (e.g. "CPU 0/KVM"), so anchor on the pid/time pair.
HEAD = re.compile(r"^(.*?)\s+(\d+)\s+(\d+\.\d+):")
MEMORY_LEVEL = re.compile(r"\|LVL (?:Remote )?RAM hit\|")


def node_ranges():
    """Physical address ranges per NUMA node, from the memory-block symlinks."""
    block = int(Path(BLOCK_SIZE_PATH).read_text().strip(), 16)
    ranges = []
    for nd in sorted(Path("/sys/devices/system/node").glob("node[0-9]*")):
        try:
            node = int(nd.name[4:])
        except ValueError:
            continue
        for entry in nd.glob("memory[0-9]*"):
            try:
                idx = int(entry.name[6:])
            except ValueError:
                continue
            ranges.append((idx * block, (idx + 1) * block, node))
    ranges.sort()
    return ranges


def make_lookup(ranges):
    starts = [r[0] for r in ranges]

    def lookup(addr):
        i = bisect_right(starts, addr) - 1
        if i < 0:
            return None
        start, end, node = ranges[i]
        return node if start <= addr < end else None

    return lookup


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("perf_data")
    ap.add_argument("--cxl-nodes", default="4,5",
                    help="comma-separated CPU-less nodes backing the CXL region")
    # Defaults to counting everything: QEMU's vCPU threads are named "CPU N/KVM",
    # not "qemu", so a comm filter aimed at the process name would drop exactly
    # the threads that touch guest memory.  The per-comm table below shows what
    # is actually in the capture so a filter can be chosen from evidence.
    ap.add_argument("--comm", default="",
                    help="only count samples whose comm contains this substring; "
                         "empty (default) counts everything")
    ap.add_argument("--bins", type=int, default=30,
                    help="time bins in the per-phase breakdown")
    args = ap.parse_args()

    cxl_nodes = {int(x) for x in args.cxl_nodes.split(",") if x.strip()}
    ranges = node_ranges()
    if not ranges:
        print("no NUMA memory ranges found in sysfs", file=sys.stderr)
        return 1
    lookup = make_lookup(ranges)

    proc = subprocess.Popen(
        ["perf", "script", "-i", args.perf_data,
         "-F", "comm,pid,time,data_src,phys_addr"],
        stdout=subprocess.PIPE, text=True)

    counts = defaultdict(int)
    per_node = defaultdict(int)
    per_comm = defaultdict(lambda: [0, 0])  # comm -> [cxl, dram]
    times = []
    other_level = 0
    no_addr = 0
    unmapped = 0
    total = 0

    for line in proc.stdout:
        m = HEAD.match(line)
        if not m:
            continue
        comm, _pid, ts = m.group(1).strip(), m.group(2), float(m.group(3))
        if args.comm and args.comm not in comm:
            continue
        total += 1
        if not MEMORY_LEVEL.search(line):
            other_level += 1
            continue
        tail = line.rsplit(None, 1)
        if len(tail) != 2:
            continue
        try:
            addr = int(tail[1], 16)
        except ValueError:
            continue
        if addr == 0:
            no_addr += 1
            continue
        node = lookup(addr)
        if node is None:
            unmapped += 1
            continue
        per_node[node] += 1
        side = "cxl" if node in cxl_nodes else "dram"
        counts[side] += 1
        per_comm[comm][0 if side == "cxl" else 1] += 1
        times.append((ts, side))

    if proc.wait() != 0:
        print("perf script failed", file=sys.stderr)
        return 1

    placed = counts["cxl"] + counts["dram"]
    print(f"samples matching comm~{args.comm!r}: {total}")
    print(f"  cache/other data-source level: {other_level}")
    print(f"  memory-level without phys addr: {no_addr}")
    print(f"  outside any node range:        {unmapped}")
    print(f"  attributed to a memory device: {placed}")
    if not placed:
        print("\nnothing to report", file=sys.stderr)
        return 1

    p = counts["cxl"] / placed
    z = 1.959963984540054
    denominator = 1 + z * z / placed
    center = (p + z * z / (2 * placed)) / denominator
    margin = (z / denominator
              * math.sqrt(p * (1 - p) / placed
                          + z * z / (4 * placed * placed)))
    print()
    print(f"{'':8} {'samples':>12} {'share':>9}")
    print(f"{'CXL':8} {counts['cxl']:>12} {100 * p:>8.2f}%")
    print(f"{'DRAM':8} {counts['dram']:>12} {100 * (1 - p):>8.2f}%")
    print(f"\n95% Wilson interval on CXL share: "
          f"{100 * (center - margin):.2f}%..{100 * (center + margin):.2f}%")

    print(f"\n{'node':>5} {'samples':>12} {'share':>9}")
    for node in sorted(per_node):
        tag = "CXL" if node in cxl_nodes else "DRAM"
        print(f"{node:>5} {per_node[node]:>12} "
              f"{100 * per_node[node] / placed:>8.2f}%  {tag}")

    # Which threads the samples came from -- the capture is system-wide, so this
    # is how unrelated host activity is told apart from the workload.
    print(f"\n{'comm':<20} {'CXL':>10} {'DRAM':>10} {'of total':>9}")
    top = sorted(per_comm.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))
    for comm, (c, dr) in top[:12]:
        print(f"{comm[:20]:<20} {c:>10} {dr:>10} "
              f"{100 * (c + dr) / placed:>8.2f}%")
    if len(top) > 12:
        rest = sum(c + dr for _, (c, dr) in top[12:])
        print(f"{'(' + str(len(top) - 12) + ' more)':<20} {'':>10} {'':>10} "
              f"{100 * rest / placed:>8.2f}%")

    # Time bins: a run spends most of its wall clock loading the table, so the
    # whole-capture ratio is not the ratio during the measured window.
    if times and args.bins > 1:
        # perf may merge per-CPU buffers out of timestamp order (and warns when
        # it does), so the stream's first and last records are not reliable
        # capture boundaries.
        t0 = min(ts for ts, _ in times)
        t1 = max(ts for ts, _ in times)
        width = (t1 - t0) / args.bins or 1.0
        bins = defaultdict(lambda: [0, 0])
        for ts, side in times:
            b = min(int((ts - t0) / width), args.bins - 1)
            bins[b][0 if side == "cxl" else 1] += 1
        print(f"\n{'t(s)':>7} {'CXL':>10} {'DRAM':>10} {'CXL share':>10}")
        for b in sorted(bins):
            c, dr = bins[b]
            tot = c + dr
            print(f"{b * width:>7.0f} {c:>10} {dr:>10} "
                  f"{100 * c / tot if tot else 0:>9.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
