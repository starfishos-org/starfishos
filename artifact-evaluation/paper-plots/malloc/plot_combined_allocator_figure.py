#!/usr/bin/env python3
from __future__ import annotations

import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

_DIR = Path(__file__).resolve().parent
_EVAL_ROOT = _DIR.parent
if str(_EVAL_ROOT) not in sys.path:
    sys.path.insert(0, str(_EVAL_ROOT))

from memory_fig_style import (
    apply_rpmalloc_rcparams,
    finish_axes_like_rpmalloc,
    plot_series,
    rpmalloc_tab20_palette,
)

PATH_ALLOC = _DIR / "allocator.csv"
PATH_USER = _DIR / "user-malloc.csv"

CONFIG_BUDDY = "Buddy"
CONFIG_LOG_ON = "LLFree"
CONFIG_LLFREE_CR = "LLFree+CR"

TEST_KMALLOC = "kmalloc"
TEST_GP4K_ALLOC = "get_pages(4KB)-alloc"
TEST_GP4K_FREE = "get_pages(4KB)-free"
TEST_GP2M_ALLOC = "get_pages(2MB)-alloc"
TEST_GP2M_FREE = "get_pages(2MB)-free"


def mixed_test_key(table: dict[tuple[str, str, str, int], float]) -> str:
    for (_cfg, _mem, test, _p) in table:
        if test.startswith("random_get_free"):
            return test
    raise KeyError("No random_get_free_* test row in allocator.csv")


def load_alloc_csv(path: Path) -> dict[tuple[str, str, str, int], float]:
    out: dict[tuple[str, str, str, int], float] = {}
    with path.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            out[
                (
                    row["config"].strip(),
                    row["memory"].strip(),
                    row["test"].strip(),
                    int(row["parallel"]),
                )
            ] = float(row["avg_ops_per_sec"])
    return out


def load_user_csv(path: Path) -> tuple[list[int], dict[str, list[float]]]:
    grouped: dict[tuple[str, int], list[float]] = {}
    parallels = set()
    with path.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            if row["memory"].strip() != "user":
                continue
            if "user_malloc" not in row["test"]:
                continue
            cfg = row["config"].strip()
            p = int(row["parallel"])
            v = float(row["ops_per_sec"])
            parallels.add(p)
            grouped.setdefault((cfg, p), []).append(v)

    xs = sorted(parallels)
    series = {}
    for label, cfg in (
        ("Buddy", "Buddy"),
        ("LLFree", "LLFree"),
        ("DRAM", "LLFree+CR"),
    ):
        ys = []
        for p in xs:
            vals = grouped.get((cfg, p), [])
            ys.append(sum(vals) / len(vals) if vals else float("nan"))
        series[label] = ys
    return xs, series


def series(
    table: dict[tuple[str, str, str, int], float],
    cfg: str,
    mem: str,
    test: str,
    parallels: list[int],
) -> list[float]:
    return [table.get((cfg, mem, test, p), float("nan")) for p in parallels]


def mops(ys: list[float]) -> list[float]:
    return [y / 1e6 for y in ys]


def kops(ys: list[float]) -> list[float]:
    return [y / 1e3 for y in ys]


def save_fig(fig: plt.Figure, stem: str) -> None:
    out_base = _DIR / stem
    fig.savefig(out_base.with_suffix(".eps"), dpi=1200, format="eps", bbox_inches="tight")
    fig.savefig(out_base.with_suffix(".pdf"), dpi=1200, format="pdf", bbox_inches="tight")


def plot_series_big(ax, xs, ys, label: str, color, marker: str) -> None:
    ax.plot(
        xs,
        ys,
        marker=marker,
        color=color,
        label=label,
        markersize=6,
        linewidth=1.5,
    )


def soften_legend(ax, *, loc: str = "upper right", bbox_to_anchor=None) -> None:
    leg = ax.legend(
        loc=loc,
        bbox_to_anchor=bbox_to_anchor,
        ncol=1,
        frameon=True,
        fancybox=True,
        framealpha=0.65,
        handlelength=0.9,
        handletextpad=0.3,
        borderpad=0.22,
        labelspacing=0.22,
        borderaxespad=0.15,
    )
    frame = leg.get_frame()
    frame.set_facecolor("white")
    frame.set_edgecolor("#cccccc")
    frame.set_linewidth(0.8)


def split_legend(ax, anchors: list[tuple[float, float]]) -> None:
    handles, labels = ax.get_legend_handles_labels()
    for handle, label, anchor in zip(handles, labels, anchors):
        leg = ax.legend(
            [handle],
            [label],
            loc="lower center",
            bbox_to_anchor=anchor,
            ncol=1,
            frameon=True,
            fancybox=True,
            framealpha=0.65,
            handlelength=1.6,
            handletextpad=0.4,
            borderpad=0.3,
        )
        frame = leg.get_frame()
        frame.set_facecolor("white")
        frame.set_edgecolor("#cccccc")
        frame.set_linewidth(0.8)
        ax.add_artist(leg)


def legend_subset(ax, keep_labels: list[str], *, loc: str, bbox_to_anchor=None) -> None:
    handles, labels = ax.get_legend_handles_labels()
    filtered = [(h, l) for h, l in zip(handles, labels) if l in keep_labels]
    if not filtered:
        return
    leg = ax.legend(
        [h for h, _ in filtered],
        [l for _, l in filtered],
        loc=loc,
        bbox_to_anchor=bbox_to_anchor,
        ncol=1,
        frameon=True,
        fancybox=True,
        framealpha=0.65,
        borderpad=0.25,
        labelspacing=0.25,
        handlelength=1.1,
        handletextpad=0.35,
        borderaxespad=0.2,
    )
    frame = leg.get_frame()
    frame.set_facecolor("white")
    frame.set_edgecolor("#cccccc")
    frame.set_linewidth(0.8)


def main() -> None:
    apply_rpmalloc_rcparams()
    plt.rcParams.update(
        {
            "figure.figsize": (8.0, 3.0),
            "font.size": 19,
            "axes.titlesize": 18,
            "axes.labelsize": 18,
            "legend.fontsize": 15,
            "xtick.labelsize": 16,
            "ytick.labelsize": 16,
        }
    )

    table = load_alloc_csv(PATH_ALLOC)
    test_mixed = mixed_test_key(table)
    xs = sorted({p for (c, _m, _t, p) in table if c == CONFIG_BUDDY})
    xs_user, user_series = load_user_csv(PATH_USER)

    pal = rpmalloc_tab20_palette()
    c_dram, m_dram = pal["dram"]
    c_cxl, m_cxl = pal["cxl"]
    c_alt, m_alt = pal["cxl_alt"]
    c_figa_cxl = "#2ca02c"
    c_figa_log = "#ff7f0e"
    m_figa_cxl = "P"
    m_figa_log = "X"

    fig, axes = plt.subplots(1, 3, constrained_layout=True)
    axes = axes.flatten()

    title_kw = {"pad": 16, "fontweight": "bold"}
    xticks = [1,32, 64, 92]

    ax = axes[0]
    plot_series_big(ax, xs, mops(series(table, CONFIG_LLFREE_CR, "DRAM", TEST_KMALLOC, xs)), "DRAM", c_dram, m_dram)
    plot_series_big(ax, xs, mops(series(table, CONFIG_BUDDY, "CXL", TEST_KMALLOC, xs)), "CXL", c_figa_cxl, m_figa_cxl)
    plot_series_big(ax, xs, mops(series(table, CONFIG_LOG_ON, "CXL", TEST_KMALLOC, xs)), "CXL-Log", c_figa_log, m_figa_log)
    finish_axes_like_rpmalloc(ax, "Thp (Mops/s)")
    ax.set_xticks(xticks)
    ax.set_title("(a) Slab", **title_kw)
    soften_legend(ax, bbox_to_anchor=(1, 1))

    ax = axes[1]
    plot_series_big(ax, xs, mops(series(table, CONFIG_LLFREE_CR, "DRAM", test_mixed, xs)), "DRAM", c_dram, m_dram)
    plot_series_big(ax, xs, mops(series(table, CONFIG_BUDDY, "CXL", test_mixed, xs)), "CXL-Buddy", c_cxl, m_cxl)
    plot_series_big(ax, xs, mops(series(table, CONFIG_LLFREE_CR, "CXL", test_mixed, xs)), "CXL-LLFree", c_alt, m_alt)
    finish_axes_like_rpmalloc(ax, "Thp (Mops/s)")
    ax.set_xticks(xticks)
    ax.set_title("(b) Buddy", **title_kw)
    legend_subset(ax, ["CXL-Buddy", "CXL-LLFree"], loc="lower center", bbox_to_anchor=(0.5, 0))

    ax = axes[2]
    plot_series_big(ax, xs_user, mops(user_series["DRAM"]), "DRAM", c_dram, m_dram)
    plot_series_big(ax, xs_user, mops(user_series["Buddy"]), "CXL-Buddy", c_cxl, m_cxl)
    plot_series_big(ax, xs_user, mops(user_series["LLFree"]), "CXL-LLFree", c_alt, m_alt)
    finish_axes_like_rpmalloc(ax, "Thp (Mops/s)")
    ax.set_xticks(xticks)
    ax.set_title("(c) rpmalloc", **title_kw)
    legend_subset(ax, ["CXL-Buddy", "CXL-LLFree"], loc="lower center", bbox_to_anchor=(0.5, 0))

    save_fig(fig, "fig00-allocator-all")
    plt.close(fig)
    print("Wrote eval/malloc/fig00-allocator-all.eps and .pdf")


if __name__ == "__main__":
    main()
