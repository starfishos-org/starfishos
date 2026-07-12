#!/usr/bin/env python3
"""
Parse all kernel + user malloc logs from logs/malloc/ into a CSV,
then invoke plot_bench_malloc.py to regenerate the figures.

Usage: python3 dsm-scripts/parse_malloc_logs.py [log_dir] [output.csv]
"""

import sys, re, os, math, pathlib

LOG_DIR = sys.argv[1] if len(sys.argv) > 1 else "logs/malloc"
CSV_OUT = sys.argv[2] if len(sys.argv) > 2 else "bench_malloc_results.csv"

CONFIGS = [
    ("LLFree+CR", "llfree_cr_on"),
    ("LLFree",    "llfree_cr_off"),
    ("Buddy",     "buddy_cr_off"),
]
RUNS    = [1, 2, 3]
THREADS = [1, 2, 4, 8, 16, 32, 64, 96]

# ── kernel log patterns ────────────────────────────────────────────────────
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
)
pat_par_ctx = re.compile(r'\[TEST\]\s+start malloc test parallel=(?P<par>\d+)')

def sz_label(b):
    b = int(b)
    if b == 4096:       return "4KB"
    if b >= 2*1024*1024-1: return "2MB"
    return f"{b}B"

def parse_kernel_log(path, config, run):
    rows = []
    cur_par = 1
    with open(path) as f:
        for line in f:
            m = pat_par_ctx.search(line)
            if m:
                cur_par = int(m.group('par'))
                continue
            m = pat_kmalloc.search(line)
            if m:
                rows.append((config, m.group('mem'), 'kmalloc',
                             int(m.group('par')), run, int(m.group('ops'))))
                continue
            m = pat_gp_tp.search(line)
            if m:
                phase = "alloc" if m.group('phase') == 'get_pages' else "free"
                sz    = sz_label(m.group('bytes'))
                rows.append((config, m.group('mem'), f"get_pages({sz})-{phase}",
                             cur_par, run, int(m.group('ops'))))
                continue
            m = pat_random.search(line)
            if m:
                rows.append((config, m.group('mem'), 'random_get_free_4K2M',
                             cur_par, run, int(m.group('tp'))))
    return rows

# ── user log patterns ──────────────────────────────────────────────────────
pat_ub_hdr = re.compile(
    r'(?P<name>\S+)\s+(?P<threads>\d+)\s+threads\s+(?P<mode>random|fixed)'
    r'\s+(?P<sz_mode>\S+)\s+size\s+\[(?P<min>\d+),(?P<max>\d+)\]'
)
pat_ub_tp = re.compile(r'throughput\(ops/s\):\s*(?P<tp>\d+)')

def parse_user_log(path, config, run):
    rows = []
    ub_ctx = {}
    with open(path) as f:
        for line in f:
            m = pat_ub_hdr.search(line)
            if m:
                ub_ctx = {'threads': int(m.group('threads')),
                          'sz_mode': m.group('sz_mode'),
                          'min': m.group('min'), 'max': m.group('max')}
                continue
            m = pat_ub_tp.search(line)
            if m and ub_ctx:
                sz_info = f"{ub_ctx['sz_mode']}[{ub_ctx['min']}-{ub_ctx['max']}]"
                rows.append((config, 'user', f"user_malloc({sz_info})",
                             ub_ctx['threads'], run, int(m.group('tp'))))
                ub_ctx = {}
    return rows

# ── collect ────────────────────────────────────────────────────────────────
all_rows = []
log_dir  = pathlib.Path(LOG_DIR)

for cfg_label, file_prefix in CONFIGS:
    for run in RUNS:
        # kernel log
        klog = log_dir / f"{file_prefix}_run{run}_kernel.log"
        if klog.exists():
            r = parse_kernel_log(klog, cfg_label, run)
            all_rows.extend(r)
            print(f"  kernel {klog.name}: {len(r)} rows")
        else:
            print(f"  [MISSING] {klog.name}")

        # user bench logs (one per thread count)
        for t in THREADS:
            ulog = log_dir / f"{file_prefix}_run{run}_user_t{t}.log"
            if ulog.exists():
                r = parse_user_log(ulog, cfg_label, run)
                all_rows.extend(r)
            else:
                print(f"  [MISSING] {ulog.name}")

# ── write CSV ─────────────────────────────────────────────────────────────
with open(CSV_OUT, 'w') as f:
    f.write("config,memory,test,parallel,run,ops_per_sec\n")
    for row in all_rows:
        f.write(",".join(str(x) for x in row) + "\n")

print(f"\nWrote {len(all_rows)} rows to {CSV_OUT}")
print("Tests found:", sorted(set(r[2] for r in all_rows)))
