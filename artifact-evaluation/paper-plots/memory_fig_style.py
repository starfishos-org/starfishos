"""Shared line-plot style for memory microbenchmark figures."""
from __future__ import annotations

import json
from pathlib import Path

import matplotlib.pyplot as plt

_DIR = Path(__file__).resolve().parent
_CUSTOM_JSON = _DIR / "memory_custom_colors.json"

# Defaults if JSON missing / invalid keys (ours matches eval/matrix_multi.py SYSTEM_NAME)
_DEFAULT_CXL_BLUE = "#1f77b4"
_DEFAULT_OURS = "#d62728"


def _load_custom_rgb_hex() -> tuple[str, str]:
    try:
        data = json.loads(_CUSTOM_JSON.read_text(encoding="utf-8"))
        blue = str(data.get("cxl_blue", _DEFAULT_CXL_BLUE)).strip()
        ours = str(data.get("ours_red", _DEFAULT_OURS)).strip()
        if not blue.startswith("#") or len(blue) not in (4, 5, 7, 9):
            blue = _DEFAULT_CXL_BLUE
        if not ours.startswith("#") or len(ours) not in (4, 5, 7, 9):
            ours = _DEFAULT_OURS
        return blue, ours
    except (OSError, json.JSONDecodeError, TypeError):
        return _DEFAULT_CXL_BLUE, _DEFAULT_OURS


_COLOR_CXL, _COLOR_OURS = _load_custom_rgb_hex()
_COLOR_DRAM = "black"


def memory_line_palette():
    """(color, marker) per series: CXL=eval/memory_custom_colors.json cxl_blue, ours=ours_red, DRAM=black."""
    return {
        "cxl": (_COLOR_CXL, "o"),
        "cxl_alt": (_COLOR_OURS, "s"),
        "dram": (_COLOR_DRAM, "^"),
    }


def rpmalloc_tab20_palette():
    """Alias for callers; palette is no longer tab20 (kept name for import stability)."""
    return memory_line_palette()


def apply_rpmalloc_rcparams():
    plt.rcdefaults()
    plt.rcParams.update({"font.size": 18, "figure.figsize": (4, 2.5)})


def plot_series(ax, xs, ys, label: str, color, marker: str):
    ax.plot(
        xs,
        ys,
        marker=marker,
        color=color,
        label=label,
        markersize=4,
        linewidth=2,
    )


def plot_series_yerr(
    ax,
    xs,
    ys,
    yerr,
    label: str,
    color,
    marker: str,
    *,
    capsize: float = 2.5,
):
    """Line + markers with symmetric error bars (same visual weight as plot_series)."""
    ax.errorbar(
        xs,
        ys,
        yerr=yerr,
        marker=marker,
        color=color,
        ecolor=color,
        linestyle="-",
        linewidth=2,
        markersize=4,
        capsize=capsize,
        capthick=1,
        elinewidth=1,
        label=label,
    )


def finish_axes_like_rpmalloc(ax, ylabel: str):
    ax.set_xlabel("#Threads")
    ax.set_ylabel(ylabel)
    ax.set_ylim(bottom=0)
    ax.grid(True, which="both", axis="y", linestyle=":")
    ax.xaxis.set_major_locator(plt.MaxNLocator(integer=True))
    ax.legend(
        ncol=1,
        fontsize=16,
        loc="upper left",
        frameon=False,
        columnspacing=0.1,
        borderaxespad=0,
    )
