#!/usr/bin/env python3
"""Correlate DBx1000 fast/slow workers against host NUMA placement.

Takes the JSONL produced by sample_qemu_numa.py plus one or more DBx1000 logs
containing per-worker "[tid=N] txn_cnt=..." lines, and reports whether the
bimodal fast/slow split lines up with the host NUMA node each worker's vCPU
thread actually ran on.

Worker->guest CPU: worker i on machine m runs on guest CPU m*GUEST_CPUS+i,
which is what DBx1000's "bind 64 cpu:" banner lists.

Usage:
    correlate_numa_placement.py placement.jsonl run.log [run.log ...]
"""
import argparse
import collections
import json
import re
import statistics
import sys

TXN_RE = re.compile(r'\[tid=(\d+)\] txn_cnt=(\d+)')


def load_placement(path, t0=None, t1=None):
    """Return {guest_cpu: Counter(node)} over samples within [t0, t1]."""
    per_cpu = collections.defaultdict(collections.Counter)
    meta = {}
    with open(path) as fh:
        for line in fh:
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if rec.get('type') == 'meta':
                meta = rec
            elif rec.get('type') == 'sample':
                t = rec['t']
                if (t0 is not None and t < t0) or (t1 is not None and t > t1):
                    continue
                for gid, (cpu, node) in rec['cpus'].items():
                    per_cpu[int(gid)][node] += 1
    return per_cpu, meta


def load_txn(path):
    d = {}
    for m in TXN_RE.finditer(open(path, errors='replace').read()):
        d[int(m.group(1))] = int(m.group(2))
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('placement')
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--guest-cpus', type=int, default=12)
    ap.add_argument('--threads-per-machine', type=int, default=8)
    ap.add_argument('--start', type=float, default=None)
    ap.add_argument('--end', type=float, default=None)
    args = ap.parse_args()

    per_cpu, meta = load_placement(args.placement, args.start, args.end)
    if not per_cpu:
        print('no placement samples in window', file=sys.stderr)
        return 1

    for log in args.logs:
        txn = load_txn(log)
        if len(txn) < 8:
            continue
        med = statistics.median(txn.values())
        print(f'\n=== {log} ===')
        print(f'{len(txn)} workers, median txn_cnt={med:.0f}')

        rows = []
        for wid, cnt in sorted(txn.items()):
            m, i = divmod(wid, args.threads_per_machine)
            gcpu = m * args.guest_cpus + i
            nodes = per_cpu.get(gcpu)
            if not nodes:
                continue
            total = sum(nodes.values())
            modal, modal_n = nodes.most_common(1)[0]
            rows.append((wid, cnt, cnt > 2 * med, modal, modal_n / total, nodes))

        if not rows:
            print('  no placement data for these workers')
            continue

        # Fast/slow breakdown per modal node.
        by_node = collections.defaultdict(lambda: [0, 0])
        for _, _, fast, modal, _, _ in rows:
            by_node[modal][0 if fast else 1] += 1
        print('  modal node   fast  slow   fast%')
        for node in sorted(by_node):
            f, s = by_node[node]
            print(f'  node {node:<8d} {f:4d}  {s:4d}   {100*f/(f+s):5.1f}%')

        # Mean throughput per modal node.
        print('  modal node   n   mean txn_cnt')
        agg = collections.defaultdict(list)
        for _, cnt, _, modal, _, _ in rows:
            agg[modal].append(cnt)
        for node in sorted(agg):
            v = agg[node]
            print(f'  node {node:<8d} {len(v):3d}  {statistics.mean(v):10.0f}')

        # Residency stability: how pinned were threads to their modal node?
        stab = [r[4] for r in rows]
        print(f'  modal-node residency: mean={statistics.mean(stab)*100:.1f}% '
              f'min={min(stab)*100:.1f}%')

        fastrows = [r for r in rows if r[2]]
        slowrows = [r for r in rows if not r[2]]
        if fastrows and slowrows:
            print(f'  fast workers: n={len(fastrows)} '
                  f'modal nodes={collections.Counter(r[3] for r in fastrows).most_common()}')
            print(f'  slow workers: n={len(slowrows)} '
                  f'modal nodes={collections.Counter(r[3] for r in slowrows).most_common()}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
