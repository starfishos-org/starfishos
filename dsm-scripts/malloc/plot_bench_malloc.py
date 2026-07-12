#!/usr/bin/env python3
"""
Plot bench_malloc_results.csv (output of bench_malloc_e2e.sh).
Averages over runs, draws error bars (std), y-axes from 0.

Usage: python3 dsm-scripts/plot_bench_malloc.py [bench_malloc_results.csv]
"""

import csv, sys, math, collections
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSV_FILE = sys.argv[1] if len(sys.argv) > 1 else "bench_malloc_results.csv"

# ── load ──────────────────────────────────────────────────────────────────
rows = []
with open(CSV_FILE) as f:
    for r in csv.DictReader(f):
        r["parallel"] = int(r["parallel"])
        r["ops"]      = int(r["ops_per_sec"])
        if r["test"].startswith("random_get_free_4K2M"):
            r["test"] = "random_get_free_4K2M"
        rows.append(r)

# ── aggregate: IQR outlier removal, then mean ± std ───────────────────────
from collections import defaultdict
buckets = defaultdict(list)
for r in rows:
    key = (r["config"], r["memory"], r["test"], r["parallel"])
    buckets[key].append(r["ops"])

def iqr_filter(vals):
    """Remove values outside [Q1 - 1.5*IQR, Q3 + 1.5*IQR]. Keep all if n < 4."""
    if len(vals) < 4:
        return vals
    s = sorted(vals)
    n = len(s)
    q1 = s[n // 4]
    q3 = s[(3 * n) // 4]
    iqr = q3 - q1
    lo, hi = q1 - 1.5 * iqr, q3 + 1.5 * iqr
    filtered = [v for v in vals if lo <= v <= hi]
    return filtered if filtered else vals

def stats(vals):
    vals = iqr_filter(vals)
    n   = len(vals)
    mu  = sum(vals) / n
    std = math.sqrt(sum((v - mu)**2 for v in vals) / n) if n > 1 else 0
    return mu, std, n

def get_series(test, config, mem):
    pars = sorted(set(k[3] for k in buckets if k[0]==config and k[1]==mem and k[2]==test))
    xs, ys, errs = [], [], []
    for p in pars:
        key = (config, mem, test, p)
        if key not in buckets: continue
        mu, std, _ = stats(buckets[key])
        xs.append(p); ys.append(mu); errs.append(std)
    return xs, ys, errs

# ── style ─────────────────────────────────────────────────────────────────
# Series: (config, memory) -> display label
SERIES = {
    ("Buddy",    "DRAM"): "DRAM",
    ("Buddy",    "CXL"):  "CXL-Buddy",
    ("LLFree",   "CXL"):  "CXL-LLFree",
    ("LLFree+CR","CXL"):  "CXL-LLFree+CR",
}
USER_SERIES = {
    ("Buddy",    "user"): "Buddy",
    ("LLFree",   "user"): "LLFree",
    ("LLFree+CR","user"): "LLFree+CR",
}
USER_COLORS = {"Buddy": "#ff7f0e", "LLFree": "#2ca02c", "LLFree+CR": "#d62728"}
USER_MARKERS = {"Buddy": "s", "LLFree": "^", "LLFree+CR": "D"}
COLORS  = {
    "DRAM":          "#1f77b4",
    "CXL-Buddy":     "#ff7f0e",
    "CXL-LLFree":    "#2ca02c",
    "CXL-LLFree+CR": "#d62728",
}
MARKERS = {"DRAM":"o", "CXL-Buddy":"s", "CXL-LLFree":"^", "CXL-LLFree+CR":"D"}
LS      = {"DRAM":"-", "CXL-Buddy":"--", "CXL-LLFree":"--", "CXL-LLFree+CR":"--"}

fmt = plt.FuncFormatter(lambda v, _: f"{v/1e6:.1f}M" if v >= 1e6 else f"{int(v/1e3)}K")

# CXL-LLFree+CR shown only for kmalloc
def series_for(test):
    return {k: v for k, v in SERIES.items()
            if v != "CXL-LLFree+CR" or test == "kmalloc"}

def draw_ax(ax, test, title):
    for (cfg, mem), label in series_for(test).items():
        xs, ys, errs = get_series(test, cfg, mem)
        if not xs: continue
        ax.errorbar(xs, ys, yerr=errs,
                    color=COLORS[label], linestyle=LS[label],
                    marker=MARKERS[label], markersize=5, linewidth=1.8,
                    capsize=3, elinewidth=1, label=label)
    ax.set_title(title, fontsize=10)
    ax.set_xlabel("parallel threads")
    ax.set_ylabel("ops/s")
    ax.set_ylim(bottom=0)
    ax.grid(True, alpha=0.3)
    ax.yaxis.set_major_formatter(fmt)

# ── Fig 1: overview 2×3 ───────────────────────────────────────────────────
PLOT_TESTS = [
    ("kmalloc",               "kmalloc"),
    ("get_pages(4KB)-alloc",  "get_pages 4KB alloc"),
    ("get_pages(4KB)-free",   "free_pages 4KB"),
    ("get_pages(2MB)-alloc",  "get_pages 2MB alloc"),
    ("get_pages(2MB)-free",   "free_pages 2MB"),
    ("random_get_free_4K2M",  "random alloc/free 4K+2M"),
]

fig, axes = plt.subplots(2, 3, figsize=(15, 9))
for ax, (test, title) in zip(axes.flatten(), PLOT_TESTS):
    draw_ax(ax, test, title)

# shared legend from kmalloc panel (has all 4 series)
handles, labels = axes[0][0].get_legend_handles_labels()
fig.legend(handles, labels, loc="lower center", ncol=4,
           bbox_to_anchor=(0.5, -0.01), fontsize=10, framealpha=0.9)
nruns = max(int(r['run']) for r in rows)
fig.suptitle(
    "Kernel Allocator Benchmark: DRAM vs CXL (Buddy / LLFree / LLFree+CR)\n"
    f"mean ± std ({nruns} runs, IQR outliers removed)  |  "
    "CXL-LLFree+CR shown only for kmalloc",
    fontsize=11)
fig.tight_layout(rect=[0, 0.06, 1, 1])
out1 = CSV_FILE.replace(".csv", "_overview.png")
fig.savefig(out1, dpi=130, bbox_inches="tight")
print(f"Saved {out1}")

# ── Fig 2: CXL focus ──────────────────────────────────────────────────────
CXL_TESTS = [
    ("get_pages(4KB)-free",  "free_pages 4KB"),
    ("get_pages(4KB)-alloc", "get_pages 4KB alloc"),
    ("kmalloc",               "kmalloc"),
]
fig2, axes2 = plt.subplots(1, 3, figsize=(13, 4.5))
for ax, (test, title) in zip(axes2, CXL_TESTS):
    draw_ax(ax, test, title)
    ax.legend(fontsize=9)

fig2.suptitle("CXL Allocator: Buddy vs LLFree vs LLFree+CR", fontsize=11)
fig2.tight_layout()
out2 = CSV_FILE.replace(".csv", "_cxl.png")
fig2.savefig(out2, dpi=130, bbox_inches="tight")
print(f"Saved {out2}")

# ── Fig 3: user-space malloc (line plot, threads on x-axis) ───────────────
user_tests = sorted(set(r["test"] for r in rows if r["test"].startswith("user_malloc")))
if user_tests:
    ncols = len(user_tests)
    fig3, axes3 = plt.subplots(1, ncols, figsize=(6 * ncols, 4.5), squeeze=False)
    for ax3, test in zip(axes3[0], user_tests):
        for (cfg, mem), label in USER_SERIES.items():
            pars = sorted(set(k[3] for k in buckets if k[0]==cfg and k[1]==mem and k[2]==test))
            xs, ys, errs = [], [], []
            for p in pars:
                key = (cfg, mem, test, p)
                if key not in buckets: continue
                mu, std, _ = stats(buckets[key])
                xs.append(p); ys.append(mu); errs.append(std)
            if not xs: continue
            ax3.errorbar(xs, ys, yerr=errs,
                         color=USER_COLORS[label], marker=USER_MARKERS[label],
                         markersize=5, linewidth=1.8, linestyle="--",
                         capsize=3, elinewidth=1, label=label)
        ax3.set_title(test, fontsize=10)
        ax3.set_xlabel("threads")
        ax3.set_ylabel("ops/s")
        ax3.set_ylim(bottom=0)
        ax3.yaxis.set_major_formatter(fmt)
        ax3.legend(fontsize=9)
        ax3.grid(True, alpha=0.3)
    fig3.suptitle("User-space malloc throughput (rpmalloc)", fontsize=11)
    fig3.tight_layout()
    out3 = CSV_FILE.replace(".csv", "_user_malloc.png")
    fig3.savefig(out3, dpi=130, bbox_inches="tight")
    print(f"Saved {out3}")
else:
    print("No user_malloc rows found in CSV — skipping user malloc figure.")
