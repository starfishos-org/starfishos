#!/usr/bin/env python3
"""Parse process-migration (cfork checkpoint/restore) AE logs and plot the
paper "process-migration" breakdown figures.

Data source
-----------
The kernel emits the cfork prepare/checkpoint/restore timing breakdown via
``print_perf_cfork_time`` / ``print_perf_restore_time``
(``kernel/include/dsm/perf_timing.h``).  Each machine log then contains lines
like::

    perf_cfork_time[PREPARE]: 9321086 ns
    perf_cfork_time[PREPARE_KVS]: 1235 ns
    perf_cfork_time[CKPT]: 10410 ns
    perf_cfork_time[CKPT_STOP_ALL_THREADS]: 1201 ns
    perf_cfork_time[CKPT_THREADS]: 8831 ns
    perf_cfork_time[CKPT_CAP_GROUP]: 1477 ns
    perf_cfork_time[CKPT_OTHER]: 867 ns
    prepare copy time object: vmspace, 1595159 ns, cnt: 1
    prepare copy time object: cap_group, 3657709 ns, cnt: 1
    ...
    perf_restore_time[RESTORE]: 0 ns
    perf_restore_time[RESTORE_KVS]: 27608 ns
    perf_restore_time[RESTORE_PROMOTE_THREADS]: 45962 ns
    perf_restore_time[RESTORE_START_ALL_THREADS]: 369 ns

Inputs (in --log-dir, produced by run.sh)
  <Benchmark>.log      one QEMU log per benchmark row (Float, Linpack, ...)

Outputs (under --out-dir)
  results/process-migration.csv     paper schema (same columns as
                                    p3os-paper/eval/process-migration.csv)
  figures/process-migration-data-large.{eps,pdf,png}
  figures/process-migration-data-small.{eps,pdf,png}

Verify the drawing offline against the paper's own data::

  python3 parse_and_plot.py --csv /mnt/disk1/yjs/p3os-paper/eval/process-migration.csv \
                            --out-dir /tmp/pm-check
"""
from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Paper CSV column order (see p3os-paper/eval/process-migration.csv).
CSV_COLUMNS = [
    "Benchmark",
    "CKPT", "CKPT_CAP_GROUP", "CKPT_OTHER", "CKPT_STOP_ALL_THREADS", "CKPT_THREADS",
    "PREPARE", "PREPARE_KVS",
    "copy_cap_group", "copy_connection", "copy_irq_notification",
    "copy_notification", "copy_pmobject", "copy_thread", "copy_vmspace",
    "RESTORE", "RESTORE_KVS", "RESTORE_PROMOTE_THREADS", "RESTORE_START_ALL_THREADS",
]

# Benchmark rows in the paper figure (order matters — it is the x-axis order).
# Keys are the log basenames run.sh writes; values are the paper labels.
DEFAULT_BENCHES = ["Float", "Linpack", "Matmul", "Pyaes", "PCA", "DB1000"]

# perf_cfork_time[...] / perf_restore_time[...] keys → CSV column.
PERF_KEYS = {
    "PREPARE": "PREPARE",
    "PREPARE_KVS": "PREPARE_KVS",
    "CKPT": "CKPT",
    "CKPT_STOP_ALL_THREADS": "CKPT_STOP_ALL_THREADS",
    "CKPT_THREADS": "CKPT_THREADS",
    "CKPT_CAP_GROUP": "CKPT_CAP_GROUP",
    "CKPT_OTHER": "CKPT_OTHER",
    "RESTORE": "RESTORE",
    "RESTORE_KVS": "RESTORE_KVS",
    "RESTORE_PROMOTE_THREADS": "RESTORE_PROMOTE_THREADS",
    "RESTORE_START_ALL_THREADS": "RESTORE_START_ALL_THREADS",
}

# "prepare copy time object: <name>" → CSV column.  The kernel object names
# (obj_name_tbl) are matched case-insensitively; a couple of aliases cover
# naming differences (pmo/pmobject, irq/irq_notification).
OBJ_TO_COLUMN = {
    "vmspace": "copy_vmspace",
    "cap_group": "copy_cap_group",
    "thread": "copy_thread",
    "connection": "copy_connection",
    "pmobject": "copy_pmobject",
    "pmo": "copy_pmobject",
    "notification": "copy_notification",
    "irq_notification": "copy_irq_notification",
    "irq": "copy_irq_notification",
}

_PERF_RE = re.compile(r"perf_(?:cfork|restore)_time\[(\w+)\]:\s*(\d+)\s*ns")
_OBJ_RE = re.compile(r"prepare copy time object:\s*(\w+),\s*(\d+)\s*ns")


def parse_log(text: str) -> dict:
    """Extract one benchmark's breakdown (all values in ns) from a log."""
    row = {c: 0 for c in CSV_COLUMNS if c != "Benchmark"}
    for m in _PERF_RE.finditer(text):
        key, val = m.group(1), int(m.group(2))
        col = PERF_KEYS.get(key)
        if col is not None:
            row[col] = val
    for m in _OBJ_RE.finditer(text):
        name, val = m.group(1).lower(), int(m.group(2))
        col = OBJ_TO_COLUMN.get(name)
        if col is not None:
            row[col] = val
    return row


def collect(log_dir: Path, benches):
    rows = []
    for bench in benches:
        f = log_dir / f"{bench}.log"
        if not f.exists():
            print(f"[WARN] missing log: {f}")
            continue
        row = parse_log(f.read_text(errors="replace"))
        if not any(row.values()):
            print(f"[WARN] no perf_cfork_time lines found in {f}")
            continue
        row["Benchmark"] = bench
        rows.append(row)
    return rows


def write_csv(rows, csv_path: Path):
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(CSV_COLUMNS)
        for row in rows:
            w.writerow([row.get(c, "") for c in CSV_COLUMNS])
    print(f"Wrote {csv_path}")


# ── drawing (self-contained, mirrors p3os-paper/eval/process_migration.py) ──

def _draw_fig(fig_dir: Path, name: str):
    fig_dir.mkdir(parents=True, exist_ok=True)
    stem = fig_dir / name
    plt.savefig(stem.with_suffix(".eps"), format="eps", bbox_inches="tight")
    plt.savefig(stem.with_suffix(".pdf"), format="pdf", bbox_inches="tight")
    plt.savefig(stem.with_suffix(".png"), dpi=200, format="png", bbox_inches="tight")
    print(f"Wrote {stem}.eps/.pdf/.png")


def _preprocess(csv_path: Path):
    import pandas as pd

    data = pd.read_csv(csv_path)
    data["prepare-vmspace"] = data["copy_vmspace"]
    data["prepare-cap-group"] = data["copy_cap_group"]
    data["prepare-threads"] = data["copy_thread"]
    data["prepare-other"] = (
        data["PREPARE"] - data["copy_vmspace"] - data["copy_cap_group"]
        - data["copy_thread"] + data["PREPARE_KVS"]
    )
    data["stop-all-threads"] = data["CKPT_STOP_ALL_THREADS"]
    data["ckpt-threads"] = data["CKPT_THREADS"]
    data["ckpt-other"] = data["CKPT_OTHER"] + data["CKPT_CAP_GROUP"]
    data["restore-other"] = data["RESTORE"] + data["RESTORE_KVS"]
    data["restore-threads"] = data["RESTORE_PROMOTE_THREADS"]
    data["start-all-threads"] = data["RESTORE_START_ALL_THREADS"]
    return data


def _draw_large(data, x, width, colors, stages, fig_dir):
    plt.rcParams.update({"figure.figsize": (10, 6), "font.size": 38})
    fig, ax = plt.subplots(1, 1)

    large_stages = ["prepare-other", "prepare-cap-group", "prepare-threads", "prepare-vmspace"]
    bottom = np.zeros(len(data.index))
    original_bottom = np.zeros(len(data.index))
    for stage in large_stages:
        original_bottom += np.array(data[stage] / 1e6)
    non_last_max = float(np.max(original_bottom[:-1])) if len(original_bottom) > 1 else float(original_bottom[0])
    cutoff = non_last_max * 1.2
    last_total = float(original_bottom[-1])

    for i, stage in enumerate(large_stages):
        values = np.array(data[stage] / 1e6)
        if last_total > cutoff and i == len(large_stages) - 1:
            last_idx = len(data.index) - 1
            if last_idx < len(values):
                cur = float(bottom[last_idx])
                if cur + values[last_idx] > cutoff:
                    values[last_idx] = max(0, cutoff - cur)
        ax.bar(x, values, width, bottom=bottom,
               color=colors[stages.index(stage)], edgecolor="black", label=stage)
        bottom += values

    if last_total > cutoff:
        ax.set_ylim(0, cutoff * 1.18)
        last_idx = len(x) - 1
        bar_top = cutoff
        bw = width / 3
        ax.plot([x[last_idx] - bw * 2, x[last_idx] + bw * 2],
                [(bar_top * 0.95) - cutoff * 0.03, (bar_top * 0.95) + cutoff * 0.03], "k-", lw=3)
        ax.plot([x[last_idx] - bw * 2, x[last_idx] + bw * 2],
                [(bar_top * 0.92) - cutoff * 0.03, (bar_top * 0.92) + cutoff * 0.03], "k-", lw=3)
        ax.text(x[last_idx], bar_top + cutoff * 0.05, f"{last_total:.1f}",
                ha="center", va="bottom", fontsize=32)

    ax.set_ylabel("Time (ms)", labelpad=50)
    ticks = ax.get_yticks()
    ax.set_yticks([t for t in ticks if t <= 20])
    ax.set_xticks(x)
    ax.set_xticklabels(data.iloc[:, 0], rotation=30, ha="right")
    ax.grid(True, which="both", axis="y", linestyle=":")
    ax.legend(bbox_to_anchor=(1.02, 1), loc="upper left", frameon=False, fontsize=39)
    _draw_fig(fig_dir, "process-migration-data-large")
    plt.cla()
    plt.close(fig)


def _draw_small(data, x, width, colors, stages, fig_dir):
    plt.rcParams.update({"figure.figsize": (8, 3), "font.size": 17})
    fig3 = plt.figure()
    ax3 = fig3.add_subplot(111)
    small_stages = ["stop-all-threads", "ckpt-threads", "ckpt-other",
                    "restore-threads", "restore-other", "start-all-threads"]
    bottom2 = np.zeros(len(data.index))
    for stage in small_stages:
        ax3.bar(x, data[stage] / 1000.0, width, bottom=bottom2,
                color=colors[stages.index(stage)], edgecolor="black", label=stage)
        bottom2 += data[stage] / 1000.0
    ax3.set_ylabel("Time (us)")
    ax3.set_xticks(x)
    ax3.set_xticklabels(data.iloc[:, 0], rotation=30, ha="right")
    ax3.grid(True, which="both", axis="y", linestyle=":")
    h, l = ax3.get_legend_handles_labels()
    ax3.legend(handles=h[::-1], labels=l[::-1], frameon=False, loc="upper left",
               bbox_to_anchor=(1.02, 1), fontsize=18.5)
    plt.tight_layout()
    _draw_fig(fig_dir, "process-migration-data-small")
    plt.close(fig3)


def draw(csv_path: Path, fig_dir: Path):
    data = _preprocess(csv_path)
    x = np.arange(len(data.index))
    width = 0.5
    blue = [plt.cm.Blues(i) for i in [0.2, 0.4, 0.6, 0.8]]
    red = [plt.cm.Reds(i) for i in [0.2, 0.5, 0.8]]
    purple = [plt.cm.Purples(i) for i in [0.2, 0.5, 0.8]]
    colors = blue + red + purple
    stages = ["prepare-vmspace", "prepare-cap-group", "prepare-threads", "prepare-other",
              "stop-all-threads", "ckpt-threads", "ckpt-other",
              "restore-threads", "restore-other", "start-all-threads"]
    _draw_large(data, x, width, colors, stages, fig_dir)
    _draw_small(data, x, width, colors, stages, fig_dir)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log-dir", type=Path,
                    help="Directory of <Benchmark>.log files from run.sh")
    ap.add_argument("--out-dir", required=True, type=Path)
    ap.add_argument("--csv", type=Path,
                    help="Draw directly from an existing process-migration.csv "
                         "(skip log parsing; used to verify against paper data)")
    ap.add_argument("--benches", nargs="*", default=DEFAULT_BENCHES)
    args = ap.parse_args()

    fig_dir = args.out_dir / "figures"
    if args.csv:
        draw(args.csv, fig_dir)
        return

    if not args.log_dir:
        ap.error("either --csv or --log-dir is required")
    rows = collect(args.log_dir, args.benches)
    if not rows:
        raise SystemExit("No benchmark produced cfork timing; nothing to plot")
    csv_path = args.out_dir / "results" / "process-migration.csv"
    write_csv(rows, csv_path)
    draw(csv_path, fig_dir)


if __name__ == "__main__":
    main()
