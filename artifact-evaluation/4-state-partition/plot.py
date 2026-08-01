#!/usr/bin/env python3
"""Parse state-partition AE logs and plot paper Figure 13 (camera-ready).

The camera-ready figure is the 8-machine panel (the default), but the plotter
still takes any list of cluster sizes and draws one panel per size.  Private
(All_DRAM) is the single-machine ideal and has its own baseline per cluster
size, run at that size's total worker count; every point is normalized to the
Private baseline of its own panel.

Inputs (in --log-dir, produced by run.sh):
  <bench>_<config>_m<machines>.log   one machine-0 log per point, keyed by the
                                     panel (Private's m8 log is its
                                     8-machine-equivalent baseline, measured on
                                     one wider guest)
  <bench>_<config>.log               legacy single-size layout (fallback, only
                                     with an explicit single --machine-counts)
  <bench>_All_DRAM_m1.log            legacy shared baseline (fallback for runs
                                     predating the per-panel Private point;
                                     warns, because it normalizes N x 8 worker
                                     cluster points against 8 workers)

Outputs:
  csv/state_partition.csv    raw metric per (config, machines) row
                             (LevelDB in ops/s, DBx1000 in Mtxn/s, the four
                             Phoenix apps in microseconds)
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
BASELINE_CONFIG = "All_DRAM"


def baseline_point(count):
    """Private baseline of a panel: same total workers, one machine."""
    return (BASELINE_CONFIG, count)

# The single db_bench benchmark run by user/script/run_leveldb.sh.  Pinning the
# name keeps the metric tied to one phase: db_bench prints a "micros/op" line
# per benchmark, and several of them print no "MB/s" at all, so "the last
# micros/op line" would silently start reporting a different benchmark the
# moment the workload grows a second one.
LEVELDB_BENCH = "fillbatch"
PAT_LEVELDB = re.compile(
    r"^\s*" + LEVELDB_BENCH + r"\s*:\s*([\d.]+)\s*micros/op", re.MULTILINE
)

DEFAULT_MACHINE_COUNTS = [8]

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
    """All (config, machines) points: every config at every cluster size."""
    points = []
    for cfg in SHARED_CONFIGS:
        for count in machine_counts:
            points.append((cfg, count))
    for count in machine_counts:
        points.append(baseline_point(count))
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
    if bench == "leveldb" and not PAT_LEVELDB.search(text):
        # Reject rather than fall back to another benchmark's micros/op line:
        # a silently substituted metric is worse than a missing point.
        print(f"[WARN] rejecting leveldb log without a '{LEVELDB_BENCH}' result "
              f"line (run.sh's bench_command must run --benchmarks={LEVELDB_BENCH})")
        return False
    return True


def extract_leveldb(text: str):
    # e.g. "fillbatch    :   5.234 micros/op;   21.1 MB/s"
    #
    # Read micros/op, not MB/s: LevelDB prints MB/s with a single decimal, so
    # once the cluster slows below ~1 MB/s every placement collapses onto the
    # same quantized value (all three shared configs read 0.4 at 4 machines and
    # 0.2 at 8) and the comparison the figure exists for is lost.  micros/op
    # keeps three decimals over the whole range.  Report it as ops/s so LevelDB
    # stays a higher-is-better throughput metric: the entry size is fixed, so
    # ops/s is proportional to MB/s and every normalized ratio is unchanged.
    #
    # Anchored to LEVELDB_BENCH rather than "the last micros/op line" so the
    # figure cannot silently switch to another benchmark's number.
    val = None
    for m in PAT_LEVELDB.finditer(text):
        micros = float(m.group(1))
        if micros > 0:
            val = 1e6 / micros
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
              machine_counts, allow_legacy_names: bool) -> Path:
    """Per-point log path; legacy names accepted for replots of older runs."""
    preferred = log_dir / f"{bench}_{cfg}_m{count}.log"
    if preferred.exists():
        return preferred
    # The unsuffixed name carries no cluster size, so it can only be read as
    # the size the caller asked for.  Requires an explicit --machine-counts:
    # the default is a single size too, and silently relabelling a legacy
    # 2-machine log as the 8-machine point is worse than reporting it missing.
    legacy = log_dir / f"{bench}_{cfg}.log"
    if allow_legacy_names and len(machine_counts) == 1 and legacy.exists():
        return legacy
    # Runs that predate the per-panel Private point have one m1 baseline that
    # every panel shared.  Accept it so those runs stay replottable, but say so
    # loudly: it normalizes 32- and 64-worker cluster points against an
    # 8-worker single-machine run, which reads scale-out speedup as placement
    # benefit (Matrix Multiply came out at ~3x that way).
    if cfg == BASELINE_CONFIG and count != 1:
        shared_baseline = log_dir / f"{bench}_{cfg}_m1.log"
        if shared_baseline.exists():
            print(f"[WARN] {bench}: no per-panel Private baseline for "
                  f"{count} machines; falling back to the legacy shared "
                  f"{shared_baseline.name}. Its worker count does not match "
                  f"the cluster points it normalizes.")
            return shared_baseline
    return preferred


def collect(log_dir: Path, machine_counts, allow_legacy_names: bool = False):
    points = experiment_points(machine_counts)
    data = {b: {p: None for p in points} for b in BENCHS}
    for bench in BENCHS:
        for cfg, count in points:
            f = point_log(log_dir, bench, cfg, count, machine_counts,
                          allow_legacy_names)
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
                f"{machine_counts}"
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
            point = (cfg, count)
            for bench, value in zip(header, row):
                if bench in data and value.strip():
                    data[bench][point] = float(value)
    return data


def normalize(data, machine_counts):
    """Normalize each point to the Private baseline of its own panel."""
    points = experiment_points(machine_counts)
    norm = {b: {p: float("nan") for p in points} for b in BENCHS}
    for bench in BENCHS:
        for count in machine_counts:
            panel = [p for p in points if p[1] == count]
            base = data[bench][baseline_point(count)]
            if not base:
                if any(data[bench][p] is not None for p in panel):
                    print(f"[WARN] cannot normalize {bench} at {count} "
                          "machines: All_DRAM value missing/zero")
                continue
            for point in panel:
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
    # at that size plus that size's own Private baseline (== 1.0).
    def panel_points(count):
        return [(cfg, count) for cfg in SHARED_CONFIGS] + [baseline_point(count)]

    # Only plot benches that produced data (e.g. leveldb is skipped when it
    # hangs / is excluded from BENCHS).  The list is shared by every panel so
    # multi-size runs keep the same x categories in the same order: a bench
    # measurable at one size but not another leaves a gap rather than shifting
    # every other group sideways in one panel only.
    drawable = [b for b in BENCHS
                if any(not np.isnan(norm[b][p])
                       for count in machine_counts
                       for p in panel_points(count))]
    if not drawable:
        stale = out.with_suffix(".png")
        if stale.exists():
            stale.unlink()
        print("[WARN] no normalized data are drawable (an All_DRAM baseline "
              "is required); wrote CSV results and skipped the figure")
        return False

    # The submitted 2-machine figure never exceeded the Private baseline, so
    # the axis was fixed at 1.05.  The 8-machine panel does exceed it
    # (Matrix Multiply reaches ~3x), and a fixed limit silently clips those
    # bars flat at 1.0 — exactly the growth the figure exists to show.  Size
    # the axis from the data instead, shared by every panel so the panels stay
    # comparable, and pick a tick step that always lands on 1.0 so the Private
    # baseline is readable as a gridline.
    top = max(
        [v for count in machine_counts for b in drawable
         for v in (norm[b][p] for p in panel_points(count)) if not np.isnan(v)]
        + [1.0]
    )
    ymax = top * 1.05
    step = next((s for s in (0.25, 0.5, 1.0, 2.0, 5.0) if top / s <= 8), 10.0)
    yticks = np.arange(0, ymax, step)

    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({"font.size": 26})

    n_panels = len(machine_counts)
    fig, axes = plt.subplots(
        1, n_panels, figsize=(10.5 * n_panels, 4.8), sharey=True, squeeze=False
    )
    axes = axes[0]

    for ax, count in zip(axes, machine_counts):
        benchs = drawable
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
        ax.set_yticks(yticks)
        ax.set_ylim(0, ymax)
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
    # Default stays None so an omitted flag is distinguishable from an
    # explicit one: the unsuffixed legacy log layout has no cluster size of its
    # own, so adopting it is only safe when the caller named the size.
    ap.add_argument("--machine-counts", type=int, nargs="+",
                    default=None, metavar="N",
                    help="cluster sizes plotted as panels; each has its own "
                         f"Private baseline (default: {DEFAULT_MACHINE_COUNTS})")
    ap.add_argument("--allow-partial", action="store_true",
                    help="allow unrelated points to be absent; requested points stay mandatory")
    ap.add_argument("--require-point", action="append", default=[],
                    metavar="BENCH/CONFIG/MACHINES",
                    help="require this requested log to contain its metric; repeatable")
    args = ap.parse_args()

    sized_explicitly = args.machine_counts is not None
    machine_counts = list(dict.fromkeys(
        args.machine_counts if sized_explicitly else DEFAULT_MACHINE_COUNTS))
    if any(count < 1 for count in machine_counts):
        ap.error("--machine-counts entries must be positive")

    if args.require_point and not args.log_dir:
        ap.error("--require-point requires --log-dir")
    required_points = parse_required_points(
        args.require_point, machine_counts, ap)
    if args.csv:
        # The paper CSV records no cluster size either, so it must not inherit
        # the default one; the size it was measured at has to be stated.
        if not sized_explicitly:
            ap.error("--csv holds one cluster size per config; pass the "
                     "--machine-counts value it was measured at")
        data = load_paper_csv(args.csv, machine_counts)
    elif args.log_dir:
        data = collect(args.log_dir, machine_counts,
                       allow_legacy_names=sized_explicitly)
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
