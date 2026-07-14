#!/usr/bin/env python3
"""
Parse kernel malloc test logs (1.txt = CXL-Buddy; 2.txt = CXL-Log-ON).
DRAM & CXL-LLfree page/kmalloc stats: mean ± sample stdev over llfree1.txt + llfree2.txt only.
Fig07: mixed random 4KiB+2MiB (same two logs).
Warns if DRAM relative stdev/mean exceeds DRAM_ERRBAR_REL_WARN or fewer than 2 llfree samples.

Figures (graph_spec.txt):
  1. kmalloc: DRAM, CXL, CXL-Log-ON
  2. kmalloc: DRAM, CXL-Buddy, CXL-LLfree  (kernel kmalloc; same metric as logs)
  3. malloc 4KiB (get_pages alloc)
  4. free 4KiB (free_pages)
  5. malloc 2MiB
  6. free 2MiB
"""
from __future__ import annotations

import re
import statistics
import sys
import warnings
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
    plot_series_yerr,
    rpmalloc_tab20_palette,
)

# Input logs (paths relative to this script)
PATH_BUDDY = _DIR / "1.txt"
PATH_LOG_ON = _DIR / "2.txt"
PATH_LLFREE_A = _DIR / "llfree1.txt"
PATH_LLFREE_B = _DIR / "llfree2.txt"

# Relative sample stdev / |mean|; above this -> warnings.warn (tune if too noisy)
DRAM_ERRBAR_REL_WARN = 0.15

_RE_PARALLEL = re.compile(r"start malloc test parallel=(\d+)")
_RE_DRAM_KMALLOC = re.compile(
    r"DRAM kmalloc avg throughput \(parallel=\d+\): (\d+) ops/s"
)
_RE_CXL_KMALLOC = re.compile(
    r"CXL kmalloc avg throughput \(parallel=\d+\): (\d+) ops/s"
)
_RE_GET_4K = re.compile(
    r"get_pages throughput \((DRAM|CXL), 4096B\): (\d+) ops/s"
)
_RE_FREE_4K = re.compile(
    r"free_pages throughput \((DRAM|CXL), 4096B\): (\d+) ops/s"
)
_RE_GET_2M = re.compile(
    r"get_pages throughput \((DRAM|CXL), 2097152B\): (\d+) ops/s"
)
_RE_FREE_2M = re.compile(
    r"free_pages throughput \((DRAM|CXL), 2097152B\): (\d+) ops/s"
)
_RE_MIXED_RANDOM = re.compile(
    r"(DRAM|CXL) random get_pages/free_pages 4KiB\+2MiB iters=\d+ throughput=(\d+) loop-iters/s"
)


def parse_log(path: Path) -> dict:
    """Return blocks keyed by parallel: kmalloc, get4k, free4k, get2m, free2m."""
    text = path.read_text(encoding="utf-8", errors="replace")
    parallel = None
    out: dict[int, dict] = {}

    def ensure(p: int):
        if p not in out:
            out[p] = {
                "dram_kmalloc": None,
                "cxl_kmalloc": None,
                "get4k": {},
                "free4k": {},
                "get2m": {},
                "free2m": {},
                "mixed_random": {},
            }
        return out[p]

    for line in text.splitlines():
        m = _RE_PARALLEL.search(line)
        if m:
            parallel = int(m.group(1))
            ensure(parallel)
            continue
        if parallel is None:
            continue
        blk = ensure(parallel)
        m = _RE_DRAM_KMALLOC.search(line)
        if m:
            blk["dram_kmalloc"] = int(m.group(1))
            continue
        m = _RE_CXL_KMALLOC.search(line)
        if m:
            blk["cxl_kmalloc"] = int(m.group(1))
            continue
        m = _RE_GET_4K.search(line)
        if m:
            blk["get4k"][m.group(1)] = int(m.group(2))
            continue
        m = _RE_FREE_4K.search(line)
        if m:
            blk["free4k"][m.group(1)] = int(m.group(2))
            continue
        m = _RE_GET_2M.search(line)
        if m:
            blk["get2m"][m.group(1)] = int(m.group(2))
            continue
        m = _RE_FREE_2M.search(line)
        if m:
            blk["free2m"][m.group(1)] = int(m.group(2))
            continue
        m = _RE_MIXED_RANDOM.search(line)
        if m:
            blk["mixed_random"][m.group(1)] = int(m.group(2))
            continue

    return out


def mean_std_two(v1: int | None, v2: int | None) -> tuple[float | None, float]:
    """Mean and sample stdev over non-None values (0.0 stdev if only one run)."""
    vals = [float(v) for v in (v1, v2) if v is not None]
    if not vals:
        return None, 0.0
    if len(vals) == 1:
        return vals[0], 0.0
    return statistics.mean(vals), statistics.stdev(vals)


def warn_dram_errbar(
    fig_id: str,
    parallel: int,
    metric: str,
    mean_raw: float,
    stdev_raw: float,
    n_runs: int,
) -> None:
    if n_runs < 2:
        warnings.warn(
            f"{fig_id} parallel={parallel} {metric}: DRAM only {n_runs}/2 llfree logs — missing run(s).",
            UserWarning,
            stacklevel=2,
        )
    if mean_raw == 0:
        return
    rel = stdev_raw / abs(mean_raw)
    if stdev_raw > 0 and rel > DRAM_ERRBAR_REL_WARN:
        warnings.warn(
            f"{fig_id} parallel={parallel} {metric}: DRAM error bar too wide: "
            f"stdev/|mean|={rel:.1%} (threshold {DRAM_ERRBAR_REL_WARN:.0%}).",
            UserWarning,
            stacklevel=2,
        )


def save_fig(stem: str):
    out_base = _DIR / stem
    plt.tight_layout()
    plt.savefig(out_base.with_suffix(".eps"), dpi=1200, format="eps", bbox_inches="tight")
    plt.savefig(out_base.with_suffix(".pdf"), dpi=1200, format="pdf", bbox_inches="tight")


def main():
    buddy = parse_log(PATH_BUDDY)
    log_on = parse_log(PATH_LOG_ON)
    llfree_a = parse_log(PATH_LLFREE_A)
    llfree_b = parse_log(PATH_LLFREE_B)

    apply_rpmalloc_rcparams()

    pal = rpmalloc_tab20_palette()
    c_dram, m_dram = pal["dram"]
    c_cxl, m_cxl = pal["cxl"]
    c_alt, m_alt = pal["cxl_alt"]

    # --- Fig 1: kmalloc DRAM (llfree1+2), CXL (Buddy), CXL-Log-ON (2.txt CXL) ---
    xs_b = sorted(buddy.keys())
    dram1_m, dram1_e = [], []
    for p in xs_b:
        da = llfree_a.get(p, {}).get("dram_kmalloc")
        db = llfree_b.get(p, {}).get("dram_kmalloc")
        m, s = mean_std_two(da, db)
        n = sum(1 for x in (da, db) if x is not None)
        warn_dram_errbar("fig01", p, "DRAM kmalloc", m if m is not None else 0.0, s, n)
        dram1_m.append(m / 1e6 if m is not None else float("nan"))
        dram1_e.append(s / 1e6 if m is not None else 0.0)
    cxl_b = [buddy[p]["cxl_kmalloc"] for p in xs_b]
    cxl_lo = [log_on.get(p, {}).get("cxl_kmalloc") for p in xs_b]

    fig, ax = plt.subplots()
    plot_series_yerr(
        ax,
        xs_b,
        dram1_m,
        dram1_e,
        "DRAM",
        c_dram,
        m_dram,
    )
    plot_series(
        ax,
        xs_b,
        [float(x) / 1e6 for x in cxl_b],
        "CXL",
        c_cxl,
        m_cxl,
    )
    plot_series(
        ax,
        xs_b,
        [float(v) / 1e6 if v is not None else float("nan") for v in cxl_lo],
        "CXL-Log-ON",
        c_alt,
        m_alt,
    )
    finish_axes_like_rpmalloc(ax, "Thp (Mops/s)")
    save_fig("fig01-kmalloc-dram-cxl-logon")

    # --- Fig 2: DRAM (llfree1+2), CXL-Buddy, CXL-LLfree (kmalloc); x = buddy run threads ---
    xs_union = sorted(buddy.keys())
    dram2_m, dram2_e = [], []
    for p in xs_union:
        da = llfree_a.get(p, {}).get("dram_kmalloc")
        db = llfree_b.get(p, {}).get("dram_kmalloc")
        m, s = mean_std_two(da, db)
        n = sum(1 for x in (da, db) if x is not None)
        warn_dram_errbar("fig02", p, "DRAM kmalloc", m if m is not None else 0.0, s, n)
        dram2_m.append(m / 1e6 if m is not None else float("nan"))
        dram2_e.append(s / 1e6 if m is not None else 0.0)
    cxl_buddy = [buddy[p]["cxl_kmalloc"] for p in xs_union]
    cxl_ll_m = []
    cxl_ll_e = []
    for p in xs_union:
        m, s = mean_std_two(
            llfree_a.get(p, {}).get("cxl_kmalloc"),
            llfree_b.get(p, {}).get("cxl_kmalloc"),
        )
        cxl_ll_m.append(m / 1e6 if m is not None else float("nan"))
        cxl_ll_e.append(s / 1e6 if m is not None else 0.0)

    fig, ax = plt.subplots()
    plot_series_yerr(
        ax,
        xs_union,
        dram2_m,
        dram2_e,
        "DRAM",
        c_dram,
        m_dram,
    )
    plot_series(
        ax,
        xs_union,
        [float(x) / 1e6 if x is not None else float("nan") for x in cxl_buddy],
        "CXL-Buddy",
        c_cxl,
        m_cxl,
    )
    plot_series_yerr(
        ax,
        xs_union,
        cxl_ll_m,
        cxl_ll_e,
        "CXL-LLfree",
        c_alt,
        m_alt,
    )
    finish_axes_like_rpmalloc(ax, "Thp (Mops/s)")
    save_fig("fig02-kmalloc-buddy-llfree")

    def draw_page(
        stem: str,
        subkey: str,
        y_scale: float,
        y_unit: str,
    ):
        xs_u = sorted(buddy.keys())
        dram_m, dram_e = [], []
        cxl_b_y = []
        cxl_l_y = []
        cxl_l_e = []
        for p in xs_u:
            b = buddy[p]
            sub_b = b.get(subkey) or {}
            sa = (llfree_a.get(p, {}) or {}).get(subkey) or {}
            sb = (llfree_b.get(p, {}) or {}).get(subkey) or {}
            da = sa.get("DRAM")
            db = sb.get("DRAM")
            dm, ds = mean_std_two(da, db)
            dn = sum(1 for x in (da, db) if x is not None)
            warn_dram_errbar(stem, p, f"DRAM/{subkey}", dm if dm is not None else 0.0, ds, dn)
            dram_m.append(dm / y_scale if dm is not None else float("nan"))
            dram_e.append(ds / y_scale if dm is not None else 0.0)
            cxl_b_y.append(sub_b.get("CXL"))
            m, s = mean_std_two(sa.get("CXL"), sb.get("CXL"))
            cxl_l_y.append(m / y_scale if m is not None else float("nan"))
            cxl_l_e.append(s / y_scale if m is not None else 0.0)
        fig, ax = plt.subplots()
        plot_series_yerr(
            ax,
            xs_u,
            dram_m,
            dram_e,
            "DRAM",
            c_dram,
            m_dram,
        )
        plot_series(
            ax,
            xs_u,
            [float(x) / y_scale if x is not None else float("nan") for x in cxl_b_y],
            "CXL-Buddy",
            c_cxl,
            m_cxl,
        )
        plot_series_yerr(
            ax,
            xs_u,
            cxl_l_y,
            cxl_l_e,
            "CXL-LLfree",
            c_alt,
            m_alt,
        )
        finish_axes_like_rpmalloc(ax, f"Thp ({y_unit})")
        save_fig(stem)

    draw_page("fig03-page-4k-alloc", "get4k", 1e6, "Mops/s")
    draw_page("fig04-page-4k-free", "free4k", 1e6, "Mops/s")
    draw_page("fig05-page-2m-alloc", "get2m", 1e3, "Kops/s")
    draw_page("fig06-page-2m-free", "free2m", 1e3, "Kops/s")

    # --- Fig 7: mixed random 4KiB+2MiB get/free (llfree1 + llfree2 mean ± stdev) ---
    xs_mix = sorted(set(llfree_a.keys()) & set(llfree_b.keys()))
    mix_dram_m, mix_dram_e, mix_cxl_m, mix_cxl_e = [], [], [], []
    for p in xs_mix:
        d1 = (llfree_a.get(p, {}).get("mixed_random") or {}).get("DRAM")
        d2 = (llfree_b.get(p, {}).get("mixed_random") or {}).get("DRAM")
        md, sd = mean_std_two(d1, d2)
        mix_dram_m.append(md / 1e6 if md is not None else float("nan"))
        mix_dram_e.append(sd / 1e6 if md is not None else 0.0)
        c1 = (llfree_a.get(p, {}).get("mixed_random") or {}).get("CXL")
        c2 = (llfree_b.get(p, {}).get("mixed_random") or {}).get("CXL")
        mc, sc = mean_std_two(c1, c2)
        mix_cxl_m.append(mc / 1e6 if mc is not None else float("nan"))
        mix_cxl_e.append(sc / 1e6 if mc is not None else 0.0)

    fig, ax = plt.subplots()
    plot_series_yerr(
        ax,
        xs_mix,
        mix_dram_m,
        mix_dram_e,
        "DRAM",
        c_dram,
        m_dram,
    )
    plot_series_yerr(
        ax,
        xs_mix,
        mix_cxl_m,
        mix_cxl_e,
        "CXL",
        c_cxl,
        m_cxl,
    )
    finish_axes_like_rpmalloc(ax, "Thp (M loop-iters/s)")
    save_fig("fig07-mixed-random-4k2m")

    plt.close("all")
    print("Wrote eval/malloc/fig01-fig07 (.eps and .pdf)")


if __name__ == "__main__":
    main()
