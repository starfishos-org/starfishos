#!/usr/bin/env python3
"""Parse state-partition AE logs and plot paper Figure 13.

Inputs (in --log-dir, produced by run.sh):
  <bench>_<config>.log     one QEMU machine-0 log per (bench, config)

Outputs (under --out-dir):
  results/state_partition.csv    raw metric per config row / bench column
                                 (same layout as p3os-paper/eval/state_partition.csv)
  results/normalized.csv         values normalized to All_DRAM (Private)
  figures/fig13-state-partition.pdf/.eps
"""
from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

BENCHS = [
    "leveldb",
    "dbx1000",
    "pca",
    "matrix_multiply",
    "linear_regression",
    "word_count",
]

CONFIGS = [
    "All_CXL",
    "Kernel_DRAM_User_CXL",
    "Kernel_Page_CXL_Other_DRAM",
    "All_DRAM",
]

BENCH_LABEL = {
    "leveldb": "LevelDB",
    "dbx1000": "DBx1000",
    "pca": "PCA",
    "matrix_multiply": "Matrix Mult.",
    "linear_regression": "Linear Reg.",
    "word_count": "Word Count",
}

CONFIG_LABEL = {
    "All_CXL": "Share",
    "Kernel_DRAM_User_CXL": "K-mix/U-share",
    "Kernel_Page_CXL_Other_DRAM": "K-mix/U-mix",
    "All_DRAM": "Private",
}

# Throughput benches: higher is better. Others report execution time (lower
# is better) and are normalized as baseline_time / time.
THROUGHPUT_BENCHS = {"leveldb", "dbx1000"}

FATAL_LOG_PATTERNS = (
    "General Protection Fault",
    "Kernel panic",
    "do_page_fault: invalid user access",
    "do_page_fault: user NULL dereference",
    "KERNEL FAULT",
)


def log_is_valid(text: str, bench: str) -> bool:
    fatal = next((pattern for pattern in FATAL_LOG_PATTERNS if pattern in text), None)
    if fatal:
        print(f"[WARN] rejecting {bench} log with fatal marker: {fatal}")
        return False
    if bench in {"pca", "matrix_multiply", "linear_regression", "word_count"} \
            and "finalize:" not in text:
        print(f"[WARN] rejecting incomplete {bench} log without finalize marker")
        return False
    return True


def extract_leveldb(text: str):
    # e.g. "fillbatch    :   5.234 micros/op;   21.1 MB/s"
    val = None
    for line in text.splitlines():
        if "MB/s" in line:
            m = re.search(r"([\d.]+)\s*MB/s", line)
            if m:
                val = float(m.group(1))
    return val


def extract_dbx1000(text: str):
    val = None
    for line in text.splitlines():
        if "thp=" in line:
            m = re.search(r"thp=([\d.eE+-]+)", line)
            if m:
                val = float(m.group(1))
    return val


def extract_phoenix(text: str):
    # phoenix apps print "library: <usecs>" (take the plain "library:" line,
    # not "inter library:")
    val = None
    for line in text.splitlines():
        m = re.search(r"(?<!inter )library:\s*([\d.]+)", line)
        if m:
            val = float(m.group(1))
    return val


EXTRACTORS = {
    "leveldb": extract_leveldb,
    "dbx1000": extract_dbx1000,
    "pca": extract_phoenix,
    "matrix_multiply": extract_phoenix,
    "linear_regression": extract_phoenix,
    "word_count": extract_phoenix,
}


def collect(log_dir: Path):
    data = {b: {c: None for c in CONFIGS} for b in BENCHS}
    for bench in BENCHS:
        for cfg in CONFIGS:
            f = log_dir / f"{bench}_{cfg}.log"
            if not f.exists():
                print(f"[WARN] missing log: {f}")
                continue
            text = f.read_text(errors="replace")
            if not log_is_valid(text, bench):
                continue
            val = EXTRACTORS[bench](text)
            if val is None:
                print(f"[WARN] no metric found in {f}")
            data[bench][cfg] = val
    return data


def normalize(data):
    norm = {b: {c: float("nan") for c in CONFIGS} for b in BENCHS}
    for bench in BENCHS:
        base = data[bench]["All_DRAM"]
        if not base:
            print(f"[WARN] cannot normalize {bench}: All_DRAM value missing/zero")
            continue
        for cfg in CONFIGS:
            v = data[bench][cfg]
            if not v:
                continue
            if bench in THROUGHPUT_BENCHS:
                norm[bench][cfg] = v / base
            else:
                norm[bench][cfg] = base / v
    return norm


def write_csvs(data, norm, results_dir: Path):
    results_dir.mkdir(parents=True, exist_ok=True)
    # paper layout: header = bench names, one row per config (CONFIGS order)
    with (results_dir / "state_partition.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(BENCHS)
        for cfg in CONFIGS:
            w.writerow([data[b][cfg] if data[b][cfg] is not None else "" for b in BENCHS])
    with (results_dir / "normalized.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["config"] + BENCHS)
        for cfg in CONFIGS:
            w.writerow([CONFIG_LABEL[cfg]] + [f"{norm[b][cfg]:.4f}" for b in BENCHS])


def plot(norm, fig_dir: Path):
    fig_dir.mkdir(parents=True, exist_ok=True)
    colormap = plt.colormaps["tab20c"]
    colors = [colormap(i) for i in range(4)]

    # Only plot benches that produced data (e.g. leveldb is skipped when it
    # hangs / is excluded from BENCHS).
    benchs = [b for b in BENCHS
              if any(not np.isnan(norm[b][c]) for c in CONFIGS)]
    if not benchs:
        raise SystemExit("No bench produced data; nothing to plot")

    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({"font.size": 26, "figure.figsize": (13, 4.8)})
    plt.figure()

    x = np.arange(len(benchs))
    n_cfg = len(CONFIGS)
    width = 0.8 / n_cfg

    for i, cfg in enumerate(CONFIGS):
        vals = [norm[b][cfg] for b in benchs]
        plt.bar(
            x + (i - n_cfg / 2 + 0.5) * width,
            vals,
            width,
            label=CONFIG_LABEL[cfg],
            color=colors[i],
            edgecolor="black",
        )

    plt.ylabel("Norm. Perf.")
    ymax = max(
        (norm[b][c] for b in benchs for c in CONFIGS if not np.isnan(norm[b][c])),
        default=1.0,
    )
    plt.yticks([0, 0.25, 0.5, 0.75, 1.0])
    plt.ylim(0, max(1.05, ymax * 1.05))
    plt.grid(axis="y", linestyle="--", linewidth=0.8, alpha=0.5)
    plt.gca().set_axisbelow(True)
    plt.xticks(x, [BENCH_LABEL[b] for b in benchs], rotation=15, ha="center")
    plt.legend(
        frameon=False, fontsize=26, loc="upper center", ncol=4,
        columnspacing=0.6, handletextpad=0.3, labelspacing=1.5,
        bbox_to_anchor=(0.49, 1.3),
    )
    plt.tight_layout()

    out = fig_dir / "fig13-state-partition"
    plt.savefig(out.with_suffix(".pdf"), format="pdf", bbox_inches="tight")
    plt.savefig(out.with_suffix(".eps"), format="eps", bbox_inches="tight")
    plt.close()
    print(f"Wrote {out}.pdf and .eps")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log-dir", required=True, type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    args = ap.parse_args()

    data = collect(args.log_dir)
    for bench in BENCHS:
        print(f"{bench}: " + ", ".join(f"{c}={data[bench][c]}" for c in CONFIGS))
    norm = normalize(data)
    write_csvs(data, norm, args.out_dir / "results")
    plot(norm, args.out_dir / "figures")


if __name__ == "__main__":
    main()
