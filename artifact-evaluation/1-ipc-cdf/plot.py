#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import re
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

MODE_ORDER = ["cross_empty", "cross", "cross_empty_4t", "cross_4t"]
SCRIPT_DIR = Path(__file__).resolve().parent


def cpu_freq_hz(log: Path) -> float:
    pat = re.compile(r"cpu frequency=\(Dec\)(\d+)")
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pat.search(line)
        if match:
            return float(match.group(1))
    raise RuntimeError(f"missing CPU frequency in {log}")


def parse_summary(log: Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    current: dict[str, object] | None = None
    begin = re.compile(r"\[SUMMARY\]\s+mode=(\S+)\s+total=(\d+)\s+threads=(\d+)")
    values = re.compile(
        r"\[SUMMARY\]\s+p50=(\d+)\s+p75=(\d+)\s+p90=(\d+)\s+p99=(\d+)\s+max=(\d+)\s+\((\S+)\)"
    )
    freq = cpu_freq_hz(log)
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = begin.search(line)
        if match:
            current = {
                "machine": log.stem,
                "mode": match.group(1),
                "total": int(match.group(2)),
                "threads": int(match.group(3)),
            }
            continue
        match = values.search(line)
        if match and current is not None:
            unit = match.group(6)
            factor = 1.0 / 1000.0 if unit == "ns" else 1e6 / freq
            current.update(
                {
                    "p50_us": int(match.group(1)) * factor,
                    "p75_us": int(match.group(2)) * factor,
                    "p90_us": int(match.group(3)) * factor,
                    "p99_us": int(match.group(4)) * factor,
                    "max_us": int(match.group(5)) * factor,
                    "source_unit": unit,
                }
            )
            rows.append(current)
            current = None
    return rows


def parse_cdf(log: Path) -> dict[str, list[tuple[int, float]]]:
    freq = cpu_freq_hz(log)
    factor = 1e6 / freq
    out: dict[str, list[tuple[int, float]]] = defaultdict(list)
    mode: str | None = None
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"\[CDF_BEGIN\]\s+mode=(\S+)\s+count=(\d+)", line)
        if match:
            mode = match.group(1)
            continue
        if "[CDF_END]" in line:
            mode = None
            continue
        if mode is None:
            continue
        match = re.match(r"\[CDF\]\s+(\d+)\s+(\d+)", line)
        if match:
            out[mode].append((int(match.group(1)), int(match.group(2)) * factor))
    return out


def parse_breakdown(log: Path) -> dict[str, list[tuple[int, int, int, int]]]:
    out: dict[str, list[tuple[int, int, int, int]]] = defaultdict(list)
    mode: str | None = None
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"\[BREAKDOWN_BEGIN\]\s+mode=(\S+)", line)
        if match:
            mode = match.group(1)
            continue
        if "[BREAKDOWN_END]" in line:
            mode = None
            continue
        if mode is None:
            continue
        match = re.match(r"\[BD\]\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)", line)
        if match:
            out[mode].append(tuple(int(x) for x in match.groups()))
    return out


def parse_server_timing(log: Path) -> dict[str, dict[str, list[int]]]:
    out: dict[str, dict[str, list[int]]] = {}
    block = -1
    active = False
    deq: list[int] = []
    handle: list[int] = []
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        if "[SRV_TIMING_BEGIN]" in line:
            block += 1
            active = True
            deq = []
            handle = []
            continue
        if "[SRV_TIMING_END]" in line:
            active = False
            if block < len(MODE_ORDER):
                out[MODE_ORDER[block]] = {"dequeue": deq, "handle": handle}
            continue
        if not active:
            continue
        match = re.match(r"\[ST\]\s+(\d+)\s+(\d+)", line.strip())
        if match:
            deq.append(int(match.group(1)))
            handle.append(int(match.group(2)))
    return out


def median_us(values: list[int], factor: float) -> float:
    return statistics.median(values) * factor if values else 0.0


def breakdown_rows(log: Path) -> list[dict[str, object]]:
    factor = 1e6 / cpu_freq_hz(log)
    out = []
    for mode, rows in parse_breakdown(log).items():
        if not rows:
            continue
        total, alloc, enqueue, wait = zip(*rows)
        out.append(
            {
                "machine": log.stem,
                "mode": mode,
                "samples": len(rows),
                "total_us": statistics.median(total) * factor,
                "alloc_us": statistics.median(alloc) * factor,
                "enqueue_us": statistics.median(enqueue) * factor,
                "wait_us": statistics.median(wait) * factor,
            }
        )
    return out


def write_csv(path: Path, rows: list[dict[str, object]], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def export_cdf(path: Path, machine0: dict[str, list[tuple[int, float]]], machine1: dict[str, list[tuple[int, float]]]) -> None:
    rows = []
    for machine, data in [("machine0", machine0), ("machine1", machine1)]:
        for mode, samples in data.items():
            samples = sorted(samples, key=lambda item: item[0])
            n = len(samples)
            for pos, (sample_id, latency_us) in enumerate(samples, start=1):
                rows.append(
                    {
                        "machine": machine,
                        "mode": mode,
                        "sample_id": sample_id,
                        "latency_us": latency_us,
                        "cdf": pos / n if n else 0.0,
                    }
                )
    write_csv(path, rows, ["machine", "mode", "sample_id", "latency_us", "cdf"])


def cdf_xy(data: dict[str, list[tuple[int, float]]], mode: str) -> tuple[list[float], list[float]]:
    samples = sorted(data.get(mode, []), key=lambda item: item[0])
    n = len(samples)
    return [lat for _idx, lat in samples], [(i + 1) / n for i in range(n)]


def percentile(data: dict[str, list[tuple[int, float]]], mode: str, q: float) -> float:
    xs, _ys = cdf_xy(data, mode)
    if not xs:
        return 0.0
    index = min(len(xs) - 1, max(0, int(round(q * (len(xs) - 1)))))
    return xs[index]


def plot_cdf(fig_dir: Path, machine0: dict[str, list[tuple[int, float]]], machine1: dict[str, list[tuple[int, float]]]) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(8.4, 3.2), sharey=True)
    panels = [
        (
            axes[0],
            "Empty IPC",
            [("Local", machine0, "direct_empty"), ("Remote", machine1, "cross_empty"), ("Remote-Conc.", machine1, "cross_empty_4t")],
        ),
        (
            axes[1],
            "Read 4KiB",
            [("Local", machine0, "direct"), ("Remote", machine1, "cross"), ("Remote-Conc.", machine1, "cross_4t")],
        ),
    ]
    colors = {"Local": "#2ca02c", "Remote": "#1f77b4", "Remote-Conc.": "#d62728"}
    styles = {"Local": "--", "Remote": "-", "Remote-Conc.": "-"}
    for ax, title, series in panels:
        xmax = 0.0
        for label, source, mode in series:
            xs, ys = cdf_xy(source, mode)
            if not xs:
                continue
            ax.plot(xs, ys, label=label, color=colors[label], linestyle=styles[label], linewidth=1.8)
            xmax = max(xmax, percentile(source, mode, 0.99))
        ax.set_title(title)
        ax.set_xlabel("Latency (us)")
        ax.set_ylim(0, 1)
        if xmax > 0:
            ax.set_xlim(0, xmax * 1.05)
        ax.grid(True, linestyle=":", alpha=0.7)
    axes[0].set_ylabel("CDF")
    axes[1].legend(loc="lower right", frameon=False)
    fig.tight_layout()
    fig.savefig(fig_dir / "ipc_cdf.png", dpi=300, bbox_inches="tight")
    plt.close(fig)


def row_by_mode(rows: list[dict[str, object]], mode: str) -> dict[str, object]:
    for row in rows:
        if row["mode"] == mode:
            return row
    return {"total_us": 0.0, "alloc_us": 0.0, "enqueue_us": 0.0, "wait_us": 0.0}


def plot_breakdown(fig_dir: Path, machine0_rows: list[dict[str, object]], machine1_rows: list[dict[str, object]], srv_rows: dict[str, dict[str, float]]) -> None:
    direct = row_by_mode(machine0_rows, "direct")
    cross = row_by_mode(machine1_rows, "cross")
    cross4 = row_by_mode(machine1_rows, "cross_4t")

    def remote_stack(row: dict[str, object], mode: str) -> dict[str, float]:
        enqueue = float(row["alloc_us"]) + float(row["enqueue_us"])
        dequeue = srv_rows.get(mode, {}).get("dequeue_us", 0.0)
        handle = srv_rows.get(mode, {}).get("handle_us", 0.0)
        total = float(row["total_us"])
        if mode == "cross":
            queue = 0.0
            handle = max(0.0, total - enqueue - dequeue)
        else:
            remain = max(0.0, total - enqueue - dequeue)
            if handle <= 0:
                handle = min(2.0, remain)
            queue = max(0.0, remain - handle)
        return {"Enq": enqueue, "Deq": dequeue, "Execute": handle, "Queue": queue, "total": total}

    series = [
        ("Local", {"total": float(direct["total_us"]), "white": True}),
        ("Remote", remote_stack(cross, "cross")),
        ("Remote-Conc.", remote_stack(cross4, "cross_4t")),
    ]
    comps = ["Enq", "Deq", "Execute", "Queue"]
    colors = {"Enq": "#42A5F5", "Deq": "#AB47BC", "Execute": "#66BB6A", "Queue": "#FFA726"}

    fig, ax = plt.subplots(figsize=(4.5, 3.4))
    bottoms = [0.0] * len(series)
    for i, (_label, data) in enumerate(series):
        if data.get("white"):
            ax.bar(i, data["total"], color="white", edgecolor="#333333", linewidth=0.9)
            bottoms[i] = data["total"]
    for comp in comps:
        vals = [0.0 if data.get("white") else data[comp] for _label, data in series]
        ax.bar(range(len(series)), vals, bottom=bottoms, label=comp, color=colors[comp], edgecolor="#333333", linewidth=0.8)
        bottoms = [bottoms[i] + vals[i] for i in range(len(vals))]
    for i, (_label, data) in enumerate(series):
        ax.text(i, bottoms[i] + 0.3, f"{data['total']:.1f}", ha="center", va="bottom", fontsize=10)
    ax.set_xticks(range(len(series)))
    ax.set_xticklabels([label for label, _data in series])
    ax.set_ylabel("P50 Lat. (us)")
    ax.grid(True, axis="y", linestyle=":", alpha=0.6)
    ax.legend(ncol=2, frameon=False, loc="upper left")
    fig.tight_layout()
    fig.savefig(fig_dir / "ipc_read_breakdown.png", dpi=300, bbox_inches="tight")
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--log-dir",
        type=Path,
        default=SCRIPT_DIR / "logs",
        help="IPC log directory (default: %(default)s)",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=SCRIPT_DIR,
        help="output directory (default: %(default)s)",
    )
    args = parser.parse_args()

    machine0_log = args.log_dir / "machine0.log"
    machine1_log = args.log_dir / "machine1.log"
    if not machine0_log.is_file() or not machine1_log.is_file():
        raise FileNotFoundError("expected machine0.log and machine1.log under --log-dir")

    args.out_dir.mkdir(parents=True, exist_ok=True)

    summary_rows = parse_summary(machine0_log) + parse_summary(machine1_log)
    write_csv(
        args.out_dir / "summary.csv",
        summary_rows,
        ["machine", "mode", "total", "threads", "p50_us", "p75_us", "p90_us", "p99_us", "max_us", "source_unit"],
    )

    cdf0 = parse_cdf(machine0_log)
    cdf1 = parse_cdf(machine1_log)
    export_cdf(args.out_dir / "cdf.csv", cdf0, cdf1)

    bd0 = breakdown_rows(machine0_log)
    bd1 = breakdown_rows(machine1_log)
    write_csv(
        args.out_dir / "breakdown.csv",
        bd0 + bd1,
        ["machine", "mode", "samples", "total_us", "alloc_us", "enqueue_us", "wait_us"],
    )

    srv_factor = 1e6 / cpu_freq_hz(machine0_log)
    srv = parse_server_timing(machine0_log)
    srv_flat = []
    srv_for_plot: dict[str, dict[str, float]] = {}
    for mode, values in srv.items():
        row = {
            "machine": "machine0",
            "mode": mode,
            "samples": len(values["dequeue"]),
            "dequeue_us": median_us(values["dequeue"], srv_factor),
            "handle_us": median_us(values["handle"], srv_factor),
        }
        srv_flat.append(row)
        srv_for_plot[mode] = {"dequeue_us": row["dequeue_us"], "handle_us": row["handle_us"]}
    write_csv(args.out_dir / "server_timing.csv", srv_flat, ["machine", "mode", "samples", "dequeue_us", "handle_us"])

    plot_cdf(args.out_dir, cdf0, cdf1)
    plot_breakdown(args.out_dir, bd0, bd1, srv_for_plot)
    print(f"Wrote results to {args.out_dir}")


if __name__ == "__main__":
    main()
