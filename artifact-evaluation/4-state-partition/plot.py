#!/usr/bin/env python3
"""Parse state-partition AE logs and plot paper Figure 13 (camera-ready).

The camera-ready ablation runs the three shared placements at several
cluster sizes (default 4 and 8 machines) and normalizes every point to the
single-machine Private (All_DRAM) baseline, so the growing benefit of
partitioned placements can be seen across cluster sizes.

Inputs (in --log-dir, produced by run.sh):
  <bench>_<config>_m<machines>.log   one machine-0 log per point
  <bench>_<config>.log               legacy single-size layout (fallback,
                                     only with a single --machine-counts)

Outputs:
  csv/state_partition.csv    raw metric per (config, machines) row
  csv/normalized.csv         values normalized to All_DRAM (Private)
  figures/state_partition.png  one panel per cluster size
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
from matplotlib.patches import Patch

SCRIPT_DIR = Path(__file__).resolve().parent

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

SHARED_CONFIGS = [cfg for cfg in CONFIGS if cfg != "All_DRAM"]
BASELINE_POINT = ("All_DRAM", 1)

DEFAULT_MACHINE_COUNTS = [4, 8]

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


def experiment_points(machine_counts):
    """All (config, machines) points: shared configs per size + baseline."""
    points = []
    for cfg in SHARED_CONFIGS:
        for count in machine_counts:
            points.append((cfg, count))
    points.append(BASELINE_POINT)
    return points


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


def point_log(log_dir: Path, bench: str, cfg: str, count: int,
              machine_counts) -> Path:
    """Per-point log path; legacy name accepted for single-size replots."""
    preferred = log_dir / f"{bench}_{cfg}_m{count}.log"
    if preferred.exists():
        return preferred
    legacy = log_dir / f"{bench}_{cfg}.log"
    if len(machine_counts) == 1 and legacy.exists():
        return legacy
    return preferred


def collect(log_dir: Path, machine_counts):
    points = experiment_points(machine_counts)
    data = {b: {p: None for p in points} for b in BENCHS}
    for bench in BENCHS:
        for cfg, count in points:
            f = point_log(log_dir, bench, cfg, count, machine_counts)
            if not f.exists():
                print(f"[WARN] missing log: {f}")
                continue
            text = f.read_text(errors="replace")
            if not log_is_valid(text, bench):
                continue
            val = EXTRACTORS[bench](text)
            if val is None:
                print(f"[WARN] no metric found in {f}")
            data[bench][cfg, count] = val
    return data


def point_name(point) -> str:
    cfg, count = point
    return f"{cfg}/m{count}"


def require_complete(data, machine_counts):
    points = experiment_points(machine_counts)
    missing = [f"{bench}/{point_name(p)}" for bench in BENCHS for p in points
               if data[bench][p] is None]
    if missing:
        raise SystemExit(
            "Incomplete state-partition dataset; missing "
            f"{len(missing)} of {len(BENCHS) * len(points)} points: "
            + ", ".join(missing)
        )


def parse_required_points(raw_points, machine_counts, parser):
    """Validate repeatable BENCH/CONFIG/MACHINES selectors from the runner."""
    valid_points = set(experiment_points(machine_counts))
    points = []
    seen = set()
    for raw in raw_points:
        parts = raw.split("/")
        if len(parts) != 3 or parts[0] not in BENCHS or parts[1] not in CONFIGS \
                or not parts[2].isdigit():
            parser.error(
                f"invalid --require-point {raw!r}; expected BENCH/CONFIG/MACHINES "
                "using a known state-partition benchmark and configuration"
            )
        point = (parts[0], (parts[1], int(parts[2])))
        if point[1] not in valid_points:
            parser.error(
                f"--require-point {raw!r} does not match --machine-counts "
                f"{machine_counts} (All_DRAM always uses 1 machine)"
            )
        if point in seen:
            parser.error(f"duplicate --require-point: {raw}")
        seen.add(point)
        points.append(point)
    return points


def require_requested(data, points):
    missing = [f"{bench}/{point_name(p)}" for bench, p in points
               if data[bench][p] is None]
    if missing:
        raise SystemExit(
            "Requested state-partition points are missing or unparseable: "
            + ", ".join(missing)
        )


def load_paper_csv(path: Path, machine_counts):
    """Load the row-per-configuration CSV format used by the paper.

    The legacy paper CSV has one row per configuration at a single cluster
    size, so validation mode requires exactly one --machine-counts entry.
    """
    if len(machine_counts) != 1:
        raise SystemExit(
            "--csv holds one cluster size per config; pass a single "
            "--machine-counts value to validate against it"
        )
    count = machine_counts[0]
    points = experiment_points(machine_counts)
    data = {b: {p: None for p in points} for b in BENCHS}
    with path.open(newline="") as source:
        reader = csv.reader(source)
        header = next(reader)
        for cfg, row in zip(CONFIGS, reader):
            point = BASELINE_POINT if cfg == "All_DRAM" else (cfg, count)
            for bench, value in zip(header, row):
                if bench in data and value.strip():
                    data[bench][point] = float(value)
    return data


def normalize(data, machine_counts):
    points = experiment_points(machine_counts)
    norm = {b: {p: float("nan") for p in points} for b in BENCHS}
    for bench in BENCHS:
        base = data[bench][BASELINE_POINT]
        if not base:
            if any(data[bench][p] is not None for p in points):
                print(f"[WARN] cannot normalize {bench}: All_DRAM value missing/zero")
            continue
        for point in points:
            v = data[bench][point]
            if not v:
                continue
            if bench in THROUGHPUT_BENCHS:
                norm[bench][point] = v / base
            else:
                norm[bench][point] = base / v
    return norm


def write_csvs(data, norm, results_dir: Path, machine_counts):
    results_dir.mkdir(parents=True, exist_ok=True)
    points = experiment_points(machine_counts)
    with (results_dir / "state_partition.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["config", "machines"] + BENCHS)
        for cfg, count in points:
            w.writerow([cfg, count] + [
                data[b][cfg, count] if data[b][cfg, count] is not None else ""
                for b in BENCHS
            ])
    with (results_dir / "normalized.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["config", "machines"] + BENCHS)
        for cfg, count in points:
            w.writerow([CONFIG_LABEL[cfg], count] + [
                "" if np.isnan(norm[b][cfg, count]) else f"{norm[b][cfg, count]:.4f}"
                for b in BENCHS
            ])


def plot(norm, fig_dir: Path, machine_counts):
    fig_dir.mkdir(parents=True, exist_ok=True)
    colormap = plt.colormaps["tab20c"]
    colors = [colormap(i) for i in range(4)]
    out = fig_dir / "state_partition"

    # A panel per cluster size; each panel shows the three shared placements
    # at that size plus the shared single-machine Private baseline (== 1.0).
    def panel_points(count):
        return [(cfg, count) for cfg in SHARED_CONFIGS] + [BASELINE_POINT]

    # Only plot benches that produced data (e.g. leveldb is skipped when it
    # hangs / is excluded from BENCHS).
    drawable = {
        count: [b for b in BENCHS
                if any(not np.isnan(norm[b][p]) for p in panel_points(count))]
        for count in machine_counts
    }
    if not any(drawable.values()):
        stale = out.with_suffix(".png")
        if stale.exists():
            stale.unlink()
        print("[WARN] no normalized data are drawable (an All_DRAM baseline "
              "is required); wrote CSV results and skipped the figure")
        return False

    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({"font.size": 26})

    n_panels = len(machine_counts)
    fig, axes = plt.subplots(
        1, n_panels, figsize=(10.5 * n_panels, 4.8), sharey=True, squeeze=False
    )
    axes = axes[0]

    for ax, count in zip(axes, machine_counts):
        benchs = drawable[count]
        if not benchs:
            ax.set_visible(False)
            continue
        x = np.arange(len(benchs))
        points = panel_points(count)
        n_bar = len(points)
        width = 0.8 / n_bar
        for i, (cfg, point_count) in enumerate(points):
            vals = [norm[b][cfg, point_count] for b in benchs]
            ax.bar(
                x + (i - n_bar / 2 + 0.5) * width,
                vals,
                width,
                color=colors[CONFIGS.index(cfg)],
                edgecolor="black",
            )
        ax.set_yticks([0, 0.25, 0.5, 0.75, 1.0])
        ax.set_ylim(0, 1.05)
        ax.grid(axis="y", linestyle="--", linewidth=0.8, alpha=0.5)
        ax.set_axisbelow(True)
        ax.set_xticks(x)
        ax.set_xticklabels([BENCH_LABEL[b] for b in benchs],
                           rotation=15, ha="center")
        ax.set_title(f"{count} machines", fontsize=26, pad=8)
    axes[0].set_ylabel("Norm. Perf.")
    legend_handles = [
        Patch(facecolor=colors[CONFIGS.index(cfg)], edgecolor="black")
        for cfg in CONFIGS
    ]
    fig.legend(
        legend_handles, [CONFIG_LABEL[cfg] for cfg in CONFIGS],
        frameon=False, fontsize=26, loc="upper center", ncol=4,
        columnspacing=0.6, handletextpad=0.3,
        bbox_to_anchor=(0.5, 1.16),
    )
    fig.tight_layout()

    fig.savefig(out.with_suffix(".png"), dpi=200, format="png",
                bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {out}.png")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log-dir", type=Path)
    ap.add_argument("--csv", type=Path,
                    help="paper-format state_partition.csv (skip log parsing)")
    ap.add_argument("--csv-dir", type=Path, default=SCRIPT_DIR / "csv")
    ap.add_argument("--fig-dir", type=Path, default=SCRIPT_DIR / "figures")
    ap.add_argument("--machine-counts", type=int, nargs="+",
                    default=DEFAULT_MACHINE_COUNTS, metavar="N",
                    help="cluster sizes of the shared placements "
                         "(default: %(default)s; Private always uses 1)")
    ap.add_argument("--allow-partial", action="store_true",
                    help="allow unrelated points to be absent; requested points stay mandatory")
    ap.add_argument("--require-point", action="append", default=[],
                    metavar="BENCH/CONFIG/MACHINES",
                    help="require this requested log to contain its metric; repeatable")
    args = ap.parse_args()

    machine_counts = list(dict.fromkeys(args.machine_counts))
    if any(count < 1 for count in machine_counts):
        ap.error("--machine-counts entries must be positive")

    if args.require_point and not args.log_dir:
        ap.error("--require-point requires --log-dir")
    required_points = parse_required_points(
        args.require_point, machine_counts, ap)
    if args.csv:
        data = load_paper_csv(args.csv, machine_counts)
    elif args.log_dir:
        data = collect(args.log_dir, machine_counts)
    else:
        ap.error("either --log-dir or --csv is required")
    for bench in BENCHS:
        print(f"{bench}: " + ", ".join(
            f"{point_name(p)}={data[bench][p]}"
            for p in experiment_points(machine_counts)))
    require_requested(data, required_points)
    if not args.allow_partial:
        require_complete(data, machine_counts)
    norm = normalize(data, machine_counts)
    write_csvs(data, norm, args.csv_dir, machine_counts)
    plot(norm, args.fig_dir, machine_counts)


if __name__ == "__main__":
    main()
