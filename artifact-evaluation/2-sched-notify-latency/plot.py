#!/usr/bin/env python3
"""Summarize local/cross-machine sched/notify samples and draw the AE figure."""

import argparse
import csv
import math
import re
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


SAMPLE_RE = re.compile(
    r"\[SCHED_NOTIFY_BENCH\] "
    r"metric=(local_sched|local_notify|cross_sched|cross_notify|sched|notify) "
    r"sample=(\d+) latency_ns=(\d+)"
)
LABELS = {
    "local_sched": "Local sched",
    "cross_sched": "Cross-machine sched",
    "local_notify": "Local notify",
    "cross_notify": "Cross-machine notify",
    "sched": "Remote scheduling",
    "notify": "Remote notification",
}
CHCORE_METRICS = (
    "local_sched",
    "cross_sched",
    "local_notify",
    "cross_notify",
)
LEGACY_METRICS = ("sched", "notify")
SCRIPT_DIR = Path(__file__).resolve().parent


def save_paper_figure(fig_dir: Path, stem: str, fig, *, dpi: int = 180) -> None:
    fig.savefig((fig_dir / stem).with_suffix(".png"), dpi=dpi, bbox_inches="tight")


def run_number(path: Path) -> int:
    match = re.search(r"run(\d+)", str(path))
    return int(match.group(1)) if match else 1


def parse_logs(log_dir: Path):
    samples = []
    for path in sorted(log_dir.rglob("machine0.log"), key=run_number):
        run = run_number(path)
        for line in path.read_text(errors="replace").splitlines():
            match = SAMPLE_RE.search(line)
            if match:
                samples.append(
                    {
                        "run": run,
                        "metric": match.group(1),
                        "sample": int(match.group(2)),
                        "latency_ns": int(match.group(3)),
                    }
                )
    return samples


def mean_std(values):
    mean = sum(values) / len(values)
    variance = sum((value - mean) ** 2 for value in values) / len(values)
    return mean, math.sqrt(variance)


def write_outputs(samples, csv_dir: Path, fig_dir: Path):
    csv_dir.mkdir(parents=True, exist_ok=True)
    fig_dir.mkdir(parents=True, exist_ok=True)
    with (csv_dir / "samples.csv").open("w", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=samples[0].keys(), lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(samples)

    by_run = defaultdict(list)
    for sample in samples:
        by_run[(sample["metric"], sample["run"])].append(sample["latency_ns"])

    # Each guest invocation first averages its repeated samples.  The plotted
    # value and error bar are then the mean/std across independent VM runs.
    run_means = defaultdict(list)
    for (metric, _run), values in by_run.items():
        run_means[metric].append(sum(values) / len(values))

    metrics = CHCORE_METRICS if set(run_means) & set(CHCORE_METRICS) else LEGACY_METRICS
    missing = set(metrics) - set(run_means)
    if missing:
        raise ValueError(f"missing metrics: {', '.join(sorted(missing))}")

    summary = []
    for metric in metrics:
        raw = [s["latency_ns"] for s in samples if s["metric"] == metric]
        means = run_means[metric]
        mean, run_std = mean_std(means)
        summary.append(
            {
                "metric": metric,
                "runs": len(means),
                "samples": len(raw),
                "avg_ns": mean,
                "run_std_ns": run_std,
                "min_ns": min(raw),
                "max_ns": max(raw),
            }
        )

    with (csv_dir / "summary.csv").open("w", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=summary[0].keys(), lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(summary)

    figure, axis = plt.subplots(figsize=(6.2, 4.2))
    axis.bar(
        range(len(summary)),
        [row["avg_ns"] / 1000 for row in summary],
        yerr=[row["run_std_ns"] / 1000 for row in summary],
        color=["#4C78A8", "#E45756", "#72B7B2", "#F58518"][:len(summary)],
        capsize=5,
        width=0.62,
    )
    axis.set_xticks(
        range(len(summary)),
        [LABELS[row["metric"]] for row in summary],
    )
    axis.set_ylabel("End-to-end latency (µs, mean ± run std)")
    axis.set_title("Scheduling and notification latency")
    axis.grid(axis="y", alpha=0.3)
    figure.tight_layout()
    save_paper_figure(fig_dir, "sched_notify_latency", figure)
    plt.close(figure)

    for row in summary:
        print(
            f"{row['metric']}_latency: runs={row['runs']} "
            f"samples={row['samples']} avg_us={row['avg_ns'] / 1000:.3f} "
            f"run_std_us={row['run_std_ns'] / 1000:.3f}"
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--log-dir",
        type=Path,
        default=SCRIPT_DIR / "logs",
        help="benchmark log directory (default: %(default)s)",
    )
    parser.add_argument(
        "--csv-dir",
        type=Path,
        default=SCRIPT_DIR / "csv",
        help="CSV output directory (default: %(default)s)",
    )
    parser.add_argument(
        "--fig-dir",
        type=Path,
        default=SCRIPT_DIR / "figures",
        help="figure output directory (default: %(default)s)",
    )
    args = parser.parse_args()

    samples = parse_logs(args.log_dir)
    if not samples:
        raise SystemExit(f"no microbenchmark samples found under {args.log_dir}")
    write_outputs(samples, args.csv_dir, args.fig_dir)


if __name__ == "__main__":
    main()
