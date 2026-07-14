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


def load_rows(path: Path):
    rows = []
    with path.open(newline="") as source:
        for row in csv.DictReader(source):
            row["parallel"] = int(row["parallel"])
            row["run"] = int(row["run"])
            row["ops_per_sec"] = float(row["ops_per_sec"])
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


def make_buckets(rows):
    buckets = defaultdict(list)
    for row in rows:
        key = (row["config"], row["memory"], row["test"], row["parallel"])
        buckets[key].append(row["ops_per_sec"])
    return buckets


def values_for(buckets, test, config, memory):
    points = []
    parallels = sorted(
        key[3]
        for key in buckets
        if key[0] == config and key[1] == memory and key[2] == test
    )
    for parallel in parallels:
        mean, std = mean_std(buckets[(config, memory, test, parallel)])
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

    styles = {
        "DRAM": ("black", "^"),
        "CXL": ("#2ca02c", "P"),
        "CXL-Log": ("#ff7f0e", "X"),
        "CXL-Buddy": ("#1f77b4", "o"),
        "CXL-LLFree": ("#d62728", "s"),
    }

    def paper_series(axis, test, config, memory, label):
        points = values_for(buckets, test, config, memory)
        if not points:
            return
        color, marker = styles[label]
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
            capsize=2.5,
        )

    figure, axes = plt.subplots(1, 3, figsize=(8.0, 3.0), constrained_layout=True)

    # Keep these mappings aligned with p3os-paper/eval/malloc/
    # plot_combined_allocator_figure.py.
    paper_series(axes[0], "kmalloc", "LLFree+CR", "DRAM", "DRAM")
    paper_series(axes[0], "kmalloc", "Buddy", "CXL", "CXL")
    paper_series(axes[0], "kmalloc", "LLFree", "CXL", "CXL-Log")

    paper_series(axes[1], "random_get_free_4K2M", "LLFree+CR", "DRAM", "DRAM")
    paper_series(axes[1], "random_get_free_4K2M", "Buddy", "CXL", "CXL-Buddy")
    paper_series(axes[1], "random_get_free_4K2M", "LLFree+CR", "CXL", "CXL-LLFree")

    user_test = user_tests[0]
    paper_series(axes[2], user_test, "LLFree+CR", "user", "DRAM")
    paper_series(axes[2], user_test, "Buddy", "user", "CXL-Buddy")
    paper_series(axes[2], user_test, "LLFree", "user", "CXL-LLFree")

    for axis, title in zip(axes, ["(a) Slab", "(b) Buddy", "(c) rpmalloc"]):
        axis.set_title(title, fontweight="bold", pad=12)
        axis.set_xlabel("#Threads")
        axis.set_ylabel("Thp (Mops/s)")
        axis.set_ylim(bottom=0)
        axis.set_xlim(left=0, right=100)
        axis.set_xticks([1, 32, 64, 96])
        axis.grid(True, which="both", axis="y", linestyle=":")
        axis.legend(fontsize=8, frameon=False)

    figure.savefig(
        out_dir / "fig00-allocator-all.png",
        dpi=240,
        bbox_inches="tight",
    )
    plt.close(figure)


def write_summary(buckets, out_dir: Path):
    path = out_dir / "allocator_summary.csv"
    with path.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(
            ["config", "memory", "test", "parallel", "samples", "mean_ops_per_sec", "std_ops_per_sec"]
        )
        for key in sorted(buckets):
            mean, std = mean_std(buckets[key])
            writer.writerow([*key, len(buckets[key]), mean, std])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--csv",
        type=Path,
        default=SCRIPT_DIR / "allocator_results.csv",
        help="allocator result CSV (default: %(default)s)",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=SCRIPT_DIR,
        help="output directory (default: %(default)s)",
    )
    args = parser.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    rows = load_rows(args.csv)
    buckets = make_buckets(rows)
    draw_paper_figure(rows, buckets, args.out_dir)
    print(f"Figure written to {args.out_dir / 'fig00-allocator-all.png'}")


if __name__ == "__main__":
    main()
