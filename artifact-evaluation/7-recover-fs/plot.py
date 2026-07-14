#!/usr/bin/env python3
"""Draw the paper-style LevelDB recovery timeline from one real recovery run."""

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load_csv(path):
    with path.open(newline="") as source:
        return list(csv.DictReader(source))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--detail", type=Path, required=True)
    parser.add_argument("--throughput", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    detail = load_csv(args.detail)
    points = load_csv(args.throughput)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    event = {row["event"]: row for row in points}
    kill_ms = float(event["machine0_killed"]["elapsed_ms"])
    detect_ms = float(event["machine0_detected"]["elapsed_ms"])
    fs_ms = float(event["fs_recovered"]["elapsed_ms"])
    db_ms = float(event["leveldb_reopened"]["elapsed_ms"])
    baseline = float(event["pre_crash"]["ops_per_sec"])
    recovered = float(event["leveldb_reopened"]["ops_per_sec"])
    labels = {row["stage"]: row for row in detail}

    fig, axis = plt.subplots(figsize=(8.4, 3.2), constrained_layout=True)
    # The line contains only actual db_bench measurements; the zero segment is
    # the interval in which the original QEMU was dead and recovery ran.
    x = [0, kill_ms, fs_ms, db_ms]
    y = [baseline, 0, 0, recovered]
    axis.plot(x, [value / 1000 for value in y], color="#4C78A8", marker="o", linewidth=2,
              label="LevelDB throughput (measured)")

    axis.axvspan(kill_ms, fs_ms, color="#54A24B", alpha=0.16, label="FS recovery")
    axis.axvspan(fs_ms, db_ms, color="#E45756", alpha=0.13, label="LevelDB restart")
    axis.axvline(kill_ms, color="#54A24B", linestyle="--", linewidth=1.2)
    axis.axvline(detect_ms, color="#54A24B", linestyle=":", linewidth=1.2)
    axis.axvline(fs_ms, color="#54A24B", linestyle="--", linewidth=1.2)
    axis.axvline(db_ms, color="#E45756", linestyle="--", linewidth=1.2)

    fs_guest = labels.get("restart_fs", {}).get("guest_reported_ms") or "?"
    db_guest = labels.get("restart_leveldb", {}).get("guest_reported_ms") or "?"
    axis.annotate("machine 0\nkilled", (kill_ms, 0), xytext=(6, 24), textcoords="offset points",
                  color="#2E7D32", ha="left", arrowprops={"arrowstyle": "-"})
    axis.annotate("failure\ndetected", (detect_ms, 0), xytext=(6, 8), textcoords="offset points",
                  color="#2E7D32", ha="left", arrowprops={"arrowstyle": "-"})
    axis.annotate(f"FS recovered\n({fs_guest} ms guest)", (fs_ms, 0), xytext=(6, 50),
                  textcoords="offset points", color="#2E7D32", ha="left",
                  arrowprops={"arrowstyle": "-"})
    axis.annotate(f"LevelDB started\n({db_guest} ms DB::Open)", (db_ms, recovered / 1000),
                  xytext=(8, -42), textcoords="offset points", color="#B23A48", ha="left",
                  arrowprops={"arrowstyle": "->", "color": "#B23A48"})

    axis.set_xlabel("Recovery timeline (ms)")
    axis.set_ylabel("Throughput (Kops/s)")
    axis.set_ylim(bottom=0)
    axis.grid(axis="y", linestyle=":", alpha=0.7)
    axis.legend(frameon=False, loc="upper left")
    fig.savefig(args.out_dir / "recovery-performance.png", dpi=240)
    fig.savefig(args.out_dir / "recovery-performance.pdf", bbox_inches="tight")


if __name__ == "__main__":
    main()
