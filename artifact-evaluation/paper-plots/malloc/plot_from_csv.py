#!/usr/bin/env python3
"""
Redraw malloc/page figures from eval/malloc/allocator.csv (single-run table).

Mappings (must match how allocator.csv was exported):
  Fig01  DRAM        -> LLFree+CR + DRAM
         CXL         -> Buddy + CXL  (CXL-Buddy baseline)
         CXL-Log-ON  -> LLFree + CXL  (logging baseline; was 2.txt in log pipeline)
  Fig02–06 / Fig07   DRAM & CXL-LLfree -> LLFree+CR; CXL-Buddy -> Buddy

CSV has no per-point stdev → all curves use plot_series (no error bars).

Run from eval/:  python3 malloc/plot_from_csv.py
"""
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

from memory_fig_style import (  # noqa: E402
    apply_rpmalloc_rcparams,
    finish_axes_like_rpmalloc,
    plot_series,
    rpmalloc_tab20_palette,
)

PATH_CSV = _DIR / "allocator.csv"

CONFIG_BUDDY = "Buddy"
CONFIG_LOG_ON = "LLFree"
CONFIG_LLFREE_CR = "LLFree+CR"

TEST_KMALLOC = "kmalloc"
TEST_GP4K_ALLOC = "get_pages(4KB)-alloc"
TEST_GP4K_FREE = "get_pages(4KB)-free"
TEST_GP2M_ALLOC = "get_pages(2MB)-alloc"
TEST_GP2M_FREE = "get_pages(2MB)-free"


def load_csv(path: Path) -> dict[tuple[str, str, str, int], float]:
    """Key (config, memory, test, parallel) -> avg_ops_per_sec."""
    out: dict[tuple[str, str, str, int], float] = {}
    with path.open(encoding="utf-8", newline="") as f:
        for row in csv.DictReader(f):
            cfg = row["config"].strip()
            mem = row["memory"].strip()
            test = row["test"].strip()
            p = int(row["parallel"])
            v = float(row["avg_ops_per_sec"])
            out[(cfg, mem, test, p)] = v
    return out


def mixed_test_key(table: dict) -> str:
    for (cfg, mem, test, _p) in table:
        if test.startswith("random_get_free"):
            return test
    raise KeyError("No random_get_free_* test row in allocator.csv")


def series(
    table: dict,
    cfg: str,
    mem: str,
    test: str,
    parallels: list[int],
) -> list[float]:
    ys = []
    for p in parallels:
        k = (cfg, mem, test, p)
        if k not in table:
            ys.append(float("nan"))
        else:
            ys.append(table[k])
    return ys


def save_fig(stem: str) -> None:
    out_base = _DIR / stem
    plt.tight_layout()
    plt.savefig(out_base.with_suffix(".eps"), dpi=1200, format="eps", bbox_inches="tight")
    plt.savefig(out_base.with_suffix(".pdf"), dpi=1200, format="pdf", bbox_inches="tight")


def main() -> None:
    table = load_csv(PATH_CSV)
    test_mixed = mixed_test_key(table)

    xs = sorted({p for (c, _m, _t, p) in table if c == CONFIG_BUDDY})

    apply_rpmalloc_rcparams()
    pal = rpmalloc_tab20_palette()
    c_dram, m_dram = pal["dram"]
    c_cxl, m_cxl = pal["cxl"]
    c_alt, m_alt = pal["cxl_alt"]

    def mops(ys: list[float]) -> list[float]:
        return [y / 1e6 for y in ys]

    def kops(ys: list[float]) -> list[float]:
        return [y / 1e3 for y in ys]

    # --- Fig01 ---
    fig, ax = plt.subplots()
    plot_series(
        ax,
        xs,
        mops(series(table, CONFIG_LLFREE_CR, "DRAM", TEST_KMALLOC, xs)),
        "DRAM",
        c_dram,
        m_dram,
    )
    plot_series(
        ax,
        xs,
        mops(series(table, CONFIG_BUDDY, "CXL", TEST_KMALLOC, xs)),
        "CXL",
        c_cxl,
        m_cxl,
    )
    plot_series(
        ax,
        xs,
        mops(series(table, CONFIG_LOG_ON, "CXL", TEST_KMALLOC, xs)),
        "CXL-Log-ON",
        c_alt,
        m_alt,
    )
    finish_axes_like_rpmalloc(ax, "Thp (Mops/s)")
    save_fig("fig01-kmalloc-dram-cxl-logon")

    # --- Fig02 ---
    fig, ax = plt.subplots()
    plot_series(
        ax,
        xs,
        mops(series(table, CONFIG_LLFREE_CR, "DRAM", TEST_KMALLOC, xs)),
        "DRAM",
        c_dram,
        m_dram,
    )
    plot_series(
        ax,
        xs,
        mops(series(table, CONFIG_BUDDY, "CXL", TEST_KMALLOC, xs)),
        "CXL-Buddy",
        c_cxl,
        m_cxl,
    )
    plot_series(
        ax,
        xs,
        mops(series(table, CONFIG_LLFREE_CR, "CXL", TEST_KMALLOC, xs)),
        "CXL-LLfree",
        c_alt,
        m_alt,
    )
    finish_axes_like_rpmalloc(ax, "Thp (Mops/s)")
    save_fig("fig02-kmalloc-buddy-llfree")

    def draw_page(stem: str, test: str, scale: str) -> None:
        fig, ax = plt.subplots()
        if scale == "M":
            conv = mops
            unit = "Mops/s"
        else:
            conv = kops
            unit = "Kops/s"
        plot_series(
            ax,
            xs,
            conv(series(table, CONFIG_LLFREE_CR, "DRAM", test, xs)),
            "DRAM",
            c_dram,
            m_dram,
        )
        plot_series(
            ax,
            xs,
            conv(series(table, CONFIG_BUDDY, "CXL", test, xs)),
            "CXL-Buddy",
            c_cxl,
            m_cxl,
        )
        plot_series(
            ax,
            xs,
            conv(series(table, CONFIG_LLFREE_CR, "CXL", test, xs)),
            "CXL-LLfree",
            c_alt,
            m_alt,
        )
        finish_axes_like_rpmalloc(ax, f"Thp ({unit})")
        save_fig(stem)

    draw_page("fig03-page-4k-alloc", TEST_GP4K_ALLOC, "M")
    draw_page("fig04-page-4k-free", TEST_GP4K_FREE, "M")
    draw_page("fig05-page-2m-alloc", TEST_GP2M_ALLOC, "K")
    draw_page("fig06-page-2m-free", TEST_GP2M_FREE, "K")

    # --- Fig07: mixed (LLFree+CR only, same as log pipeline) ---
    fig, ax = plt.subplots()
    plot_series(
        ax,
        xs,
        mops(series(table, CONFIG_LLFREE_CR, "DRAM", test_mixed, xs)),
        "DRAM",
        c_dram,
        m_dram,
    )
    plot_series(
        ax,
        xs,
        mops(series(table, CONFIG_LLFREE_CR, "CXL", test_mixed, xs)),
        "CXL",
        c_cxl,
        m_cxl,
    )
    finish_axes_like_rpmalloc(ax, "Thp (M loop-iters/s)")
    save_fig("fig07-mixed-random-4k2m")

    plt.close("all")
    print("Wrote eval/malloc/fig01-fig07 from allocator.csv (.eps and .pdf)")


if __name__ == "__main__":
    main()
