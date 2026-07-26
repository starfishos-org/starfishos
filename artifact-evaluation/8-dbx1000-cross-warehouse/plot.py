#!/usr/bin/env python3
"""Validate and plot the reviewer-requested DBx1000 ratio sweep."""
from __future__ import annotations

import argparse
import csv
import math
import re
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

PAGE_KB = 4
PAT_THP = re.compile(r"thp=([\d.eE+-]+)")
PAT_BIND = re.compile(r"bind (\d+) cpu:\s*([0-9 ]+)")
PAT_INIT = re.compile(
    r"TPCC init: g_thread_cnt=(\d+), g_num_wh=(\d+), "
    r"transaction_cross_warehouse_mode=(\d+), "
    r"cross_warehouse_txn_pct=(\d+), warmup=(\d+), "
    r"max_txn_per_part=(\d+), load_unused_tables=(\d+)"
)
PAT_PROC = re.compile(r"\[VMSPACE MEMORY\] Process: (\S+)")
PAT_CXL = re.compile(r"\[VMSPACE MEMORY\] CXL \(shared\): (\d+) pages")
PAT_MACHINE = re.compile(r"\[VMSPACE MEMORY\] Machine (\d+): (\d+) pages")
PAT_FATAL = re.compile(
    r"General Protection Fault|Kernel panic|panic:|BUG:|BUG_ON|"
    r"Unhandled .*exception|Unhandled .*fault|KERNEL FAULT|Trap No\.",
    re.IGNORECASE,
)


class DataError(RuntimeError):
    pass


def log_path(log_dir: Path, machine: int, machines: int, ratio: int, rep: int):
    return log_dir / f"machine{machine}_m{machines}_r{ratio}_rep{rep}.log"


def parse_log(path: Path):
    text = path.read_text(errors="replace")
    throughputs = [float(value) for value in PAT_THP.findall(text)]
    blocks = []
    current = None
    process = None
    for line in text.splitlines():
        match = PAT_PROC.search(line)
        if match:
            process = match.group(1)
            continue
        match = PAT_CXL.search(line)
        if match:
            current = {"process": process, "cxl": int(match.group(1)), "dram": {}}
            blocks.append(current)
            continue
        match = PAT_MACHINE.search(line)
        if match and current is not None:
            current["dram"][int(match.group(1))] = int(match.group(2))
    rundb = [block for block in blocks if "rundb" in (block["process"] or "")]
    return text, (throughputs[-1] if throughputs else None), (rundb[-1] if rundb else None)


def validate_point(
    log_dir: Path,
    machines: int,
    ratio: int,
    rep: int,
    warehouses: int,
    threads_per_machine: int,
    guest_cpus: int,
    warmup: int,
    max_txn: int,
    cluster_machines: int,
):
    primary_text = None
    for machine in range(machines):
        path = log_path(log_dir, machine, machines, ratio, rep)
        if not path.is_file():
            raise DataError(f"missing log: {path}")
        text = path.read_text(errors="replace")
        if not text.strip() or f"DSM] machine {machine} " not in text:
            raise DataError(f"incomplete machine log: {path}")
        fatal = PAT_FATAL.search(text)
        if fatal:
            raise DataError(f"fatal guest marker in {path}: {fatal.group(0)}")
        if machine == 0:
            primary_text = text

    primary = log_path(log_dir, 0, machines, ratio, rep)
    init = [tuple(map(int, match)) for match in PAT_INIT.findall(primary_text or "")]
    expected = (
        machines * threads_per_machine,
        warehouses,
        1,
        ratio,
        warmup,
        max_txn,
        0,
    )
    if not init or init[-1] != expected:
        raise DataError(f"wrong DBx1000 config in {primary}: expected={expected}, found={init[-1] if init else None}")
    expected_cpus = [
        machine * guest_cpus + cpu
        for machine in range(machines)
        for cpu in range(threads_per_machine)
    ]
    bindings = [
        (int(count), [int(cpu) for cpu in cpus.split()])
        for count, cpus in PAT_BIND.findall(primary_text)
    ]
    expected_binding = (machines * threads_per_machine, expected_cpus)
    if not bindings or bindings[-1] != expected_binding:
        raise DataError(
            f"wrong DBx1000 CPU binding in {primary}: "
            f"expected={expected_binding}, found={bindings[-1] if bindings else None}"
        )
    if "PASS! SimTime" not in primary_text:
        raise DataError(f"missing PASS marker: {primary}")
    done = list(re.finditer(r"^done\r?$", primary_text, re.MULTILINE))
    if not done or not re.search(
        r"^[$][ \t]*\r?$", primary_text[done[-1].end() :], re.MULTILINE
    ):
        raise DataError(f"rundb did not return to the shell: {primary}")

    _, throughput, block = parse_log(primary)
    if throughput is None or not math.isfinite(throughput) or throughput <= 0:
        raise DataError(f"invalid throughput in {primary}: {throughput}")
    if block is None or set(block["dram"]) != set(range(machines)):
        raise DataError(f"missing final rundb footprint rows: {primary}")
    cxl_mib = block["cxl"] * PAGE_KB / 1024
    dram_mib = sum(block["dram"].values()) * PAGE_KB / 1024
    if cxl_mib + dram_mib <= 0:
        raise DataError(f"zero footprint: {primary}")
    if machines == cluster_machines and (cxl_mib <= 0 or dram_mib <= 0):
        raise DataError(f"cluster point must contain both CXL and DRAM: {primary}")
    return throughput, cxl_mib, dram_mib


def mean(values):
    return statistics.fmean(values)


def std(values):
    return statistics.stdev(values) if len(values) > 1 else 0.0


def collect(args):
    rows = []
    samples = []
    warehouses_per_machine = args.num_warehouses // args.num_machines
    warmup_per_machine = args.warmup // args.num_machines
    for ratio in args.ratios:
        cluster_thp, baseline_thp, cxl, dram, totals = [], [], [], [], []
        for rep in range(1, args.repetitions + 1):
            cluster = validate_point(
                args.log_dir, args.num_machines, ratio, rep,
                args.num_warehouses, args.threads_per_machine, args.guest_cpus, args.warmup,
                args.max_txn, args.num_machines,
            )
            baseline = validate_point(
                args.log_dir, 1, ratio, rep, warehouses_per_machine,
                args.threads_per_machine, args.guest_cpus, warmup_per_machine, args.max_txn,
                args.num_machines,
            )
            cluster_thp.append(cluster[0])
            baseline_thp.append(baseline[0])
            cxl.append(cluster[1])
            dram.append(cluster[2])
            totals.append(cluster[1] + cluster[2])
            samples.append({
                "ratio": ratio, "repetition": rep,
                "cluster_thp": cluster[0], "baseline_thp": baseline[0],
                "cxl_mib": cluster[1], "dram_mib": cluster[2],
            })
        cluster_mean = mean(cluster_thp)
        baseline_mean = mean(baseline_thp)
        rows.append({
            "ratio": ratio,
            "cluster_thp": cluster_mean,
            "cluster_std": std(cluster_thp),
            "baseline_thp": baseline_mean,
            "baseline_std": std(baseline_thp),
            "scaleup": cluster_mean / baseline_mean,
            "cxl_mib": mean(cxl),
            "cxl_std": std(cxl),
            "dram_mib": mean(dram),
            "dram_std": std(dram),
            "total_std": std(totals),
        })
    return rows, samples


def write_csvs(rows, samples, csv_dir: Path):
    csv_dir.mkdir(parents=True, exist_ok=True)
    fields = [
        "ratio_pct", "cluster_thp_mtxn_s", "cluster_thp_std",
        "baseline_thp_mtxn_s", "baseline_thp_std", "scaleup",
        "cxl_mib", "cxl_std", "dram_mib", "dram_std", "total_std",
    ]
    with (csv_dir / "cross_warehouse.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(dict(zip(fields, [
                row["ratio"], row["cluster_thp"], row["cluster_std"],
                row["baseline_thp"], row["baseline_std"], row["scaleup"],
                row["cxl_mib"], row["cxl_std"], row["dram_mib"],
                row["dram_std"], row["total_std"],
            ])))

    sample_fields = ["ratio_pct", "repetition", "cluster_thp_mtxn_s", "baseline_thp_mtxn_s", "cxl_mib", "dram_mib"]
    with (csv_dir / "cross_warehouse_samples.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=sample_fields)
        writer.writeheader()
        for row in samples:
            writer.writerow(dict(zip(sample_fields, row.values())))


def plot(rows, fig_dir: Path, machines: int):
    fig_dir.mkdir(parents=True, exist_ok=True)
    x = np.arange(len(rows))
    labels = [f'{row["ratio"]}%' for row in rows]
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10, 3.8), constrained_layout=True)
    width = 0.36
    ax1.bar(x - width / 2, [r["baseline_thp"] for r in rows], width,
            yerr=[r["baseline_std"] for r in rows], capsize=3, label="1 machine")
    bars = ax1.bar(x + width / 2, [r["cluster_thp"] for r in rows], width,
                   yerr=[r["cluster_std"] for r in rows], capsize=3, label=f"{machines} machines")
    for bar, row in zip(bars, rows):
        ax1.annotate(f'{row["scaleup"]:.1f}x',
                     (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                     xytext=(0, 4), textcoords="offset points", ha="center")
    ax1.set(xticks=x, xticklabels=labels, xlabel="Cross-warehouse transaction probability", ylabel="Throughput (Mtxn/s)")
    ax1.legend(frameon=False)
    ax1.grid(axis="y", linestyle=":")

    dram = [r["dram_mib"] / 1024 for r in rows]
    cxl = [r["cxl_mib"] / 1024 for r in rows]
    ax2.bar(x, dram, 0.58, label="Local DRAM (sum)")
    ax2.bar(x, cxl, 0.58, bottom=dram,
            yerr=[r["total_std"] / 1024 for r in rows], capsize=3, label="CXL (shared)")
    ax2.set(xticks=x, xticklabels=labels, xlabel="Cross-warehouse transaction probability", ylabel="Footprint (GiB)")
    ax2.legend(frameon=False)
    ax2.grid(axis="y", linestyle=":")

    output = fig_dir / "dbx1000-cross-warehouse.png"
    fig.savefig(output, dpi=200)
    plt.close(fig)
    print(f"Wrote {output}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log-dir", required=True, type=Path)
    parser.add_argument("--csv-dir", required=True, type=Path)
    parser.add_argument("--fig-dir", required=True, type=Path)
    parser.add_argument("--num-machines", type=int, default=8)
    parser.add_argument("--num-warehouses", type=int, default=64)
    parser.add_argument("--threads-per-machine", type=int, default=8)
    parser.add_argument("--guest-cpus", type=int, default=12)
    parser.add_argument("--warmup", type=int, default=7040000)
    parser.add_argument("--max-txn", type=int, default=10000)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--ratios", nargs="+", type=int, default=[15, 50, 80])
    args = parser.parse_args()
    if args.num_warehouses % args.num_machines or args.warmup % args.num_machines:
        parser.error("warehouses and warmup must be divisible by machine count")
    try:
        rows, samples = collect(args)
    except (DataError, OSError, ValueError) as error:
        parser.error(str(error))
    write_csvs(rows, samples, args.csv_dir)
    plot(rows, args.fig_dir, args.num_machines)


if __name__ == "__main__":
    main()
