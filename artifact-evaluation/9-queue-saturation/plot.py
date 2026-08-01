#!/usr/bin/env python3
"""Parse the queue-saturation sweep and plot throughput vs client threads.

Camera-ready revision plan (Reviewer B on paper Figure 11b): tail latency
and saturation throughput per service queue.

Inputs (in --log-dir, produced by run.sh):
  machine1.log   client serial log with one [SUMMARY] + [TPUT] pair per
                 sweep point, mode-tagged sat_<queue>_t<threads>

Outputs:
  csv/trials.csv               one row per raw repeat
  csv/saturation.csv           per-(queue, threads) trimmed means
  csv/queue_summary.csv        saturation status and peak per queue
  figures/queue_saturation.png throughput vs client threads
"""
from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path
from statistics import mean

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

SCRIPT_DIR = Path(__file__).resolve().parent

DEFAULT_QUEUES = ["empty", "read"]
# Must stay in step with run.sh's THREADS default, and run.sh rejects any entry
# >= its guest vCPU count -- so this list and QSAT_CPU_NUM move together.  The
# camera-ready sweep is 1..12 client threads on a 32-vCPU guest
# (out/qsat32_*_20260730); naming a thread count run.sh will not produce makes
# every standalone replot fail as "incomplete".
DEFAULT_THREADS = [1, 2, 4, 6, 8, 10, 12]
DEFAULT_PLATEAU_THRESHOLD_PCT = 5.0
# Y-axis floor, so a run whose numbers land under the submitted figure's range
# still renders at the submitted scale.  Not a ceiling -- see plot().
PAPER_YMAX = 850.0

QUEUE_LABEL = {
    "empty": "Empty IPC",
    "read": "Read 4KiB",
}
QUEUE_COLOR = {"empty": "#1f77b4", "read": "#2ca02c"}
QUEUE_MARKER = {"empty": "o", "read": "s"}

MODE_RE = re.compile(r"^sat_([a-z]+)_t(\d+)(?:_r(\d+))?$")


def cpu_freq_hz(log: Path) -> float:
    pat = re.compile(r"cpu frequency=\(Dec\)(\d+)")
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pat.search(line)
        if match:
            return float(match.group(1))
    raise RuntimeError(f"missing CPU frequency in {log}")


def parse_points(log: Path) -> dict[tuple[str, int, int], dict[str, object]]:
    """Collect internally consistent, process-complete sweep points."""
    freq = cpu_freq_hz(log)
    us = 1e6 / freq
    begin = re.compile(r"\[SUMMARY\]\s+mode=(\S+)\s+total=(\d+)\s+threads=(\d+)")
    values = re.compile(
        r"\[SUMMARY\]\s+p50=(\d+)\s+p75=(\d+)\s+p90=(\d+)\s+p99=(\d+)\s+max=(\d+)"
    )
    tput = re.compile(
        r"\[TPUT\]\s+mode=(\S+)\s+total=(\d+)\s+threads=(\d+)\s+wall_cycles=(\d+)"
    )
    exited = re.compile(r"^queue_client_exited:(\S+):(\d+)\s*$")
    shell_prompt = re.compile(r"^\$[ \t]*$")

    points: dict[tuple[str, int, int], dict[str, object]] = {}
    current: tuple[str, int, int] | None = None
    pending_exit: tuple[str, int, int] | None = None
    for lineno, raw_line in enumerate(
        log.read_text(encoding="utf-8", errors="replace").splitlines(), start=1
    ):
        line = raw_line.rstrip("\r")
        match = begin.search(line)
        if match:
            tag = match.group(1)
            mode = MODE_RE.fullmatch(tag)
            if mode is None:
                current = None
                continue
            tag_threads = int(mode.group(2))
            summary_threads = int(match.group(3))
            if tag_threads != summary_threads:
                raise RuntimeError(
                    f"{log}:{lineno}: SUMMARY tag threads {tag_threads} "
                    f"!= threads {summary_threads} for {tag}"
                )
            current = (mode.group(1), tag_threads, int(mode.group(3) or 1))
            if current in points:
                raise RuntimeError(
                    f"{log}:{lineno}: duplicate SUMMARY for {tag}"
                )
            points[current] = {
                "tag": tag,
                "total": int(match.group(2)),
                "threads": summary_threads,
            }
            continue
        match = values.search(line)
        if match and current is not None:
            entry = points[current]
            if "p50_us" in entry:
                raise RuntimeError(
                    f"{log}:{lineno}: duplicate percentile SUMMARY for {entry['tag']}"
                )
            entry.update(
                p50_us=int(match.group(1)) * us,
                p75_us=int(match.group(2)) * us,
                p90_us=int(match.group(3)) * us,
                p99_us=int(match.group(4)) * us,
                max_us=int(match.group(5)) * us,
            )
            continue
        match = tput.search(line)
        if match:
            tag = match.group(1)
            mode = MODE_RE.fullmatch(tag)
            if mode is None:
                continue
            tag_threads = int(mode.group(2))
            tput_threads = int(match.group(3))
            if tag_threads != tput_threads:
                raise RuntimeError(
                    f"{log}:{lineno}: TPUT tag threads {tag_threads} "
                    f"!= threads {tput_threads} for {tag}"
                )
            key = (mode.group(1), tag_threads, int(mode.group(3) or 1))
            if current != key or key not in points:
                raise RuntimeError(
                    f"{log}:{lineno}: TPUT {tag} has no matching preceding SUMMARY"
                )
            entry = points[key]
            if entry["tag"] != tag:
                raise RuntimeError(
                    f"{log}:{lineno}: SUMMARY mode {entry['tag']} != TPUT mode {tag}"
                )
            total = int(match.group(2))
            if total != entry["total"] or tput_threads != entry["threads"]:
                raise RuntimeError(
                    f"{log}:{lineno}: SUMMARY/TPUT total or threads mismatch for {tag}"
                )
            if "wall_s" in entry:
                raise RuntimeError(f"{log}:{lineno}: duplicate TPUT for {tag}")
            wall_s = int(match.group(4)) / freq
            entry["wall_s"] = wall_s
            if wall_s > 0:
                entry["kops"] = total / wall_s / 1e3
            current = None
            continue
        match = exited.fullmatch(line)
        if match:
            tag = match.group(1)
            mode = MODE_RE.fullmatch(tag)
            pending_exit = None
            if mode is None or int(match.group(2)) != 0:
                continue
            key = (mode.group(1), int(mode.group(2)), int(mode.group(3) or 1))
            if key not in points or points[key]["tag"] != tag:
                raise RuntimeError(
                    f"{log}:{lineno}: successful exit has no matching point for {tag}"
                )
            if points[key].get("exit_zero"):
                raise RuntimeError(f"{log}:{lineno}: duplicate successful exit for {tag}")
            points[key]["exit_zero"] = True
            pending_exit = key
            continue
        if pending_exit is not None and shell_prompt.fullmatch(line):
            points[pending_exit]["shell_returned"] = True
            pending_exit = None
    return points


def write_csv(
    path: Path, rows: list[dict[str, object]], fields: list[str]
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def classify_saturation(
    rows: list[dict[str, object]], threshold_pct: float
) -> dict[str, object]:
    """Call saturation only after the final two load steps stop scaling.

    Requiring two consecutive low-gain intervals prevents one noisy dip from
    turning an earlier sample maximum into a claimed saturation throughput.
    Otherwise the sweep only establishes a lower bound on peak throughput.
    """
    required_intervals = 2
    peak = max(rows, key=lambda row: float(row["kops"]))
    gains_pct: list[float] = []
    for previous_row, current_row in zip(rows, rows[1:]):
        previous = float(previous_row["kops"])
        if previous > 0:
            gains_pct.append(
                (float(current_row["kops"]) - previous) / previous * 100.0
            )

    recent_gains = gains_pct[-required_intervals:]
    saturated = (
        len(recent_gains) == required_intervals
        and all(gain <= threshold_pct for gain in recent_gains)
    )

    return {
        "status": "saturated" if saturated else "not_reached",
        "peak_kops": float(peak["kops"]),
        "peak_threads": int(peak["threads"]),
        "peak_p99_us": float(peak["p99_us"]),
        "last_gain_pct": gains_pct[-1] if gains_pct else None,
        "recent_gains_pct": ";".join(f"{gain:.6f}" for gain in recent_gains),
        "plateau_threshold_pct": threshold_pct,
        "plateau_intervals_required": required_intervals,
    }


def write_queue_summary(path: Path, summaries: dict[str, dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "queue", "status", "peak_kops", "peak_threads", "peak_p99_us",
        "last_gain_pct", "recent_gains_pct", "plateau_threshold_pct",
        "plateau_intervals_required",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for queue, summary in summaries.items():
            writer.writerow({"queue": queue, **summary})


def aggregate_trials(
    grouped: dict[tuple[str, int], list[dict[str, object]]],
    queues: list[str],
) -> tuple[list[dict[str, object]], dict[str, list[dict[str, object]]]]:
    """Drop the lowest/highest-throughput trial, then average the rest.

    With fewer than three trials there is nothing left after trimming, so the
    plain mean of every trial is used instead and the row says so
    (kept_repeats == repeats, empty dropped_*_kops).  A thin run is a debug
    run, not a reason to throw away a sweep that already took hours; the
    paper's REPEATS=3 is what actually gets trimmed.
    """
    rows: list[dict[str, object]] = []
    series: dict[str, list[dict[str, object]]] = {queue: [] for queue in queues}
    metric_fields = [
        "wall_s", "kops", "p50_us", "p75_us", "p90_us", "p99_us", "max_us"
    ]

    for (queue, threads), trials in sorted(grouped.items()):
        if queue not in series:
            # parse_points may see queues the caller did not ask about.
            continue
        ordered = sorted(trials, key=lambda trial: float(trial["kops"]))
        if len(ordered) >= 3:
            kept = ordered[1:-1]
            dropped_low = round(float(ordered[0]["kops"]), 3)
            dropped_high = round(float(ordered[-1]["kops"]), 3)
        else:
            print(f"[WARN] {queue} at {threads} threads has {len(ordered)} "
                  "trial(s); averaging all of them without dropping outliers "
                  "(REPEATS=3 or more is what the paper reports)")
            kept = ordered
            dropped_low = dropped_high = ""
        row: dict[str, object] = {
            "queue": queue,
            "threads": threads,
            "repeats": len(trials),
            "kept_repeats": len(kept),
            "dropped_low_kops": dropped_low,
            "dropped_high_kops": dropped_high,
            "total_ops": int(round(mean(int(t["total_ops"]) for t in kept))),
        }
        for field in metric_fields:
            precision = 6 if field == "wall_s" else 3
            row[field] = round(mean(float(t[field]) for t in kept), precision)
        rows.append(row)
        series[queue].append(row)

    return rows, series


def plot(fig_dir: Path, queues: list[str], series, _summaries) -> None:
    fig_dir.mkdir(parents=True, exist_ok=True)
    plt.rcdefaults()
    # Match p3os-paper/eval/ipc_cdf/plot_queue_saturation_1_10.py.
    plt.rcParams.update(
        {
            "font.size": 14,
            "axes.labelsize": 15,
            "xtick.labelsize": 13,
            "ytick.labelsize": 13,
            "legend.fontsize": 14,
        }
    )
    fig, ax_tput = plt.subplots(figsize=(3.7, 3.15))

    for queue in queues:
        rows = series[queue]
        if not rows:
            continue
        threads = [r["threads"] for r in rows]
        kops = [r["kops"] for r in rows]
        label = QUEUE_LABEL.get(queue, queue)
        color = QUEUE_COLOR.get(queue, None)
        marker = QUEUE_MARKER.get(queue, "o")
        ax_tput.plot(threads, kops, marker=marker, color=color,
                     linewidth=2, markersize=6, label=label)

    ax_tput.set_xlabel("Client threads")
    ax_tput.set_ylabel("Throughput (kops/s)")
    all_threads = sorted({r["threads"] for q in queues for r in series[q]})
    if all_threads:
        ax_tput.set_xticks(all_threads)
    # Size the axis from the data.  A fixed limit (this was 850, matching one
    # particular measurement of the empty queue) silently clips the curve flat
    # the moment a faster placement or a longer thread axis exceeds it, which
    # hides exactly the saturation knee the figure exists to show.  PAPER_YMAX
    # keeps the submitted look when the data fits under it.
    all_kops = [r["kops"] for q in queues for r in series[q]]
    ax_tput.set_ylim(0, max([PAPER_YMAX] + [k * 1.08 for k in all_kops]))
    ax_tput.grid(True, alpha=0.28, linewidth=0.7)
    ax_tput.set_axisbelow(True)
    ax_tput.legend(
        loc="upper right", frameon=False, fontsize=10, handlelength=1.6,
        handletextpad=0.45, labelspacing=0.25, borderaxespad=0.3,
    )
    fig.subplots_adjust(left=0.19, right=0.985, top=0.98, bottom=0.19)

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
    parser.add_argument(
        "--repeats", type=int, default=None,
        help="expected repeats per point; auto-detected when omitted",
    )
    parser.add_argument(
        "--plateau-threshold-pct", type=float,
        default=DEFAULT_PLATEAU_THRESHOLD_PCT,
        help="maximum final throughput gain considered a plateau (default: %(default)s)",
    )
    parser.add_argument("--allow-partial", action="store_true",
                        help="debug only: plot whatever points parsed")
    args = parser.parse_args()
    if len(set(args.threads)) != len(args.threads):
        raise SystemExit("--threads must not contain duplicates")
    if len(set(args.queues)) != len(args.queues):
        raise SystemExit("--queues must not contain duplicates")
    if args.plateau_threshold_pct < 0:
        raise SystemExit("--plateau-threshold-pct must be non-negative")
    if args.repeats is not None and args.repeats <= 0:
        raise SystemExit("--repeats must be a positive integer")

    log = args.log_dir / "machine1.log"
    if not log.is_file():
        raise FileNotFoundError(f"expected client log: {log}")

    points = parse_points(log)
    detected_repeats = max((key[2] for key in points), default=1)
    repeats = args.repeats if args.repeats is not None else detected_repeats
    wanted = [
        (q, t, repeat)
        for q in args.queues for t in args.threads
        for repeat in range(1, repeats + 1)
    ]
    complete = [key for key in wanted
                if key in points and "p99_us" in points[key]
                and "kops" in points[key]
                and points[key].get("exit_zero") is True
                and points[key].get("shell_returned") is True]
    missing = [
        f"sat_{q}_t{t}_r{repeat}"
        for q, t, repeat in wanted if (q, t, repeat) not in complete
    ]
    if missing and not args.allow_partial:
        raise SystemExit("incomplete queue-saturation sweep; missing: "
                         + ", ".join(missing))

    trial_rows = []
    grouped: dict[tuple[str, int], list[dict[str, object]]] = {}
    series = {q: [] for q in args.queues}
    for queue, threads, repeat in sorted(complete):
        p = points[queue, threads, repeat]
        row = {
            "queue": queue,
            "threads": threads,
            "repeat": repeat,
            "total_ops": int(p["total"]),
            "wall_s": round(p["wall_s"], 6),
            "kops": round(p["kops"], 3),
            "p50_us": round(p["p50_us"], 3),
            "p75_us": round(p["p75_us"], 3),
            "p90_us": round(p["p90_us"], 3),
            "p99_us": round(p["p99_us"], 3),
            "max_us": round(p["max_us"], 3),
        }
        trial_rows.append(row)
        grouped.setdefault((queue, threads), []).append(row)

    rows, series = aggregate_trials(grouped, args.queues)

    args.csv_dir.mkdir(parents=True, exist_ok=True)
    value_fields = [
        "total_ops", "wall_s", "kops", "p50_us", "p75_us", "p90_us",
        "p99_us", "max_us",
    ]
    write_csv(
        args.csv_dir / "trials.csv", trial_rows,
        ["queue", "threads", "repeat", *value_fields],
    )
    write_csv(
        args.csv_dir / "saturation.csv", rows,
        [
            "queue", "threads", "repeats", "kept_repeats",
            "dropped_low_kops", "dropped_high_kops", *value_fields,
        ],
    )

    summaries = {}
    for queue in args.queues:
        if not series[queue]:
            continue
        summary = classify_saturation(series[queue], args.plateau_threshold_pct)
        summaries[queue] = summary
        if summary["status"] == "saturated":
            print(f"{queue}: saturation throughput {summary['peak_kops']:.1f} "
                  f"kops/s at {summary['peak_threads']} threads "
                  f"(p99 {summary['peak_p99_us']:.1f} µs)")
        else:
            print(f"{queue}: saturation not reached; peak observed "
                  f"{summary['peak_kops']:.1f} kops/s at "
                  f"{summary['peak_threads']} threads")

    write_queue_summary(args.csv_dir / "queue_summary.csv", summaries)

    plot(args.fig_dir, args.queues, series, summaries)
    print(f"Wrote CSV to {args.csv_dir} and figures to {args.fig_dir}")


if __name__ == "__main__":
    main()
