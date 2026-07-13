#!/usr/bin/env python3
"""Parse allocator-throughput AE logs and plot paper Figure 12.

Inputs (in --log-dir, produced by run.sh):
  <config>_run<r>_kernel.log        kernel boot-time malloc tests
  <config>_run<r>_user_t<T>.log     userspace rpmalloc benchmark

Outputs (under --out-dir):
  results/allocator.csv     per-(config,memory,test,parallel) mean ops/s
  results/user-malloc.csv   per-run userspace rpmalloc rows
  figures/fig12-allocator-all.pdf/.eps   combined 3-panel figure:
      (a) Slab (kmalloc), (b) Buddy (mixed random 4K/2M), (c) rpmalloc
"""
from __future__ import annotations

import argparse
import csv
import re
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# log config label -> paper config label
CONFIG_LABEL = {
    "buddy": "Buddy",
    "llfree": "LLFree",
    "llfree_cr": "LLFree+CR",
}

PAT_KMALLOC = re.compile(
    r"\[TEST\]\s+(?P<mem>DRAM|CXL)\s+kmalloc avg throughput"
    r"\s+\(parallel=(?P<par>\d+)\):\s+(?P<ops>\d+)\s+ops/s"
)
PAT_GP = re.compile(
    r"\[TEST\]\s+(?P<phase>get_pages|free_pages) throughput"
    r"\s+\((?P<mem>DRAM|CXL),\s*(?P<bytes>\d+)B\):\s+(?P<ops>\d+)\s+ops/s"
)
PAT_RANDOM = re.compile(
    r"\[TEST\]\s+(?P<mem>DRAM|CXL)\s+random get_pages/free_pages 4KiB\+2MiB"
    r"\s+iters=(?P<iters>\d+)\s+throughput=(?P<tp>\d+)\s+loop-iters/s"
)
PAT_PAR_CTX = re.compile(r"\[TEST\]\s+start malloc test parallel=(?P<par>\d+)")
PAT_USER_TP = re.compile(r"throughput\(ops/s\):\s*(?P<tp>\d+)")

TEST_KMALLOC = "kmalloc"
TEST_MIXED = "random_get_free_4K2M"


def sz_label(b: int) -> str:
    if b == 4096:
        return "4KB"
    if b >= 2 * 1024 * 1024 - 1:
        return "2MB"
    return f"{b}B"


def parse_kernel_log(path: Path):
    """Yield (memory, test, parallel, ops_per_sec) tuples."""
    cur_par = 1
    for line in path.read_text(errors="replace").splitlines():
        m = PAT_PAR_CTX.search(line)
        if m:
            cur_par = int(m.group("par"))
            continue
        m = PAT_KMALLOC.search(line)
        if m:
            yield (m.group("mem"), TEST_KMALLOC, int(m.group("par")), float(m.group("ops")))
            continue
        m = PAT_GP.search(line)
        if m:
            phase = "alloc" if m.group("phase") == "get_pages" else "free"
            test = f"get_pages({sz_label(int(m.group('bytes')))})-{phase}"
            yield (m.group("mem"), test, cur_par, float(m.group("ops")))
            continue
        m = PAT_RANDOM.search(line)
        if m:
            yield (m.group("mem"), TEST_MIXED, cur_par, float(m.group("tp")))


def parse_user_log(path: Path):
    """Return the last throughput(ops/s) value in the log, or None."""
    tp = None
    for line in path.read_text(errors="replace").splitlines():
        m = PAT_USER_TP.search(line)
        if m:
            tp = float(m.group("tp"))
    return tp


def collect(log_dir: Path):
    kern_pat = re.compile(r"(?P<cfg>\w+)_run(?P<run>\d+)_kernel\.log$")
    user_pat = re.compile(r"(?P<cfg>\w+)_run(?P<run>\d+)_user_t(?P<t>\d+)\.log$")

    # (config, memory, test, parallel) -> [ops...]
    kernel: dict[tuple, list] = defaultdict(list)
    # (config, threads) -> [(run, ops)...]
    user: dict[tuple, list] = defaultdict(list)

    for f in sorted(log_dir.iterdir()):
        m = kern_pat.match(f.name)
        if m and m.group("cfg") in CONFIG_LABEL:
            cfg = CONFIG_LABEL[m.group("cfg")]
            for mem, test, par, ops in parse_kernel_log(f):
                kernel[(cfg, mem, test, par)].append(ops)
            continue
        m = user_pat.match(f.name)
        if m and m.group("cfg") in CONFIG_LABEL:
            cfg = CONFIG_LABEL[m.group("cfg")]
            tp = parse_user_log(f)
            if tp is not None:
                user[(cfg, int(m.group("t")))].append((int(m.group("run")), tp))
    return kernel, user


def write_csvs(kernel, user, results_dir: Path):
    results_dir.mkdir(parents=True, exist_ok=True)

    with (results_dir / "allocator.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["config", "memory", "test", "parallel", "avg_ops_per_sec", "total_ops_per_sec"])
        for (cfg, mem, test, par), vals in sorted(kernel.items()):
            avg = sum(vals) / len(vals)
            w.writerow([cfg, mem, test, par, int(avg), int(avg)])

    with (results_dir / "user-malloc.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["config", "memory", "test", "parallel", "run", "ops_per_sec"])
        for (cfg, threads), rows in sorted(user.items()):
            for run, tp in sorted(rows):
                w.writerow([cfg, "user", "user_malloc(random[16-256])", threads, run, int(tp)])


# ── plotting (mirrors p3os-paper/eval/malloc/plot_combined_allocator_figure.py) ──

COLOR_DRAM, MARK_DRAM = "black", "^"
COLOR_CXL, MARK_CXL = "#1f77b4", "o"
COLOR_ALT, MARK_ALT = "#d62728", "s"
COLOR_FIGA_CXL, MARK_FIGA_CXL = "#2ca02c", "P"
COLOR_FIGA_LOG, MARK_FIGA_LOG = "#ff7f0e", "X"


def kseries(kernel, cfg, mem, test, parallels):
    out = []
    for p in parallels:
        vals = kernel.get((cfg, mem, test, p), [])
        out.append(sum(vals) / len(vals) / 1e6 if vals else float("nan"))
    return out


def useries(user, cfg, threads):
    out = []
    for t in threads:
        rows = user.get((cfg, t), [])
        out.append(sum(v for _, v in rows) / len(rows) / 1e6 if rows else float("nan"))
    return out


def plot_line(ax, xs, ys, label, color, marker):
    ax.plot(xs, ys, marker=marker, color=color, label=label, markersize=6, linewidth=1.5)


def finish_axes(ax, ylabel):
    ax.set_xlabel("#Threads")
    ax.set_ylabel(ylabel)
    ax.set_ylim(bottom=0)
    ax.grid(True, which="both", axis="y", linestyle=":")
    ax.xaxis.set_major_locator(plt.MaxNLocator(integer=True))


def soft_legend(ax, handles=None, labels=None, *, loc="upper right", bbox=None):
    if handles is None:
        handles, labels = ax.get_legend_handles_labels()
    leg = ax.legend(
        handles, labels, loc=loc, bbox_to_anchor=bbox, ncol=1, frameon=True,
        fancybox=True, framealpha=0.65, handlelength=0.9, handletextpad=0.3,
        borderpad=0.22, labelspacing=0.22, borderaxespad=0.15,
    )
    frame = leg.get_frame()
    frame.set_facecolor("white")
    frame.set_edgecolor("#cccccc")
    frame.set_linewidth(0.8)


def legend_subset(ax, keep, *, loc, bbox=None):
    handles, labels = ax.get_legend_handles_labels()
    kept = [(h, l) for h, l in zip(handles, labels) if l in keep]
    if kept:
        soft_legend(ax, [h for h, _ in kept], [l for _, l in kept], loc=loc, bbox=bbox)


def plot_fig(kernel, user, fig_dir: Path):
    fig_dir.mkdir(parents=True, exist_ok=True)
    plt.rcdefaults()
    plt.rcParams.update({
        "figure.figsize": (8.0, 3.0), "font.size": 19, "axes.titlesize": 18,
        "axes.labelsize": 18, "legend.fontsize": 15,
        "xtick.labelsize": 16, "ytick.labelsize": 16,
    })

    xs = sorted({p for (c, _m, _t, p) in kernel})
    xs_user = sorted({t for (_c, t) in user})
    if not xs and not xs_user:
        raise SystemExit("No data parsed from logs; nothing to plot")

    fig, axes = plt.subplots(1, 3, constrained_layout=True)
    title_kw = {"pad": 16, "fontweight": "bold"}
    xticks = [x for x in (1, 32, 64, 92) if not xs or x <= max(xs) + 4]

    # (a) Slab: DRAM (LLFree+CR), CXL (Buddy), CXL-Log (LLFree+CR)
    ax = axes[0]
    plot_line(ax, xs, kseries(kernel, "LLFree+CR", "DRAM", TEST_KMALLOC, xs), "DRAM", COLOR_DRAM, MARK_DRAM)
    plot_line(ax, xs, kseries(kernel, "Buddy", "CXL", TEST_KMALLOC, xs), "CXL", COLOR_FIGA_CXL, MARK_FIGA_CXL)
    plot_line(ax, xs, kseries(kernel, "LLFree+CR", "CXL", TEST_KMALLOC, xs), "CXL-Log", COLOR_FIGA_LOG, MARK_FIGA_LOG)
    finish_axes(ax, "Thp (Mops/s)")
    ax.set_xticks(xticks)
    ax.set_title("(a) Slab", **title_kw)
    soft_legend(ax, bbox=(1, 1))

    # (b) Buddy: mixed random 4K/2M
    ax = axes[1]
    plot_line(ax, xs, kseries(kernel, "LLFree+CR", "DRAM", TEST_MIXED, xs), "DRAM", COLOR_DRAM, MARK_DRAM)
    plot_line(ax, xs, kseries(kernel, "Buddy", "CXL", TEST_MIXED, xs), "CXL-Buddy", COLOR_CXL, MARK_CXL)
    plot_line(ax, xs, kseries(kernel, "LLFree+CR", "CXL", TEST_MIXED, xs), "CXL-LLFree", COLOR_ALT, MARK_ALT)
    finish_axes(ax, "Thp (Mops/s)")
    ax.set_xticks(xticks)
    ax.set_title("(b) Buddy", **title_kw)
    legend_subset(ax, ["CXL-Buddy", "CXL-LLFree"], loc="lower center", bbox=(0.5, 0))

    # (c) rpmalloc: DRAM (LLFree+CR run, user malloc on DRAM),
    #     CXL-Buddy (Buddy), CXL-LLFree (LLFree)
    ax = axes[2]
    plot_line(ax, xs_user, useries(user, "LLFree+CR", xs_user), "DRAM", COLOR_DRAM, MARK_DRAM)
    plot_line(ax, xs_user, useries(user, "Buddy", xs_user), "CXL-Buddy", COLOR_CXL, MARK_CXL)
    plot_line(ax, xs_user, useries(user, "LLFree", xs_user), "CXL-LLFree", COLOR_ALT, MARK_ALT)
    finish_axes(ax, "Thp (Mops/s)")
    ax.set_xticks(xticks)
    ax.set_title("(c) rpmalloc", **title_kw)
    legend_subset(ax, ["CXL-Buddy", "CXL-LLFree"], loc="lower center", bbox=(0.5, 0))

    out = fig_dir / "fig12-allocator-all"
    fig.savefig(out.with_suffix(".pdf"), dpi=1200, format="pdf", bbox_inches="tight")
    fig.savefig(out.with_suffix(".eps"), dpi=1200, format="eps", bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote {out}.pdf and .eps")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log-dir", required=True, type=Path)
    ap.add_argument("--out-dir", required=True, type=Path)
    args = ap.parse_args()

    kernel, user = collect(args.log_dir)
    print(f"Parsed {len(kernel)} kernel data points, {len(user)} user data points")
    write_csvs(kernel, user, args.out_dir / "results")
    plot_fig(kernel, user, args.out_dir / "figures")


if __name__ == "__main__":
    main()
