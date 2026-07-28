#!/usr/bin/env python3
"""Sample host NUMA placement of QEMU vCPU threads during a DSM run.

Periodically records, for every QEMU vCPU thread, the host CPU it last ran on
(field 39 of /proc/<pid>/task/<tid>/stat) and the NUMA node that CPU belongs to.
Also snapshots per-process memory distribution via numastat.

QEMU instances are identified by the "-name chcore-<vm_id>" option, vCPU threads
by their "CPU <n>/KVM" comm.  With GUEST_CPUS per machine the guest-global CPU
id is vm_id * GUEST_CPUS + n, which is what DBx1000's bind banner reports.

Usage:
    sample_qemu_numa.py --out placement.jsonl [--interval 0.25] [--duration 600]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

NAME_RE = re.compile(r'-name\s+chcore-(\d+)')
VCPU_RE = re.compile(r'^CPU (\d+)/(?:KVM|TCG)$')


def cpu_to_node():
    """Map host CPU id -> NUMA node by walking /sys/devices/system/node."""
    mapping = {}
    base = '/sys/devices/system/node'
    for entry in os.listdir(base):
        if not entry.startswith('node'):
            continue
        node = int(entry[4:])
        for f in os.listdir(os.path.join(base, entry)):
            if f.startswith('cpu') and f[3:].isdigit():
                mapping[int(f[3:])] = node
    return mapping


def find_qemu():
    """Return {vm_id: pid} for running QEMU instances."""
    out = {}
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            with open(f'/proc/{pid}/cmdline', 'rb') as fh:
                cmd = fh.read().replace(b'\0', b' ').decode('utf-8', 'replace')
        except OSError:
            continue
        if 'qemu' not in cmd or 'chcore-' not in cmd:
            continue
        m = NAME_RE.search(cmd)
        if m:
            out[int(m.group(1))] = int(pid)
    return out


def vcpu_threads(pid, guest_cpus):
    """Return {guest_cpu_index: tid} for a QEMU pid.

    Prefers the "CPU <n>/KVM" thread names, which QEMU only sets when started
    with -name ...,debug-threads=on.  Without that every thread inherits the
    process name, so fall back on creation order: QEMU spawns the vCPU threads
    consecutively during startup, so the longest run of consecutive tids of
    length guest_cpus (excluding the main thread and kernel helpers such as
    kvm-nx-lpage-recovery) is the vCPU set, in guest-CPU order.
    """
    res = {}
    try:
        tids = sorted(int(t) for t in os.listdir(f'/proc/{pid}/task'))
    except OSError:
        return res

    named = {}
    others = []
    for tid in tids:
        try:
            with open(f'/proc/{pid}/task/{tid}/comm') as fh:
                comm = fh.read().strip()
        except OSError:
            continue
        m = VCPU_RE.match(comm)
        if m:
            named[int(m.group(1))] = tid
        elif tid != pid and not comm.startswith('kvm-'):
            others.append(tid)
    if named:
        return named

    # Fallback: longest consecutive tid run of exactly guest_cpus threads.
    best = []
    run = []
    for tid in others:
        if run and tid == run[-1] + 1:
            run.append(tid)
        else:
            run = [tid]
        if len(run) > len(best):
            best = list(run)
    if len(best) >= guest_cpus:
        best = best[:guest_cpus]
        for i, tid in enumerate(best):
            res[i] = tid
    return res


def last_cpu(pid, tid):
    """Field 39 (1-based) of /proc/<pid>/task/<tid>/stat is the last-run CPU."""
    try:
        with open(f'/proc/{pid}/task/{tid}/stat') as fh:
            data = fh.read()
    except OSError:
        return None
    # comm is parenthesised and may contain spaces/slashes -- split after it.
    rp = data.rfind(')')
    if rp < 0:
        return None
    fields = data[rp + 2:].split()
    # fields[0] is field 3 (state), so field 39 is fields[36].
    try:
        return int(fields[36])
    except (IndexError, ValueError):
        return None


def numastat(pid):
    try:
        out = subprocess.run(['numastat', '-p', str(pid)],
                             capture_output=True, text=True, timeout=30)
        return out.stdout
    except (OSError, subprocess.SubprocessError):
        return ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--interval', type=float, default=0.25)
    ap.add_argument('--duration', type=float, default=1800)
    ap.add_argument('--guest-cpus', type=int, default=12)
    ap.add_argument('--numastat-every', type=float, default=30,
                    help='seconds between numastat snapshots (0 disables)')
    args = ap.parse_args()

    c2n = cpu_to_node()
    deadline = time.time() + args.duration
    last_numastat = 0.0

    with open(args.out, 'w', buffering=1) as fh:
        fh.write(json.dumps({'type': 'meta', 'cpu_to_node': c2n,
                             'guest_cpus': args.guest_cpus,
                             'start': time.time()}) + '\n')
        while time.time() < deadline:
            qemus = find_qemu()
            if qemus:
                sample = {}
                for vm_id, pid in sorted(qemus.items()):
                    for gcpu, tid in sorted(vcpu_threads(pid, args.guest_cpus).items()):
                        cpu = last_cpu(pid, tid)
                        if cpu is None:
                            continue
                        gid = vm_id * args.guest_cpus + gcpu
                        sample[gid] = [cpu, c2n.get(cpu, -1)]
                if sample:
                    fh.write(json.dumps({'type': 'sample', 't': time.time(),
                                         'cpus': sample}) + '\n')
                now = time.time()
                if args.numastat_every and now - last_numastat >= args.numastat_every:
                    last_numastat = now
                    for vm_id, pid in sorted(qemus.items()):
                        fh.write(json.dumps({'type': 'numastat', 't': now,
                                             'vm_id': vm_id, 'pid': pid,
                                             'text': numastat(pid)}) + '\n')
            time.sleep(args.interval)
    return 0


if __name__ == '__main__':
    sys.exit(main())
