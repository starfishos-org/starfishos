#!/usr/bin/env python3
"""Parse the queue-saturation sweep and plot tail latency vs throughput.

Camera-ready revision plan (Reviewer B on paper Figure 11b): tail latency
and saturation throughput per service queue.

Inputs (in --log-dir, produced by run.sh):
  machine1.log   client serial log with one [SUMMARY] + [TPUT] pair per
                 sweep point, mode-tagged sat_<queue>_t<threads>

Outputs:
  csv/saturation.csv           one row per (queue, threads) point
  figures/queue_saturation.png two panels: throughput vs offered load, and
                               p99 latency vs achieved throughput
"""
from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

SCRIPT_DIR = Path(__file__).resolve().parent

DEFAULT_QUEUES = ["empty", "read"]
DEFAULT_THREADS = [1, 2, 4, 8]

QUEUE_LABEL = {
    "empty": "No-op service queue",
    "read": "FS read (4KiB) service queue",
}
QUEUE_COLOR = {"empty": "#1f77b4", "read": "#d62728"}
QUEUE_MARKER = {"empty": "o", "read": "s"}

MODE_RE = re.compile(r"^sat_([a-z]+)_t(\d+)$")


def cpu_freq_hz(log: Path) -> float:
    pat = re.compile(r"cpu frequency=\(Dec\)(\d+)")
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pat.search(line)
        if match:
            return float(match.group(1))
    raise RuntimeError(f"missing CPU frequency in {log}")


def parse_points(log: Path) -> dict[tuple[str, int], dict[str, float]]:
    """Collect [SUMMARY] percentiles and [TPUT] wall clock per sweep tag."""
    freq = cpu_freq_hz(log)
    us = 1e6 / freq
    begin = re.compile(r"\[SUMMARY\]\s+mode=(\S+)\s+total=(\d+)\s+threads=(\d+)")
    values = re.compile(
        r"\[SUMMARY\]\s+p50=(\d+)\s+p75=(\d+)\s+p90=(\d+)\s+p99=(\d+)\s+max=(\d+)"
    )
    tput = re.compile(
        r"\[TPUT\]\s+mode=(\S+)\s+total=(\d+)\s+threads=(\d+)\s+wall_cycles=(\d+)"
    )

    points: dict[tuple[str, int], dict[str, float]] = {}
    current: tuple[str, int] | None = None
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = begin.search(line)
        if match:
            mode = MODE_RE.match(match.group(1))
            if mode is None:
                current = None
                continue
            current = (mode.group(1), int(mode.group(2)))
            points[current] = {
                "total": int(match.group(2)),
                "threads": int(match.group(3)),
            }
            continue
        match = values.search(line)
        if match and current is not None and "p50_us" not in points[current]:
            points[current].update(
                p50_us=int(match.group(1)) * us,
                p75_us=int(match.group(2)) * us,
                p90_us=int(match.group(3)) * us,
                p99_us=int(match.group(4)) * us,
                max_us=int(match.group(5)) * us,
            )
            continue
        match = tput.search(line)
        if match:
            mode = MODE_RE.match(match.group(1))
            if mode is None:
                continue
            key = (mode.group(1), int(mode.group(2)))
            entry = points.setdefault(key, {})
            total = int(match.group(2))
            wall_s = int(match.group(4)) / freq
            entry["total"] = total
            entry["wall_s"] = wall_s
            if wall_s > 0:
                entry["kops"] = total / wall_s / 1e3
    return points


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = ["queue", "threads", "total_ops", "wall_s", "kops",
              "p50_us", "p75_us", "p90_us", "p99_us", "max_us"]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def plot(fig_dir: Path, queues: list[str], series) -> None:
    fig_dir.mkdir(parents=True, exist_ok=True)
    plt.rcdefaults()
    fig, (ax_tput, ax_tail) = plt.subplots(1, 2, figsize=(9.6, 3.8))

    for queue in queues:
        rows = series[queue]
        if not rows:
            continue
        threads = [r["threads"] for r in rows]
        kops = [r["kops"] for r in rows]
        p99 = [r["p99_us"] for r in rows]
        label = QUEUE_LABEL.get(queue, queue)
        color = QUEUE_COLOR.get(queue, None)
        marker = QUEUE_MARKER.get(queue, "o")
        ax_tput.plot(threads, kops, marker=marker, color=color,
                     linewidth=2, markersize=6, label=label)
        ax_tail.plot(kops, p99, marker=marker, color=color,
                     linewidth=2, markersize=6, label=label)
        # Saturation throughput: the highest achieved rate in the sweep.
        sat = max(rows, key=lambda r: r["kops"])
        ax_tput.annotate(
            f"{sat['kops']:.0f} kops/s",
            (sat["threads"], sat["kops"]),
            textcoords="offset points", xytext=(0, 8),
            ha="center", fontsize=10, color=color,
        )

    ax_tput.set_xlabel("Client threads")
    ax_tput.set_ylabel("Throughput (kops/s)")
    ax_tput.set_title("Saturation throughput", fontsize=12)
    all_threads = sorted({r["threads"] for q in queues for r in series[q]})
    if all_threads:
        ax_tput.set_xticks(all_threads)
    ax_tput.set_ylim(bottom=0)
    ax_tail.set_xlabel("Throughput (kops/s)")
    ax_tail.set_ylabel("P99 latency (µs)")
    ax_tail.set_title("Tail latency vs throughput", fontsize=12)
    ax_tail.set_ylim(bottom=0)
    for ax in (ax_tput, ax_tail):
        ax.grid(True, linestyle=":", alpha=0.7)
        ax.set_axisbelow(True)
    handles, labels = ax_tput.get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=2, frameon=False,
               fontsize=11, bbox_to_anchor=(0.5, 1.06))
    fig.tight_layout()

    out = fig_dir / "queue_saturation.png"
    fig.savefig(out, dpi=300, bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {out}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log-dir", type=Path, default=SCRIPT_DIR / "logs")
    parser.add_argument("--csv-dir", type=Path, default=SCRIPT_DIR / "csv")
    parser.add_argument("--fig-dir", type=Path, default=SCRIPT_DIR / "figures")
    parser.add_argument("--queues", nargs="+", default=DEFAULT_QUEUES)
    parser.add_argument("--threads", type=int, nargs="+", default=DEFAULT_THREADS)
    parser.add_argument("--allow-partial", action="store_true",
                        help="debug only: plot whatever points parsed")
    args = parser.parse_args()

    log = args.log_dir / "machine1.log"
    if not log.is_file():
        raise FileNotFoundError(f"expected client log: {log}")

    points = parse_points(log)
    wanted = [(q, t) for q in args.queues for t in args.threads]
    complete = [key for key in wanted
                if key in points and "p99_us" in points[key]
                and "kops" in points[key]]
    missing = [f"sat_{q}_t{t}" for q, t in wanted if (q, t) not in complete]
    if missing and not args.allow_partial:
        raise SystemExit("incomplete queue-saturation sweep; missing: "
                         + ", ".join(missing))

    rows = []
    series = {q: [] for q in args.queues}
    for queue, threads in sorted(complete, key=lambda k: (k[0], k[1])):
        p = points[queue, threads]
        row = {
            "queue": queue,
            "threads": threads,
            "total_ops": int(p["total"]),
            "wall_s": round(p["wall_s"], 6),
            "kops": round(p["kops"], 3),
            "p50_us": round(p["p50_us"], 3),
            "p75_us": round(p["p75_us"], 3),
            "p90_us": round(p["p90_us"], 3),
            "p99_us": round(p["p99_us"], 3),
            "max_us": round(p["max_us"], 3),
        }
        rows.append(row)
        series[queue].append(row)

    args.csv_dir.mkdir(parents=True, exist_ok=True)
    write_csv(args.csv_dir / "saturation.csv", rows)

    for queue in args.queues:
        if not series[queue]:
            continue
        sat = max(series[queue], key=lambda r: r["kops"])
        print(f"{queue}: saturation throughput {sat['kops']:.1f} kops/s "
              f"at {sat['threads']} threads (p99 {sat['p99_us']:.1f} µs)")

    plot(args.fig_dir, args.queues, series)
    print(f"Wrote CSV to {args.csv_dir} and figures to {args.fig_dir}")


if __name__ == "__main__":
    main()
