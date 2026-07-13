#!/usr/bin/env python3
"""Parse dbx1000 cross-warehouse sweep logs; produce CSV + figures.

Inputs (in --log-dir, produced by run.sh):
  machine<i>_r<ratio>.log     per-machine QEMU log per ratio

Extracted per ratio:
  thp        "thp=<v>" from machine 0 (Mops/s as printed by dbx1000)
  CXL MB     last [VMSPACE MEMORY] "CXL (shared)" page count; the CXL pool is
             shared, so we take the max across machines' views
  DRAM MB    sum over machines i of machine i's own "Machine i" local pages
             (each machine's last [VMSPACE MEMORY] block)

Outputs (under --out-dir):
  results/cross_warehouse.csv
  results/footprint_per_machine.csv
  figures/dbx1000-cross-warehouse.pdf/.eps  (thp + footprint vs ratio)
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

PAGE_KB = 4

PAT_THP = re.compile(r"thp=([\d.eE+-]+)")
PAT_CXL = re.compile(r"\[VMSPACE MEMORY\] CXL \(shared\): (\d+) pages")
PAT_MACHINE = re.compile(r"\[VMSPACE MEMORY\] Machine (\d+): (\d+) pages")
PAT_PROC = re.compile(r"\[VMSPACE MEMORY\] Process: (\S+)")


def parse_machine_log(path: Path):
    """Return (thp, last_block) where last_block = (cxl_pages, {mid: pages}).

    Only [VMSPACE MEMORY] blocks of the rundb process are considered; the
    last block in the log corresponds to the post-execution stats.
    """
    thp = None
    blocks = []
    cur = None
    cur_proc = None
    for line in path.read_text(errors="replace").splitlines():
        m = PAT_THP.search(line)
        if m:
            thp = float(m.group(1))
            continue
        m = PAT_PROC.search(line)
        if m:
            cur_proc = m.group(1)
            continue
        m = PAT_CXL.search(line)
        if m:
            cur = {"cxl": int(m.group(1)), "machines": {}, "proc": cur_proc}
            blocks.append(cur)
            continue
        m = PAT_MACHINE.search(line)
        if m and cur is not None:
            cur["machines"][int(m.group(1))] = int(m.group(2))

    rundb_blocks = [b for b in blocks if b["proc"] and "rundb" in b["proc"]]
    if not rundb_blocks:
        rundb_blocks = blocks
    last = rundb_blocks[-1] if rundb_blocks else None
    return thp, last


def pages_to_mb(pages: int) -> float:
    return pages * PAGE_KB / 1024.0


def collect(log_dir: Path, ratios, num_machines):
    rows = []       # per ratio aggregate
    per_machine = []  # per (ratio, machine) detail
    for ratio in ratios:
        thp = None
        cxl_views = []
        dram_own = {}
        for i in range(num_machines):
            f = log_dir / f"machine{i}_r{ratio}.log"
            if not f.exists():
                print(f"[WARN] missing log: {f}")
                continue
            t, last = parse_machine_log(f)
            if i == 0 and t is not None:
                thp = t
            if last is None:
                print(f"[WARN] no [VMSPACE MEMORY] block in {f}")
                continue
            cxl_views.append((i, last["cxl"]))
            dram_own[i] = last["machines"].get(i, 0)
            per_machine.append({
                "ratio": ratio,
                "machine": i,
                "cxl_pages": last["cxl"],
                "own_dram_pages": last["machines"].get(i, 0),
            })
        cxl_pages = max((v for _, v in cxl_views), default=0)
        dram_pages = sum(dram_own.values())
        rows.append({
            "ratio": ratio,
            "thp": thp,
            "cxl_mb": pages_to_mb(cxl_pages),
            "dram_mb": pages_to_mb(dram_pages),
        })
    return rows, per_machine


def write_csvs(rows, per_machine, results_dir: Path):
    results_dir.mkdir(parents=True, exist_ok=True)
    with (results_dir / "cross_warehouse.csv").open("w") as f:
        f.write("cross_warehouse_pct,thp,cxl_mb,dram_mb,cxl_over_all_pct\n")
        for r in rows:
            total = r["cxl_mb"] + r["dram_mb"]
            pct = 100.0 * r["cxl_mb"] / total if total else float("nan")
            thp = r["thp"] if r["thp"] is not None else ""
            f.write(f"{r['ratio']},{thp},{r['cxl_mb']:.1f},{r['dram_mb']:.1f},{pct:.1f}\n")
    with (results_dir / "footprint_per_machine.csv").open("w") as f:
        f.write("cross_warehouse_pct,machine,cxl_pages,own_dram_pages\n")
        for row in per_machine:
            f.write(f"{row['ratio']},{row['machine']},{row['cxl_pages']},{row['own_dram_pages']}\n")


def plot(rows, fig_dir: Path):
    fig_dir.mkdir(parents=True, exist_ok=True)
    plt.rcdefaults()
    plt.rcParams.update({"font.size": 14, "figure.figsize": (9, 3.4)})

    ratios = [r["ratio"] for r in rows]
    x = np.arange(len(ratios))
    fig, (ax1, ax2) = plt.subplots(1, 2, constrained_layout=True)

    # (a) throughput
    thps = [r["thp"] if r["thp"] is not None else float("nan") for r in rows]
    ax1.plot(x, thps, marker="o", color="#d62728", linewidth=2)
    ax1.set_xticks(x, [f"{r}%" for r in ratios])
    ax1.set_xlabel("Cross-warehouse txn ratio")
    ax1.set_ylabel("Throughput (Mtxn/s)")
    ax1.set_ylim(bottom=0)
    ax1.grid(axis="y", linestyle=":")
    ax1.set_title("(a) TPC-C throughput")

    # (b) memory footprint
    width = 0.35
    cxl = [r["cxl_mb"] for r in rows]
    dram = [r["dram_mb"] for r in rows]
    ax2.bar(x - width / 2, cxl, width, label="CXL (shared)", color="#1f77b4", edgecolor="black")
    ax2.bar(x + width / 2, dram, width, label="Local DRAM (sum)", color="#aec7e8", edgecolor="black")
    ax2.set_xticks(x, [f"{r}%" for r in ratios])
    ax2.set_xlabel("Cross-warehouse txn ratio")
    ax2.set_ylabel("Memory (MB)")
    ax2.grid(axis="y", linestyle=":")
    ax2.legend(frameon=False, fontsize=12)
    ax2.set_title("(b) Memory footprint")

    out = fig_dir / "dbx1000-cross-warehouse"
    fig.savefig(out.with_suffix(".pdf"), format="pdf", bbox_inches="tight")
    fig.savefig(out.with_suffix(".eps"), format="eps", bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {out}.pdf and .eps")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log-dir", required=True, type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--num-machines", type=int, default=8)
    ap.add_argument("--ratios", nargs="+", type=int, default=[15, 50, 80])
    args = ap.parse_args()

    rows, per_machine = collect(args.log_dir, args.ratios, args.num_machines)
    for r in rows:
        print(f"ratio={r['ratio']}%: thp={r['thp']} cxl={r['cxl_mb']:.0f}MB dram={r['dram_mb']:.0f}MB")
    write_csvs(rows, per_machine, args.out_dir / "results")
    plot(rows, args.out_dir / "figures")


if __name__ == "__main__":
    main()
