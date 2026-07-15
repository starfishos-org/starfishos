#!/usr/bin/env python3
"""Draw the paper-style LevelDB recovery timeline from one real recovery run."""

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

AE_DIR = Path(__file__).resolve().parent


def load_csv(path):
    with path.open(newline="") as source:
        return list(csv.DictReader(source))


def as_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def tint(rgb, amount):
    """Mix a color with white; unlike alpha, this renders correctly in EPS."""
    return tuple((1.0 - amount) + amount * channel for channel in rgb[:3])


def preferred_ms(row):
    return as_float(row.get("guest_reported_ms") or row.get("measured_ms"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--detail", type=Path, default=AE_DIR / "recovery_detail.csv")
    parser.add_argument("--throughput", type=Path, default=AE_DIR / "throughput.csv")
    parser.add_argument("--out-dir", type=Path, default=AE_DIR)
    args = parser.parse_args()
    detail = load_csv(args.detail)
    points = load_csv(args.throughput)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    required_columns = {"event", "elapsed_ms", "workload", "ops_per_sec"}
    if not points or not required_columns.issubset(points[0]):
        raise ValueError(
            "throughput CSV must contain event, elapsed_ms, workload, ops_per_sec"
        )

    by_key = {(row["event"], row["workload"]): row for row in points}
    detail_by_stage = {row["stage"]: row for row in detail}
    for event in (
        "crash_started",
        "machine0_killed",
        "machine0_detected",
        "fs_recovered",
        "leveldb_reopened",
        "post_read_completed",
    ):
        if (event, "read") not in by_key:
            raise ValueError(f"missing throughput point: {event}/read")

    # Accept both old CSVs (which included the pre-crash delay) and new CSVs
    # (which are already crash-relative), but always draw the crash at t=0.
    crash_ms = as_float(by_key[("crash_started", "read")]["elapsed_ms"])

    def event_s(event):
        elapsed_ms = as_float(by_key[(event, "read")]["elapsed_ms"])
        return max(0.0, (elapsed_ms - crash_ms) / 1000.0)

    crash_s = 0.0
    detect_s = event_s("machine0_detected")
    fs_s = event_s("fs_recovered")
    leveldb_s = event_s("leveldb_reopened")
    read_done_s = event_s("post_read_completed")
    has_warmup = ("post_read_warmup", "read") in by_key

    colormap = plt.colormaps["tab20"]
    blue = colormap(0)
    red = colormap(4)
    green = colormap(6)
    stage_colors = [blue, green, red]

    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams["pdf.use14corefonts"] = True
    plt.rcParams.update({"font.size": 20, "figure.figsize": (11.0, 3.2)})
    fig, axis = plt.subplots()

    # Match the paper's three recovery regions: failure detection, filesystem
    # restart including p-log replay, and LevelDB restart.
    boundaries = [crash_s, detect_s, fs_s, leveldb_s]
    for index, (start, end) in enumerate(zip(boundaries, boundaries[1:])):
        axis.axvspan(
            start,
            end,
            facecolor=tint(stage_colors[index], 0.19),
            edgecolor="none",
            zorder=1,
        )
    for boundary in boundaries[1:-1]:
        axis.axvline(boundary, color="0.45", linestyle="--", linewidth=0.9, zorder=2)
    axis.axvline(leveldb_s, color=red, linestyle="-", linewidth=1.4, zorder=4)

    event_order = [
        "crash_started",
        "machine0_killed",
        "machine0_detected",
        "fs_recovered",
        "leveldb_reopened",
    ]
    if has_warmup:
        event_order.append("post_read_warmup")
    event_order.append("post_read_completed")
    selected = [by_key[(event, "read")] for event in event_order]
    time_s = [event_s(event) for event in event_order]
    rates = [as_float(row["ops_per_sec"]) / 1000.0 for row in selected]
    axis.plot(
        time_s,
        rates,
        label="Recovered Read",
        color=blue,
        linewidth=2.4,
        zorder=3,
    )
    axis.scatter(
        time_s[-(2 if has_warmup else 1):],
        rates[-(2 if has_warmup else 1):],
        color=blue,
        s=26,
        zorder=4,
    )

    ymax = max(rates) if rates else 1.0
    ymax = max(ymax, 1.0)
    axis.annotate(
        "LevelDB Restarted",
        xy=(leveldb_s, ymax * 0.04),
        xytext=(max(leveldb_s - 0.08, 0), ymax * 0.24),
        ha="right",
        arrowprops={"facecolor": red, "arrowstyle": "->"},
        color=red,
        fontsize=20,
        zorder=5,
    )

    # The paper overlays a compact horizontal breakdown bar on the throughput
    # curve. Use the measured/guest-reported durations but keep the bar in an
    # inset so short recovery stages remain legible at any benchmark duration.
    breakdown = [
        ("Crash Detect", preferred_ms(detail_by_stage.get("detect_machine0", {}))),
        ("FS Restart (+p-log apply)", preferred_ms(detail_by_stage.get("restart_fs", {}))),
        ("LevelDB Restart", preferred_ms(detail_by_stage.get("restart_leveldb", {}))),
    ]
    breakdown_total = sum(value for _, value in breakdown)
    if breakdown_total > 0:
        inset = axis.inset_axes([0.34, 0.57, 0.62, 0.32], zorder=6)
        left = 0.0
        for index, (label, value) in enumerate(breakdown):
            inset.barh(
                0,
                value,
                height=0.52,
                left=left,
                color=stage_colors[index],
                edgecolor="black",
                linewidth=0.7,
            )
            inset.annotate(
                f"{label}\n{value:g}ms",
                xy=(left + value * 0.5, 0),
                xytext=(left + value * 0.5, 0.42 if index % 2 == 0 else -0.42),
                ha="center",
                va="bottom" if index % 2 == 0 else "top",
                fontsize=10,
                arrowprops={"arrowstyle": "->", "color": "black", "linewidth": 0.7},
            )
            left += value
        inset.set_xlim(0, breakdown_total)
        inset.set_ylim(-1.0, 1.0)
        inset.axis("off")

    xmax = max(read_done_s, leveldb_s + 0.2)
    axis.set_xlim(0, xmax * 1.03)
    axis.set_ylim(0, ymax * 1.12)
    axis.set_ylabel("THP (kops/s)", fontsize=23, labelpad=2)
    axis.set_xlabel("Time (s)", fontsize=23)
    tick_step = 0.25 if xmax < 2.0 else 1.0
    axis.xaxis.set_major_locator(mticker.MultipleLocator(tick_step))
    axis.xaxis.set_major_formatter(mticker.FormatStrFormatter("%.2g"))
    axis.grid(True, linestyle="--", alpha=1.0, zorder=0)
    axis.legend(
        frameon=False,
        loc="upper left",
        ncols=2,
        handlelength=1.8,
        fontsize=16,
    )

    fig.tight_layout()
    for stem in ("recovery-performance-single", "recovery-performance"):
        fig.savefig(args.out_dir / f"{stem}.png", dpi=240, bbox_inches="tight")
        fig.savefig(args.out_dir / f"{stem}.pdf", format="pdf", bbox_inches="tight")
    fig.savefig(
        args.out_dir / "recovery-performance-single.eps",
        format="eps",
        bbox_inches="tight",
    )
    plt.close(fig)


if __name__ == "__main__":
    main()
