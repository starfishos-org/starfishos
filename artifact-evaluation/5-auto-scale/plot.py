#!/usr/bin/env python3
"""Parse auto-scaling sweep logs and plot the three paper auto-scale figures:

  auto-scale-matrix.eps  — Matrix-multiply MapReduce, throughput vs #machines
  db1000.eps             — DBx1000 TPC-C throughput vs #machines
  gemini-chcore.eps      — GeminiGraph PageRank runtime->throughput vs #machines

Each figure compares StarfishOS (Mixed / CXL) against external baselines
(Ideal = Linux DRAM, Distributed = Linux-MPI, Tigon).  The StarfishOS curves
come from ChCore sweep logs; the baselines come from `test-on-linux/` ports.

Data-file formats (identical to p3os-paper/eval/):
  matrix : lines `RESULT: N=<m> CONFIG=<MIXED|CXL|TCP|IDEAL> TIME=<us>`
  db1000 : CSV `module,machines,performance_mops`
           modules P3OS-mixed, P3OS-all_cxl, tigon, linux
  gemini : CSV `machines,MIXED_DEFAULT_CXL,CXL,DRAM,LINUX-DRAM,DISTRIBUTED`
           (values are `<seconds>s`)

Draw from data files (verify against paper)::

  python3 plot.py --out-dir /tmp/as-check \
    --matrix-data /mnt/disk1/yjs/p3os-paper/eval/mapreduce/4000size.txt \
    --db1000-data /mnt/disk1/yjs/p3os-paper/eval/db1000/db1000-p3os-tigon.csv \
    --gemini-data /mnt/disk1/yjs/p3os-paper/eval/gemini_graph/data.log
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
from matplotlib.ticker import MultipleLocator

SYSTEM_NAME = "Starfish"
X_TICKS = [1, 2, 4, 6, 8]


def _draw_fig(fig_dir: Path, name: str):
    fig_dir.mkdir(parents=True, exist_ok=True)
    stem = fig_dir / name
    plt.savefig(stem.with_suffix(".eps"), format="eps", bbox_inches="tight")
    plt.savefig(stem.with_suffix(".pdf"), format="pdf", bbox_inches="tight")
    plt.savefig(stem.with_suffix(".png"), dpi=200, format="png", bbox_inches="tight")
    print(f"Wrote {stem}.eps/.pdf/.png")


# ── auto-scale-matrix ───────────────────────────────────────────────────────

def draw_matrix(data_path: Path, fig_dir: Path):
    pattern = re.compile(r"RESULT:\s+N=(\d+)\s+CONFIG=(\w+)\s+TIME=(\d+)")
    data = {"MIXED": [], "CXL": [], "TCP": [], "IDEAL": []}
    for line in Path(data_path).read_text().splitlines():
        m = pattern.search(line)
        if not m:
            continue
        n, cfg, rt = int(m.group(1)), m.group(2), int(m.group(3))
        if cfg in data:
            data[cfg].append((n, 100 * (1 / (rt / 1000 / 1000))))
    for cfg in data:
        data[cfg].sort(key=lambda it: it[0])

    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({"font.size": 22, "figure.figsize": (3.8, 4.16)})
    fig, ax1 = plt.subplots()
    styles = {
        "MIXED": {"color": "#d62728", "marker": "x", "linestyle": "-", "label": f"{SYSTEM_NAME} Mixed"},
        "CXL": {"color": "#1f77b4", "marker": "s", "linestyle": "-.", "label": f"{SYSTEM_NAME} CXL"},
        "TCP": {"color": "black", "marker": "o", "linestyle": "--", "label": "Distributed"},
        "IDEAL": {"color": "silver", "marker": "d", "linestyle": "--", "label": "Ideal"},
    }
    for cfg in ["MIXED", "CXL", "TCP", "IDEAL"]:
        pts = [it for it in data[cfg] if it[0] in X_TICKS]
        if not pts:
            continue
        s = styles[cfg]
        ax1.plot([p[0] for p in pts], [p[1] for p in pts], label=s["label"],
                 color=s["color"], linestyle=s["linestyle"], marker=s["marker"],
                 markersize=8, markeredgewidth=2, linewidth=2.2)
    ax1.set_ylabel("1/Run Time (1/s * 100)")
    ax1.set_xticks(X_TICKS)
    ax1.set_xlabel("#Machines")
    ax1.set_ylim(0, 9)
    ax1.yaxis.set_major_locator(MultipleLocator(2))
    ax1.grid(True, which="both", axis="both", linestyle=":")
    fig.tight_layout()
    _draw_fig(fig_dir, "auto-scale-matrix")
    plt.close(fig)


# ── db1000 ──────────────────────────────────────────────────────────────────

def draw_db1000(data_path: Path, fig_dir: Path):
    data = pd.read_csv(data_path).sort_values(["module", "machines"])
    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({"font.size": 22, "figure.figsize": (3.8, 4.16)})
    fig, ax1 = plt.subplots()
    styles = {
        "P3OS-mixed": {"color": "#d62728", "marker": "x", "linestyle": "-", "label": SYSTEM_NAME + " Mixed"},
        "P3OS-all_cxl": {"color": "#1f77b4", "marker": "s", "linestyle": "-.", "label": SYSTEM_NAME + " CXL"},
        "tigon": {"color": "#ff7f0e", "marker": "^", "linestyle": ":", "label": "Tigon"},
        "linux": {"color": "silver", "marker": "d", "linestyle": "--", "label": "Ideal"},
    }
    for module, group in data.groupby("module"):
        group = group.sort_values("machines")
        s = styles.get(module, {"color": "#1f77b4", "marker": "s", "linestyle": "-", "label": module})
        ax1.plot(group["machines"].values, group["performance_mops"].values,
                 label=s["label"], color=s["color"], linestyle=s["linestyle"],
                 marker=s["marker"], markersize=8, markeredgewidth=2, linewidth=2.2)
    ax1.set_ylabel("Thp (Mops/s)")
    ax1.set_xlabel("#Machines")
    ax1.set_xticks(sorted(data["machines"].unique()))
    ax1.set_ylim(0)
    ax1.grid(True, which="both", axis="both", linestyle=":")
    fig.tight_layout()
    _draw_fig(fig_dir, "db1000")
    plt.close(fig)


# ── gemini-chcore ───────────────────────────────────────────────────────────

def _parse_secs(s):
    return float(s.strip().replace("s", ""))


def draw_gemini(data_path: Path, fig_dir: Path):
    rows = []
    for line in Path(data_path).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith(("PageRank", "Graph:", "Date:", "machines")):
            continue
        parts = line.split(",")
        if len(parts) >= 6:
            rows.append({
                "machine": int(parts[0]),
                "mixed_cxl": _parse_secs(parts[1]),
                "cxl": _parse_secs(parts[2]),
                "linux_dram": _parse_secs(parts[4]),
                "distributed": _parse_secs(parts[5]),
            })
    df = pd.DataFrame(rows)
    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({"font.size": 22, "figure.figsize": (3.8, 4.16)})
    fig, ax1 = plt.subplots()
    x = df["machine"].values
    ax1.plot(x, 100 * (1 / df["mixed_cxl"].values), label=f"{SYSTEM_NAME} Mixed",
             color="#d62728", linestyle="-", marker="x", markersize=8, markeredgewidth=2, linewidth=2.2)
    ax1.plot(x, 100 * (1 / df["cxl"].values), label=f"{SYSTEM_NAME} CXL",
             color="#1f77b4", linestyle="-.", marker="s", markersize=7, markeredgewidth=1.8, linewidth=2.2)
    ax1.plot(x, 100 * (1 / df["linux_dram"].values), label="Ideal",
             color="silver", linestyle="--", marker="d", markersize=6, markeredgewidth=1.5, linewidth=2.2)
    ax1.plot(x, 100 * (1 / df["distributed"].values), label="Distributed",
             color="black", linestyle="--", marker="o", markersize=6, markeredgewidth=1.5, linewidth=2.2)
    ax1.set_ylabel("1/Run Time (1/s * 100)")
    ax1.set_xticks(x)
    ax1.set_xlabel("#Machines")
    ax1.grid(True, which="both", axis="both", linestyle=":")
    fig.tight_layout()
    _draw_fig(fig_dir, "gemini-chcore")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--matrix-data", type=Path, help="mapreduce RESULT: text")
    ap.add_argument("--db1000-data", type=Path, help="db1000 module,machines,perf CSV")
    ap.add_argument("--gemini-data", type=Path, help="gemini machines,...,DISTRIBUTED CSV/log")
    args = ap.parse_args()

    fig_dir = args.out_dir / "figures"
    drew = False
    if args.matrix_data:
        draw_matrix(args.matrix_data, fig_dir); drew = True
    if args.db1000_data:
        draw_db1000(args.db1000_data, fig_dir); drew = True
    if args.gemini_data:
        draw_gemini(args.gemini_data, fig_dir); drew = True
    if not drew:
        ap.error("provide at least one of --matrix-data / --db1000-data / --gemini-data")


if __name__ == "__main__":
    main()
