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

BYTES_PER_MIB = 1024 * 1024
BYTES_PER_GB = 1000 * 1000 * 1000
PAGE_BYTES = 4096
PAT_THP = re.compile(r"thp=([\d.eE+-]+)")
# DBx1000 prints the aggregate as "%.2f", which quantizes the one-machine
# baseline (~0.18) to two significant digits and reports a zero standard
# deviation for any three boots that agree. Recompute from the per-thread
# counters, which carry enough digits, and keep the aggregate as a fallback.
PAT_THREAD = re.compile(
    r"\[tid=\d+\] txn_cnt=(\d+),abort_cnt=\d+ run_time=([\d.eE+-]+)"
)
PAT_BIND = re.compile(r"bind (\d+) cpu:\s*([0-9 ]+)")
PAT_INIT = re.compile(
    r"TPCC init: g_thread_cnt=(\d+), g_num_wh=(\d+), "
    r"transaction_cross_warehouse_mode=(\d+), "
    r"cross_warehouse_txn_pct=(\d+), warmup=(\d+), "
    r"max_txn_per_part=(\d+), load_unused_tables=(\d+), "
    r"measure_duration_sec=(\d+)"
)
PAT_CXLPROF_BYTES = re.compile(
    r"\[cxlprof\] exec: all machines bytes "
    r"cxl=(\d+) dram=(\d+) total=(\d+)"
)
PAT_VMSPACE_CXL = re.compile(
    r"\[INFO\] \[VMSPACE MEMORY\] CXL \(shared\): (\d+) pages"
)
PAT_VMSPACE_MACHINE = re.compile(
    r"\[INFO\] \[VMSPACE MEMORY\] Machine (\d+): (\d+) pages"
)
PAT_FATAL = re.compile(
    r"General Protection Fault|Kernel panic|panic:|BUG:|BUG_ON|"
    r"Unhandled .*exception|Unhandled .*fault|KERNEL FAULT|Trap No\.",
    re.IGNORECASE,
)


class DataError(RuntimeError):
    pass


def log_path(log_dir: Path, machine: int, machines: int, ratio: int, rep: int):
    return log_dir / f"machine{machine}_m{machines}_r{ratio}_rep{rep}.log"


def parse_log(path: Path, expected_threads: int | None = None):
    text = path.read_text(errors="replace")
    threads = [(int(txn), float(window)) for txn, window in PAT_THREAD.findall(text)]
    # Keep only the final Stats::print() block, the same way every other
    # parser here takes [-1]: a log that somehow holds two runs (a retry, an
    # appended tmux capture) would otherwise have its throughput summed across
    # both.  Without an expected count, fall back to the whole text.
    if expected_threads and len(threads) >= expected_threads:
        threads = threads[-expected_threads:]
    if expected_threads and len(threads) != expected_threads:
        raise DataError(
            f"expected {expected_threads} per-thread stat lines in {path}, "
            f"found {len(threads)} (truncated log?)"
        )
    if threads:
        # Same formula as DBx1000's own aggregate (system/stats.cpp): total
        # committed txns over the mean per-worker run_time.
        mean_window = statistics.mean(window for _, window in threads)
        throughput = (
            sum(txn for txn, _ in threads) / mean_window / 1e6
            if mean_window > 0
            else None
        )
    else:
        aggregate = PAT_THP.findall(text)
        throughput = float(aggregate[-1]) if aggregate else None
    access_bytes = [tuple(map(int, match)) for match in PAT_CXLPROF_BYTES.findall(text)]
    return text, throughput, (access_bytes[-1] if access_bytes else None)


def parse_vmspace_after(text: str, anchor: str, expected_machines: int):
    anchor_pos = text.rfind(anchor)
    if anchor_pos < 0:
        return None
    segment = text[anchor_pos:]
    process_pos = segment.find("[INFO] [VMSPACE MEMORY] Process: /rundb.bin")
    if process_pos < 0:
        return None
    segment = segment[process_pos:]
    cxl_match = PAT_VMSPACE_CXL.search(segment)
    if not cxl_match:
        return None
    machines = [
        (int(machine), int(pages))
        for machine, pages in PAT_VMSPACE_MACHINE.findall(segment[cxl_match.end():])
    ][:expected_machines]
    if [machine for machine, _ in machines] != list(range(expected_machines)):
        return None
    return int(cxl_match.group(1)), sum(pages for _, pages in machines)


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
    measure_sec: int,
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
        measure_sec,
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

    _, throughput, access_bytes = parse_log(primary, machines * threads_per_machine)
    if throughput is None or not math.isfinite(throughput) or throughput <= 0:
        raise DataError(f"invalid throughput in {primary}: {throughput}")
    if access_bytes is None:
        raise DataError(f"missing aggregate cxlprof byte counters: {primary}")
    cxl_bytes, dram_bytes, total_bytes = access_bytes
    if cxl_bytes + dram_bytes != total_bytes:
        raise DataError(
            f"inconsistent cxlprof byte counters in {primary}: {access_bytes}"
        )
    if total_bytes <= 0:
        raise DataError(f"zero cxlprof access volume: {primary}")
    post_warmup = parse_vmspace_after(
        primary_text or "",
        "[cxlprof] post-warmup: counters cleared; measurement enabled",
        machines,
    )
    post_exec = parse_vmspace_after(
        primary_text or "",
        "[Main] Steady-state vmspace stats (post-execution):",
        machines,
    )
    if post_warmup is None:
        raise DataError(f"missing post-warmup vmspace snapshot: {primary}")
    if post_exec is None:
        raise DataError(f"missing post-exec vmspace snapshot: {primary}")
    pages_to_mib = PAGE_BYTES / BYTES_PER_MIB
    return (
        throughput,
        cxl_bytes / BYTES_PER_MIB,
        dram_bytes / BYTES_PER_MIB,
        post_warmup[0] * pages_to_mib,
        post_warmup[1] * pages_to_mib,
        post_exec[0] * pages_to_mib,
        post_exec[1] * pages_to_mib,
    )


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
        baseline_cxl, baseline_dram, baseline_totals = [], [], []
        warmup_cxl_resident, warmup_dram_resident = [], []
        exec_cxl_resident, exec_dram_resident = [], []
        baseline_warmup_cxl_resident, baseline_warmup_dram_resident = [], []
        baseline_exec_cxl_resident, baseline_exec_dram_resident = [], []
        for rep in range(1, args.repetitions + 1):
            cluster = validate_point(
                args.log_dir, args.num_machines, ratio, rep,
                args.num_warehouses, args.threads_per_machine, args.guest_cpus, args.warmup,
                args.max_txn, args.measure_sec,
            )
            baseline = validate_point(
                args.log_dir, 1, ratio, rep, warehouses_per_machine,
                args.threads_per_machine, args.guest_cpus, warmup_per_machine, args.max_txn,
                args.measure_sec,
            )
            cluster_thp.append(cluster[0])
            baseline_thp.append(baseline[0])
            cxl.append(cluster[1])
            dram.append(cluster[2])
            totals.append(cluster[1] + cluster[2])
            baseline_cxl.append(baseline[1])
            baseline_dram.append(baseline[2])
            baseline_totals.append(baseline[1] + baseline[2])
            warmup_cxl_resident.append(cluster[3])
            warmup_dram_resident.append(cluster[4])
            exec_cxl_resident.append(cluster[5])
            exec_dram_resident.append(cluster[6])
            baseline_warmup_cxl_resident.append(baseline[3])
            baseline_warmup_dram_resident.append(baseline[4])
            baseline_exec_cxl_resident.append(baseline[5])
            baseline_exec_dram_resident.append(baseline[6])
            samples.append({
                "ratio": ratio, "repetition": rep,
                "cluster_thp": cluster[0], "baseline_thp": baseline[0],
                "cxl_access_mib": cluster[1], "dram_access_mib": cluster[2],
                "baseline_cxl_access_mib": baseline[1],
                "baseline_dram_access_mib": baseline[2],
                "post_warmup_cxl_resident_mib": cluster[3],
                "post_warmup_dram_resident_mib": cluster[4],
                "post_exec_cxl_resident_mib": cluster[5],
                "post_exec_dram_resident_mib": cluster[6],
                "baseline_post_warmup_cxl_resident_mib": baseline[3],
                "baseline_post_warmup_dram_resident_mib": baseline[4],
                "baseline_post_exec_cxl_resident_mib": baseline[5],
                "baseline_post_exec_dram_resident_mib": baseline[6],
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
            "cxl_access_mib": mean(cxl),
            "cxl_access_std": std(cxl),
            "dram_access_mib": mean(dram),
            "dram_access_std": std(dram),
            "total_access_std": std(totals),
            "baseline_cxl_access_mib": mean(baseline_cxl),
            "baseline_cxl_access_std": std(baseline_cxl),
            "baseline_dram_access_mib": mean(baseline_dram),
            "baseline_dram_access_std": std(baseline_dram),
            "baseline_total_access_std": std(baseline_totals),
            "post_warmup_cxl_resident_mib": mean(warmup_cxl_resident),
            "post_warmup_cxl_resident_std": std(warmup_cxl_resident),
            "post_warmup_dram_resident_mib": mean(warmup_dram_resident),
            "post_warmup_dram_resident_std": std(warmup_dram_resident),
            "post_exec_cxl_resident_mib": mean(exec_cxl_resident),
            "post_exec_cxl_resident_std": std(exec_cxl_resident),
            "post_exec_dram_resident_mib": mean(exec_dram_resident),
            "post_exec_dram_resident_std": std(exec_dram_resident),
            "baseline_post_warmup_cxl_resident_mib": mean(
                baseline_warmup_cxl_resident
            ),
            "baseline_post_warmup_cxl_resident_std": std(
                baseline_warmup_cxl_resident
            ),
            "baseline_post_warmup_dram_resident_mib": mean(
                baseline_warmup_dram_resident
            ),
            "baseline_post_warmup_dram_resident_std": std(
                baseline_warmup_dram_resident
            ),
            "baseline_post_exec_cxl_resident_mib": mean(
                baseline_exec_cxl_resident
            ),
            "baseline_post_exec_cxl_resident_std": std(
                baseline_exec_cxl_resident
            ),
            "baseline_post_exec_dram_resident_mib": mean(
                baseline_exec_dram_resident
            ),
            "baseline_post_exec_dram_resident_std": std(
                baseline_exec_dram_resident
            ),
        })
    return rows, samples


def write_csvs(rows, samples, csv_dir: Path):
    csv_dir.mkdir(parents=True, exist_ok=True)
    fields = [
        "ratio_pct", "cluster_thp_mtxn_s", "cluster_thp_std",
        "baseline_thp_mtxn_s", "baseline_thp_std", "scaleup",
        "cxl_access_mib", "cxl_access_std", "dram_access_mib",
        "dram_access_std", "total_access_std",
        "baseline_cxl_access_mib", "baseline_cxl_access_std",
        "baseline_dram_access_mib", "baseline_dram_access_std",
        "baseline_total_access_std",
        "post_warmup_cxl_resident_mib", "post_warmup_cxl_resident_std",
        "post_warmup_dram_resident_mib", "post_warmup_dram_resident_std",
        "post_exec_cxl_resident_mib", "post_exec_cxl_resident_std",
        "post_exec_dram_resident_mib", "post_exec_dram_resident_std",
        "baseline_post_warmup_cxl_resident_mib",
        "baseline_post_warmup_cxl_resident_std",
        "baseline_post_warmup_dram_resident_mib",
        "baseline_post_warmup_dram_resident_std",
        "baseline_post_exec_cxl_resident_mib",
        "baseline_post_exec_cxl_resident_std",
        "baseline_post_exec_dram_resident_mib",
        "baseline_post_exec_dram_resident_std",
    ]
    with (csv_dir / "cross_warehouse.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(dict(zip(fields, [
                row["ratio"], row["cluster_thp"], row["cluster_std"],
                row["baseline_thp"], row["baseline_std"], row["scaleup"],
                row["cxl_access_mib"], row["cxl_access_std"],
                row["dram_access_mib"], row["dram_access_std"],
                row["total_access_std"],
                row["baseline_cxl_access_mib"],
                row["baseline_cxl_access_std"],
                row["baseline_dram_access_mib"],
                row["baseline_dram_access_std"],
                row["baseline_total_access_std"],
                row["post_warmup_cxl_resident_mib"],
                row["post_warmup_cxl_resident_std"],
                row["post_warmup_dram_resident_mib"],
                row["post_warmup_dram_resident_std"],
                row["post_exec_cxl_resident_mib"],
                row["post_exec_cxl_resident_std"],
                row["post_exec_dram_resident_mib"],
                row["post_exec_dram_resident_std"],
                row["baseline_post_warmup_cxl_resident_mib"],
                row["baseline_post_warmup_cxl_resident_std"],
                row["baseline_post_warmup_dram_resident_mib"],
                row["baseline_post_warmup_dram_resident_std"],
                row["baseline_post_exec_cxl_resident_mib"],
                row["baseline_post_exec_cxl_resident_std"],
                row["baseline_post_exec_dram_resident_mib"],
                row["baseline_post_exec_dram_resident_std"],
            ])))

    sample_fields = [
        "ratio_pct", "repetition", "cluster_thp_mtxn_s",
        "baseline_thp_mtxn_s", "cxl_access_mib", "dram_access_mib",
        "baseline_cxl_access_mib", "baseline_dram_access_mib",
        "post_warmup_cxl_resident_mib", "post_warmup_dram_resident_mib",
        "post_exec_cxl_resident_mib", "post_exec_dram_resident_mib",
        "baseline_post_warmup_cxl_resident_mib",
        "baseline_post_warmup_dram_resident_mib",
        "baseline_post_exec_cxl_resident_mib",
        "baseline_post_exec_dram_resident_mib",
    ]
    with (csv_dir / "cross_warehouse_samples.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=sample_fields)
        writer.writeheader()
        for row in samples:
            writer.writerow(dict(zip(sample_fields, row.values())))


def plot(rows, fig_dir: Path, machines: int):
    fig_dir.mkdir(parents=True, exist_ok=True)
    x = np.arange(len(rows))
    labels = [f'{row["ratio"]}%' for row in rows]
    fig, (ax1, ax2, ax3) = plt.subplots(
        1, 3, figsize=(15, 3.8), constrained_layout=True
    )
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

    mib_to_gb = BYTES_PER_MIB / BYTES_PER_GB
    dram = [r["dram_access_mib"] * mib_to_gb for r in rows]
    cxl = [r["cxl_access_mib"] * mib_to_gb for r in rows]
    ax2.bar(x - width / 2, dram, width,
            yerr=[r["dram_access_std"] * mib_to_gb for r in rows], capsize=3,
            label="Local DRAM accesses")
    ax2.bar(x + width / 2, cxl, width,
            yerr=[r["cxl_access_std"] * mib_to_gb for r in rows], capsize=3,
            label="Shared CXL accesses")
    ax2.set_yscale("log")
    ax2.set(xticks=x, xticklabels=labels,
            xlabel="Cross-warehouse transaction probability",
            ylabel="Access volume (GB, log scale)")
    ax2.legend(frameon=False)
    ax2.grid(axis="y", linestyle=":")

    resident_dram = [r["post_exec_dram_resident_mib"] * mib_to_gb for r in rows]
    resident_cxl = [r["post_exec_cxl_resident_mib"] * mib_to_gb for r in rows]
    ax3.bar(
        x, resident_dram, width * 1.5,
        yerr=[r["post_exec_dram_resident_std"] * mib_to_gb for r in rows],
        capsize=3, label="Local DRAM resident",
    )
    ax3.bar(
        x, resident_cxl, width * 1.5, bottom=resident_dram,
        yerr=[r["post_exec_cxl_resident_std"] * mib_to_gb for r in rows],
        capsize=3, label="Shared CXL resident",
    )
    ax3.set(
        xticks=x, xticklabels=labels,
        xlabel="Cross-warehouse transaction probability",
        ylabel="Post-exec resident footprint (GB)",
    )
    ax3.set_ylim(0, max(dram + cxl for dram, cxl in zip(resident_dram, resident_cxl)) * 1.18)
    ax3.legend(frameon=False)
    ax3.grid(axis="y", linestyle=":")

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
    parser.add_argument("--warmup", type=int, default=512000)
    parser.add_argument("--max-txn", type=int, default=10000)
    parser.add_argument("--measure-sec", type=int, default=0)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument(
        "--ratios", nargs="+", type=int, default=[0, 5, 10, 15, 50, 80, 100]
    )
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
