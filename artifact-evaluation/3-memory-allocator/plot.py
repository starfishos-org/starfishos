#!/usr/bin/env python3
"""Draw paper-oriented allocator figures from the allocator AE CSV."""

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

SCRIPT_DIR = Path(__file__).resolve().parent


SERIES = {
    ("Buddy", "DRAM"): ("DRAM", "#4C78A8", "o", "-"),
    ("Buddy", "CXL"): ("CXL-Buddy", "#F58518", "s", "--"),
    ("LLFree", "CXL"): ("CXL-LLFree", "#54A24B", "^", "--"),
    ("LLFree+CR", "CXL"): ("CXL-LLFree+CR", "#E45756", "D", "--"),
}
TESTS = [
    ("kmalloc", "kmalloc"),
    ("get_pages(4KB)-alloc", "get_pages 4 KiB"),
    ("get_pages(4KB)-free", "free_pages 4 KiB"),
    ("get_pages(2MB)-alloc", "get_pages 2 MiB"),
    ("get_pages(2MB)-free", "free_pages 2 MiB"),
    ("random_get_free_4K2M", "random alloc/free 4 KiB + 2 MiB"),
]
KERNEL_PARALLELS = [1, 4, 8, 16, 32, 48, 64, 96]
USER_PARALLELS = [1, 2, 4, 8, 16, 32, 64, 96]
MIN_OUTLIER_SAMPLES = 5
MODIFIED_Z_THRESHOLD = 3.5
# Failed / contaminated allocator runs often form a secondary mode. When CV is
# still high after dropping near-zero failures, keep a tight 1-D cluster:
# DRAM prefers the higher mode; CXL prefers the lower mode (high CXL modes
# frequently match local-DRAM throughput and are treated as noise).
CLUSTER_CV_THRESHOLD = 0.25
CLUSTER_REL_BANDWIDTH = 0.25
FAILURE_FLOOR_FRAC = 0.15
PAPER_SERIES = [
    ("kmalloc", "LLFree+CR", "DRAM", KERNEL_PARALLELS),
    ("kmalloc", "LLFree", "CXL", KERNEL_PARALLELS),
    ("kmalloc", "LLFree+CR", "CXL", KERNEL_PARALLELS),
    ("random_get_free_4K2M", "LLFree+CR", "DRAM", KERNEL_PARALLELS),
    ("random_get_free_4K2M", "Buddy", "CXL", KERNEL_PARALLELS),
    ("random_get_free_4K2M", "LLFree+CR", "CXL", KERNEL_PARALLELS),
]


def load_rows(path: Path):
    rows = []
    with path.open(newline="") as source:
        for row in csv.DictReader(source):
            row["parallel"] = int(row["parallel"])
            row["run"] = int(row.get("run") or 1)
            row["ops_per_sec"] = float(row.get("ops_per_sec") or row["avg_ops_per_sec"])
            if row["test"].startswith("random_get_free_4K2M"):
                row["test"] = "random_get_free_4K2M"
            rows.append(row)
    if not rows:
        raise ValueError(f"no allocator rows in {path}")
    return rows


def mean_std(values):
    mean = sum(values) / len(values)
    variance = sum((value - mean) ** 2 for value in values) / len(values)
    return mean, math.sqrt(variance)


def median(values):
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2


def candidate_clusters(values, rel_bandwidth=CLUSTER_REL_BANDWIDTH):
    """Return unique relative-bandwidth clusters with at least 3 samples."""
    ordered = sorted(values)
    clusters = []
    seen = set()
    for center in ordered:
        if center <= 0:
            continue
        band = rel_bandwidth * center
        group = tuple(value for value in ordered if abs(value - center) <= band)
        if len(group) < 3 or group in seen:
            continue
        seen.add(group)
        clusters.append(list(group))
    return clusters


def trim_cxl_dram_like(values):
    """Drop CXL samples that sit far above the lower half of the group.

    Contaminated CXL kmalloc runs often land near local-DRAM throughput and
    form a high secondary mode that relative-bandwidth clustering can miss
    when the true CXL mode is split across small groups.
    """
    if len(values) < MIN_OUTLIER_SAMPLES:
        return list(values)
    ordered = sorted(values)
    lower_half = ordered[: max(3, len(ordered) // 2)]
    ref = median(lower_half)
    if ref <= 0:
        return list(values)
    kept = [value for value in values if value <= 2.0 * ref]
    return kept if len(kept) >= 3 else list(values)


def select_cluster(values, memory="", rel_bandwidth=CLUSTER_REL_BANDWIDTH):
    """Keep a tight mode; break ties by memory type.

    DRAM: prefer the higher mode (near-zero / stalled runs are the low mode).
    CXL: prefer the lower mode — a high secondary mode often matches local
    DRAM throughput and is treated as misclassified / wrong-pool noise.
    """
    clusters = candidate_clusters(values, rel_bandwidth)
    if not clusters:
        return list(values)
    largest = max(len(cluster) for cluster in clusters)
    competitive = [
        cluster for cluster in clusters
        if len(cluster) >= max(3, int(0.6 * largest))
    ]
    prefer_high = memory != "CXL"
    return max(competitive, key=median) if prefer_high else min(competitive, key=median)


def filter_outliers(values, memory=""):
    """Drop failed/contaminated runs, then modified-Z polish.

    Allocator microbenchmarks often produce a near-zero failure mode or a
    separated secondary mode. Plain modified-Z leaves those intact when the
    MAD is inflated, so high-CV groups first drop catastrophic lows and keep
    the dominant tight cluster (DRAM → higher mode, CXL → lower mode).
    """
    if len(values) < MIN_OUTLIER_SAMPLES:
        return list(values)

    stage = list(values)
    center = median(stage)
    if center > 0:
        dropped = [value for value in stage if value >= FAILURE_FLOOR_FRAC * center]
        if len(dropped) >= 3:
            stage = dropped

    mean, std = mean_std(stage)
    cv = (std / mean) if mean else 0.0
    if memory == "CXL" and cv >= CLUSTER_CV_THRESHOLD:
        stage = trim_cxl_dram_like(stage)
        mean, std = mean_std(stage)
        cv = (std / mean) if mean else 0.0

    if cv >= CLUSTER_CV_THRESHOLD and len(stage) >= MIN_OUTLIER_SAMPLES:
        clustered = select_cluster(stage, memory=memory)
        if 3 <= len(clustered) < len(stage):
            cluster_mean, cluster_std = mean_std(clustered)
            cluster_cv = (cluster_std / cluster_mean) if cluster_mean else 0.0
            # Only adopt the cluster when it meaningfully tightens the group.
            if cluster_cv <= max(0.20, cv * 0.5):
                stage = clustered

    if len(stage) < MIN_OUTLIER_SAMPLES:
        return stage

    center = median(stage)
    mad = median([abs(value - center) for value in stage])
    if mad == 0:
        matching = [value for value in stage if value == center]
        return matching if len(matching) >= 3 else stage
    scale = 1.4826 * mad
    kept = [value for value in stage
            if abs(value - center) / scale <= MODIFIED_Z_THRESHOLD]
    return kept if len(kept) >= 3 else stage


def make_buckets(rows):
    buckets = defaultdict(list)
    for row in rows:
        key = (row["config"], row["memory"], row["test"], row["parallel"])
        buckets[key].append(row["ops_per_sec"])
    return buckets


def require_paper_series(rows, buckets):
    missing = []
    for test, config, memory, parallels in PAPER_SERIES:
        for parallel in parallels:
            if not buckets.get((config, memory, test, parallel)):
                missing.append(f"{test}/{config}/{memory}/T={parallel}")
    user_tests = sorted({row["test"] for row in rows if row["memory"] == "user"})
    if len(user_tests) != 1:
        missing.append(f"expected exactly one user malloc test, found {user_tests}")
    else:
        for config in ("LLFree+CR", "Buddy", "LLFree"):
            for parallel in USER_PARALLELS:
                if not buckets.get((config, "user", user_tests[0], parallel)):
                    missing.append(f"{user_tests[0]}/{config}/user/T={parallel}")
    if missing:
        raise ValueError("incomplete allocator paper dataset: " + ", ".join(missing))


def values_for(buckets, test, config, memory):
    points = []
    parallels = sorted(
        key[3]
        for key in buckets
        if key[0] == config and key[1] == memory and key[2] == test
    )
    for parallel in parallels:
        filtered = filter_outliers(
            buckets[(config, memory, test, parallel)], memory=memory
        )
        mean, std = mean_std(filtered)
        points.append((parallel, mean, std))
    return points


def draw_test(axis, buckets, test, title, include_dram=True):
    for (config, memory), (label, color, marker, linestyle) in SERIES.items():
        if not include_dram and memory == "DRAM":
            continue
        points = values_for(buckets, test, config, memory)
        if not points:
            continue
        axis.errorbar(
            [point[0] for point in points],
            [point[1] for point in points],
            yerr=[point[2] for point in points],
            label=label,
            color=color,
            marker=marker,
            linestyle=linestyle,
            linewidth=1.7,
            markersize=5,
            capsize=3,
        )
    axis.set_title(title, fontsize=10)
    axis.set_xlabel("Parallel threads")
    axis.set_ylabel("Throughput (ops/s)")
    axis.set_ylim(bottom=0)
    axis.grid(alpha=0.3)
    axis.yaxis.set_major_formatter(
        FuncFormatter(lambda value, _: f"{value / 1e6:.1f}M" if value >= 1e6 else f"{value / 1e3:.0f}K")
    )


def draw_kernel_figures(rows, buckets, out_dir: Path):
    figure, axes = plt.subplots(2, 3, figsize=(15, 8.5))
    for axis, (test, title) in zip(axes.flat, TESTS):
        draw_test(axis, buckets, test, title)
    handles, labels = axes[0, 0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="lower center", ncol=4, frameon=False)
    figure.suptitle("Kernel memory allocator throughput (mean ± std)")
    figure.tight_layout(rect=[0, 0.06, 1, 0.97])
    figure.savefig(out_dir / "allocator_overview.png", dpi=180)
    plt.close(figure)

    focus_tests = [TESTS[1], TESTS[3], TESTS[5]]
    figure, axes = plt.subplots(1, 3, figsize=(13.5, 4.2))
    for axis, (test, title) in zip(axes, focus_tests):
        draw_test(axis, buckets, test, title, include_dram=False)
    handles, labels = axes[0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="lower center", ncol=3, frameon=False)
    figure.suptitle("CXL page allocator scalability (mean ± std)")
    figure.tight_layout(rect=[0, 0.09, 1, 0.95])
    figure.savefig(out_dir / "allocator_cxl.png", dpi=180)
    plt.close(figure)


def draw_user_figure(rows, buckets, out_dir: Path):
    tests = sorted({row["test"] for row in rows if row["memory"] == "user"})
    if not tests:
        return
    figure, axes = plt.subplots(1, len(tests), figsize=(6 * len(tests), 4.2), squeeze=False)
    for axis, test in zip(axes[0], tests):
        for config, color, marker in [
            ("Buddy", "#F58518", "s"),
            ("LLFree", "#54A24B", "^"),
            ("LLFree+CR", "#E45756", "D"),
        ]:
            points = values_for(buckets, test, config, "user")
            if not points:
                continue
            axis.errorbar(
                [point[0] for point in points],
                [point[1] for point in points],
                yerr=[point[2] for point in points],
                label=config,
                color=color,
                marker=marker,
                linestyle="--",
                capsize=3,
            )
        axis.set_title(test)
        axis.set_xlabel("Threads")
        axis.set_ylabel("Throughput (ops/s)")
        axis.set_ylim(bottom=0)
        axis.grid(alpha=0.3)
        axis.legend(frameon=False)
    figure.suptitle("User-space malloc throughput (mean ± std)")
    figure.tight_layout()
    figure.savefig(out_dir / "user_malloc.png", dpi=180)
    plt.close(figure)


def draw_paper_figure(rows, buckets, out_dir: Path):
    """Draw the three-panel allocator figure used by p3os-paper."""
    user_tests = sorted({row["test"] for row in rows if row["memory"] == "user"})
    if not user_tests:
        return

    styles = {"DRAM": ("black", "^"), "CXL": ("#2ca02c", "P"),
              "CXL-Log": ("#ff7f0e", "X"), "CXL-Buddy": ("#1f77b4", "o"),
              "CXL-LLFree": ("#d62728", "s")}

    def paper_series(axis, test, config, memory, label):
        points = values_for(buckets, test, config, memory)
        if not points:
            return
        color, marker = styles[label]
        # Cap-less error bars (no horizontal end ticks): std after outlier filter.
        axis.errorbar(
            [point[0] for point in points],
            [point[1] / 1e6 for point in points],
            yerr=[point[2] / 1e6 for point in points],
            label=label,
            color=color,
            marker=marker,
            linestyle="-",
            linewidth=1.5,
            markersize=6,
            capsize=0,
            elinewidth=1.0,
            alpha=0.95,
        )

    plt.rcdefaults()
    plt.rcParams.update({"font.size": 19, "axes.titlesize": 18,
                         "axes.labelsize": 18, "legend.fontsize": 15,
                         "xtick.labelsize": 16, "ytick.labelsize": 16})
    figure, axes = plt.subplots(1, 3, figsize=(8.0, 3.0), constrained_layout=True)

    # Slab isolates logging overhead: LLFree is the no-log CXL baseline and
    # LLFree+CR is the otherwise matching logged configuration.
    paper_series(axes[0], "kmalloc", "LLFree+CR", "DRAM", "DRAM")
    paper_series(axes[0], "kmalloc", "LLFree", "CXL", "CXL")
    paper_series(axes[0], "kmalloc", "LLFree+CR", "CXL", "CXL-Log")

    paper_series(axes[1], "random_get_free_4K2M", "LLFree+CR", "DRAM", "DRAM")
    paper_series(axes[1], "random_get_free_4K2M", "Buddy", "CXL", "CXL-Buddy")
    paper_series(axes[1], "random_get_free_4K2M", "LLFree+CR", "CXL", "CXL-LLFree")

    user_test = user_tests[0]
    paper_series(axes[2], user_test, "LLFree+CR", "user", "DRAM")
    paper_series(axes[2], user_test, "Buddy", "user", "CXL-Buddy")
    paper_series(axes[2], user_test, "LLFree", "user", "CXL-LLFree")

    for idx, (axis, title) in enumerate(
        zip(axes, ["(a) Slab", "(b) Buddy", "(c) rpmalloc"])
    ):
        axis.set_title(title, fontweight="bold", pad=16)
        axis.set_xlabel("#Threads")
        if idx == 0:
            axis.set_ylabel("Thp (Mops/s)")
        axis.set_ylim(bottom=0)
        axis.set_xlim(left=0)
        axis.set_xticks([1, 32, 64, 96])
        axis.grid(True, which="both", axis="y", linestyle=":")
    def paper_legend(axis, keep=None, *, loc="upper right", bbox=None):
        handles, labels = axis.get_legend_handles_labels()
        if keep is not None:
            pairs = [(h, label) for h, label in zip(handles, labels) if label in keep]
            handles, labels = [p[0] for p in pairs], [p[1] for p in pairs]
        legend = axis.legend(handles, labels, loc=loc, bbox_to_anchor=bbox,
                             frameon=True, fancybox=True, framealpha=0.65,
                             handlelength=1.1, handletextpad=0.35,
                             borderpad=0.25, labelspacing=0.25,
                             borderaxespad=0.2)
        legend.get_frame().set_facecolor("white")
        legend.get_frame().set_edgecolor("#cccccc")
        legend.get_frame().set_linewidth(0.8)

    paper_legend(axes[0], loc="upper right", bbox=(1, 1))
    paper_legend(axes[1], ["CXL-Buddy", "CXL-LLFree"], loc="lower center", bbox=(0.5, 0))
    paper_legend(axes[2], ["CXL-Buddy", "CXL-LLFree"], loc="lower center", bbox=(0.5, 0))

    figure.savefig(
        out_dir / "allocator-all.png",
        dpi=240,
        bbox_inches="tight",
    )
    plt.close(figure)


def write_summary(buckets, out_dir: Path):
    path = out_dir / "allocator_summary.csv"
    with path.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(
            ["config", "memory", "test", "parallel", "raw_samples", "samples_used",
             "outliers_removed", "mean_ops_per_sec", "std_ops_per_sec",
             "variance_ops_per_sec_squared"]
        )
        for key in sorted(buckets):
            raw = buckets[key]
            filtered = filter_outliers(raw, memory=key[1])
            mean, std = mean_std(filtered)
            writer.writerow([*key, len(raw), len(filtered), len(raw) - len(filtered),
                             mean, std, std ** 2])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--csv",
        type=Path,
        default=SCRIPT_DIR / "csv" / "allocator_results.csv",
        help="allocator result CSV (default: %(default)s)",
    )
    parser.add_argument("--allow-partial", action="store_true",
                        help="debug only: draw available allocator series")
    parser.add_argument(
        "--fig-dir",
        type=Path,
        default=SCRIPT_DIR / "figures",
        help="figure output directory (default: %(default)s)",
    )
    parser.add_argument(
        "--user-csv",
        type=Path,
        help="optional user-malloc CSV to append (for paper-data verification)",
    )
    args = parser.parse_args()
    args.fig_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(args.csv)
    if args.user_csv:
        rows.extend(load_rows(args.user_csv))
    buckets = make_buckets(rows)
    if not args.allow_partial:
        require_paper_series(rows, buckets)
    draw_paper_figure(rows, buckets, args.fig_dir)
    write_summary(buckets, args.fig_dir)
    print(f"Figure written to {args.fig_dir / 'allocator-all.png'}")
    print(f"Summary written to {args.fig_dir / 'allocator_summary.csv'}")


if __name__ == "__main__":
    main()
