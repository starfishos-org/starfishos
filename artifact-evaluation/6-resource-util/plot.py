#!/usr/bin/env python3
"""Parse the co-location / resource-utilization suite logs and plot the paper
"real" figure (real.eps).

Each of 12 benchmarks is measured under three conditions:
  single  — the benchmark alone (baseline)
  stress  — co-located with a competing workload, traditional (DRAM) placement
  p3os    — co-located under StarfishOS (cross-machine offload via CXL)

Inputs (in --log-dir, produced by run.sh)
  <bench>_<cond>.log      one QEMU log per (benchmark, condition)
  where <cond> in {single, stress, p3os}

Output CSV schema (same as p3os-paper/eval/real.csv):
  <bench>-<cond>,<value>          one row per (benchmark, condition)

Outputs (under --out-dir)
  results/real.csv
  figures/real.{eps,pdf,png}

Verify the drawing offline against the paper's own data::

  python3 plot.py --csv /mnt/disk1/yjs/p3os-paper/eval/real.csv \
                            --out-dir /tmp/real-check
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.patches import Patch

BENCHES = [
    "matrix", "leveldb", "linear-regression", "dbx1000", "pca", "redis",
    "word-count", "memcached", "gemini", "string-match", "kmeans", "cnn",
]
CONDS = ["single", "stress", "p3os"]


# ── per-app metric extractors ───────────────────────────────────────────────
# Each returns the last matching numeric value in the log, or None.

def _last(pattern, text, group=1):
    val = None
    for m in re.finditer(pattern, text):
        val = float(m.group(group))
    return val


def x_leveldb(t):      # MB/s
    return _last(r"([\d.]+)\s*MB/s", t)


def x_dbx1000(t):      # thp=... (Mops or ops depending on build)
    return _last(r"thp=([\d.eE+-]+)", t)


def x_phoenix(t):      # "library: <usecs>" (not "inter library:")
    return _last(r"(?<!inter )library:\s*([\d.]+)", t)


def x_redis(t):        # redis-benchmark "requests per second"
    return _last(r"([\d.]+)\s*requests per second", t)


def x_memcached(t):    # memcachetest average get latency/throughput
    return _last(r"ops/sec:\s*([\d.]+)", t) or _last(r"Total throughput:\s*([\d.]+)", t)


def x_gemini(t):       # pagerank "<seconds>s" runtime line
    return _last(r"exec_time[:=]\s*([\d.]+)", t) or _last(r"([\d.]+)\s*s(?:ec)?\b", t)


def x_cnn(t):          # tiny-cnn inference time
    value = _last(r"Forward finished\.\s*([\d.]+)\s*ms\b", t)
    if value is not None:
        return value
    seconds = _last(r"Forward finished\.\s*([\d.]+)\s*s\b", t)
    return seconds * 1000 if seconds is not None else None


EXTRACTORS = {
    "matrix": x_phoenix,
    "leveldb": x_leveldb,
    "linear-regression": x_phoenix,
    "dbx1000": x_dbx1000,
    "pca": x_phoenix,
    "redis": x_redis,
    "word-count": x_phoenix,
    "memcached": x_memcached,
    "gemini": x_gemini,
    "string-match": x_phoenix,
    "kmeans": x_phoenix,
    "cnn": x_cnn,
}


def collect(log_dir: Path):
    """Collect metrics from <bench>_<cond>.log, with stress/p3os type-log fallback."""
    # Ensure demuxed names exist when only machine*-{stress,p3os}-typeN.log remain.
    _demux_type_logs(log_dir)

    rows = []
    for bench in BENCHES:
        for cond in CONDS:
            f = log_dir / f"{bench}_{cond}.log"
            if not f.exists():
                print(f"[WARN] missing log: {f}")
                continue
            val = EXTRACTORS[bench](f.read_text(errors="replace"))
            if val is None:
                print(f"[WARN] no metric extracted from {f}")
                continue
            rows.append((f"{bench}-{cond}", val))
    return rows


def require_complete_rows(rows):
    got = {name for name, _ in rows}
    expected = {f"{bench}-{cond}" for bench in BENCHES for cond in CONDS}
    missing = sorted(expected - got)
    if missing:
        raise SystemExit(
            f"Incomplete resource-util dataset; missing {len(missing)} of "
            f"{len(expected)} points: " + ", ".join(missing)
        )


# Stress-type -> benches (matches user/script/single_stress_typeN.sh).
STRESS_TYPE_BENCHES = {
    1: ["leveldb", "matrix"],
    2: ["dbx1000", "word-count"],
    3: ["linear-regression", "redis"],
    4: ["memcached", "pca"],
    5: ["cnn", "kmeans"],
    6: ["string-match", "gemini"],
}

# Cross-type -> (bench, machine) for p3os demux (one app per machine).
CROSS_TYPE_BENCH_MACHINES = {
    1: [("leveldb", 0), ("matrix", 1)],
    2: [("dbx1000", 0), ("word-count", 1)],
    3: [("redis", 0), ("linear-regression", 1)],
    4: [("memcached", 0), ("pca", 1)],
    5: [("kmeans", 0), ("cnn", 1)],
    6: [("string-match", 0), ("gemini", 1)],
}


def _copy_if_absent(src: Path, dst: Path):
    if dst.exists() or not src.exists():
        return
    dst.write_bytes(src.read_bytes())
    print(f"[demux] {src.name} -> {dst.name}")


def _demux_type_logs(log_dir: Path):
    """Materialize <bench>_<cond>.log from archived stress/p3os type bundles."""
    for typ, benches in STRESS_TYPE_BENCHES.items():
        src = log_dir / f"machine0-stress-type{typ}.log"
        for bench in benches:
            _copy_if_absent(src, log_dir / f"{bench}_stress.log")
    for typ, pairs in CROSS_TYPE_BENCH_MACHINES.items():
        for bench, mach in pairs:
            src = log_dir / f"machine{mach}-p3os-type{typ}.log"
            _copy_if_absent(src, log_dir / f"{bench}_p3os.log")


def write_csv(rows, csv_path: Path):
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w") as f:
        for name, val in rows:
            f.write(f"{name},{val}\n")
    print(f"Wrote {csv_path}")


# ── drawing (self-contained, mirrors p3os-paper/eval/real.py) ───────────────

def _draw_fig(fig_dir: Path, name: str):
    fig_dir.mkdir(parents=True, exist_ok=True)
    stem = fig_dir / name
    plt.savefig(stem.with_suffix(".eps"), format="eps", bbox_inches="tight")
    plt.savefig(stem.with_suffix(".pdf"), format="pdf", bbox_inches="tight")
    plt.savefig(stem.with_suffix(".png"), dpi=200, format="png", bbox_inches="tight")
    print(f"Wrote {stem}.eps/.pdf/.png")


def draw(csv_path: Path, fig_dir: Path, allow_partial: bool = False):
    tab20 = plt.colormaps["tab20"]
    hatch_patterns = ["+", "x", "o", "O", ".", "*"]
    starfish_hatch = "\\\\"

    def lighten(color, amount=0.35):
        r, g, b, *rest = color
        a = rest[0] if rest else 1.0
        return (r + (1 - r) * amount, g + (1 - g) * amount, b + (1 - b) * amount, a)

    blue_color = lighten(tab20(0))
    colors = [blue_color, blue_color, tab20(6), tab20(7)]

    reversed_items = ["matrix", "word-count", "pca", "linear-regression",
                      "gemini", "kmeans", "string-match", "cnn"]
    sorted_items = ["dbx1000", "matrix", "leveldb", "word-count",
                    "linear-regression", "redis", "string-match", "pca",
                    "memcached", "gemini", "cnn", "kmeans"]
    dram_groups = [
        ["leveldb", "matrix"], ["dbx1000", "word-count"],
        ["linear-regression", "redis"], ["memcached", "pca"],
        ["string-match", "gemini"], ["cnn", "kmeans"],
    ]
    p3os_cross_items = ["cnn", "linear-regression", "matrix", "pca"]
    label_map = {
        "leveldb": "LevelDB", "dbx1000": "DBx1000", "linear-regression": "Linear Reg.",
        "matrix": "Matrix Mult.", "redis": "Redis", "pca": "PCA",
        "word-count": "Word Count", "memcached": "Memcached", "gemini": "GeminiGraph",
        "string-match": "String Match", "kmeans": "KMeans", "cnn": "CNN",
    }

    data = pd.read_csv(csv_path, header=None, names=["benchmark", "value"])
    data[["name", "type"]] = data["benchmark"].str.rsplit("-", n=1, expand=True)
    plot_data = data.pivot(index="name", columns="type", values="value")

    # Require the full 12 x 3 matrix by default.  Partial drawing is only for
    # debugging interrupted runs and must be requested explicitly.
    for col in CONDS:
        if col not in plot_data.columns:
            plot_data[col] = float("nan")
    complete = plot_data.dropna(subset=CONDS)
    dropped = sorted(set(plot_data.index) - set(complete.index))
    if dropped:
        if not allow_partial:
            raise SystemExit(
                "Incomplete resource-util CSV; need single/stress/p3os for: "
                + ", ".join(dropped)
            )
        print(f"[WARN] skipping incomplete benches (need single/stress/p3os): {dropped}")
    absent = sorted(set(BENCHES) - set(complete.index))
    if absent and not allow_partial:
        raise SystemExit("Resource-util CSV is missing benchmarks: " + ", ".join(absent))
    if complete.empty:
        raise SystemExit("No benchmark has all three conditions; nothing to plot")
    plot_data = complete

    nd = plot_data.copy()
    nd["p3os"] = nd["p3os"] / nd["single"]
    nd["stress"] = nd["stress"] / nd["single"]
    nd["single"] = nd["single"] / nd["single"]
    for item in reversed_items:
        if item in nd.index:
            nd.loc[item, "stress"] = 1 / nd.loc[item, "stress"]
            nd.loc[item, "p3os"] = 1 / nd.loc[item, "p3os"]
    nd[nd > 1.1] = 1.01
    for item in ["dbx1000", "word-count", "string-match", "gemini"]:
        if item in nd.index:
            nd.loc[item, "stress"] = 1.0
    # Keep paper order for benches that are present.
    ordered = [b for b in sorted_items if b in nd.index]
    nd = nd.reindex(ordered)

    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({"font.size": 22, "figure.figsize": (11, 3.5)})
    fig, ax = plt.subplots()
    width = 0.38
    x = np.arange(len(nd.index))

    stress_hatches = {}
    for i, group in enumerate(dram_groups):
        for item in group:
            stress_hatches[item] = hatch_patterns[i]

    stress_bars = ax.bar(x - width / 2, nd["stress"], width, label="Stress",
                         color=colors[1], edgecolor="black")
    for i, item in enumerate(nd.index):
        if item in stress_hatches and i % 3 != 0:
            stress_bars[i].set_hatch(stress_hatches[item])
        if item in ["dbx1000", "word-count", "string-match", "gemini"]:
            stress_bars[i].set_facecolor(colors[0])

    for i, item in enumerate(nd.index):
        bar = ax.bar(x[i] + width / 2, nd["p3os"].iloc[i], width,
                     color=colors[3], edgecolor="black")
        if item in p3os_cross_items:
            bar[0].set_hatch(starfish_hatch)

    for i, (sv, pv) in enumerate(zip(nd["stress"], nd["p3os"])):
        ax.text(x[i] - width * 0.6, sv + 0.05, f"{int((sv - 1) * 100)}%",
                ha="center", va="bottom", fontsize=20, rotation=90)
        ax.text(x[i] + width * 0.6, pv + 0.05, f"{int((pv - 1) * 100)}%",
                ha="center", va="bottom", fontsize=20, rotation=90)

    ax.set_ylabel("Normalized Perf")
    ax.set_xticks(x)
    ax.set_xticklabels([label_map[i] for i in nd.index], rotation=90, ha="center", fontsize=20)
    ax.set_ylim(0, 1.3)
    legend_handles = [
        Patch(facecolor=colors[0], edgecolor="black", label="Traditional"),
        Patch(facecolor=colors[3], edgecolor="black", label="StarfishOS"),
    ]
    ax.legend(handles=legend_handles, frameon=False, fontsize=20,
              bbox_to_anchor=(0.5, 1.15), loc="center", ncol=2, columnspacing=0.5)
    ax.grid(True, which="both", axis="y", linestyle=":")
    for i in range(2, len(x), 3):
        if i < len(x) - 1:
            ax.axvline(x=(x[i] + x[i + 1]) / 2, color="black", linestyle="-", linewidth=1)

    _draw_fig(fig_dir, "real")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log-dir", type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--csv", type=Path,
                    help="Draw directly from an existing real.csv (verify vs paper)")
    ap.add_argument("--allow-partial", action="store_true",
                    help="debug only: plot an interrupted/incomplete collection")
    args = ap.parse_args()

    fig_dir = args.out_dir / "figures"
    if args.csv:
        draw(args.csv, fig_dir, args.allow_partial)
        return
    if not args.log_dir:
        ap.error("either --csv or --log-dir is required")
    rows = collect(args.log_dir)
    if not rows:
        raise SystemExit("No benchmark produced a metric; nothing to plot")
    if not args.allow_partial:
        require_complete_rows(rows)
    csv_path = args.out_dir / "results" / "real.csv"
    write_csv(rows, csv_path)
    draw(csv_path, fig_dir, args.allow_partial)


if __name__ == "__main__":
    main()
