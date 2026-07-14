#!/usr/bin/env python3
"""
Draw a combined IPC CDF figure from the latest logs.

- Left panel: Empty IPC (remote + local 1-thread from 20260330_latest). MSI overlay is commented out in ``main()`` (see ``load_msi_cdf_from_ns_log``).
- Right panel: Read 4KiB (``cross`` / ``cross_4t`` / ``direct`` from 20260330_latest machine logs)

Output:
- eval/ipc_cdf/local_ipc_cdf.pdf
"""

from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import FormatStrFormatter

# Log-axis helpers (kept for easy re-enable; see ``finish_axes``).
# import numpy as np
# from matplotlib.ticker import FuncFormatter, LogLocator

_DIR = Path(__file__).resolve().parent
_EVAL_ROOT = _DIR.parent
if str(_EVAL_ROOT) not in sys.path:
    sys.path.insert(0, str(_EVAL_ROOT))

from memory_fig_style import apply_rpmalloc_rcparams, rpmalloc_tab20_palette  # noqa: E402

LOCAL_LOG_EMPTY = _DIR / "20260330_latest" / "machine0.log"
REMOTE_LOG_EMPTY = _DIR / "20260330_latest" / "machine1.log"
MSI_INTERRUPT_LOG = _EVAL_ROOT / "polling_srv" / "msi_interrupt.log"
OUTPUT_PDF = _DIR / "local_ipc_cdf.pdf"


def parse_cdf_from_log(logfile: Path) -> dict[str, list[tuple[int, int]]]:
    cdf_data: dict[str, list[tuple[int, int]]] = defaultdict(list)
    current_mode: str | None = None

    with logfile.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\[CDF_BEGIN\]\s+mode=(\S+)\s+count=(\d+)", line)
            if m:
                current_mode = m.group(1)
                continue

            if current_mode:
                m = re.match(r"\[CDF\]\s+(\d+)\s+(\d+)", line)
                if m:
                    cdf_data[current_mode].append((int(m.group(1)), int(m.group(2))))
                if "[CDF_END]" in line:
                    current_mode = None

    return cdf_data


def extract_cpu_freq_hz(logfile: Path) -> float:
    pattern = re.compile(r"cpu frequency=\(Dec\)(\d+)")
    with logfile.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pattern.search(line)
            if m:
                return float(m.group(1))
    raise RuntimeError(f"cpu frequency not found in {logfile}")


def parse_cdf_cycles_to_ns(logfile: Path) -> dict[str, list[tuple[int, int]]]:
    cyc_to_ns = 1e9 / extract_cpu_freq_hz(logfile)
    cdf_cycles = parse_cdf_from_log(logfile)
    return {
        mode: [(idx, int(round(latency * cyc_to_ns))) for idx, latency in rows]
        for mode, rows in cdf_cycles.items()
    }


def load_msi_cdf_from_ns_log(log_path: Path) -> list[tuple[int, int]] | None:
    """One sample per line, latency in ns (see eval/polling_srv/msi_interrupt.log)."""
    if not log_path.is_file():
        return None
    samples: list[int] = []
    with log_path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                samples.append(int(float(line)))
            except ValueError:
                continue
    if not samples:
        return None
    samples.sort()
    return [(i + 1, latency_ns) for i, latency_ns in enumerate(samples)]


def latency_us_cdf_points(data: list[tuple[int, int]]) -> tuple[list[float], list[float]]:
    if not data:
        return [], []
    data = sorted(data, key=lambda t: t[0])
    n = len(data)
    xs = [latency_ns / 1000.0 for _idx, latency_ns in data]
    ys = [(i + 1) / n for i in range(n)]
    return xs, ys


def latency_us_at_cdf(data: list[tuple[int, int]], quantile: float) -> float | None:
    xs, ys = latency_us_cdf_points(data)
    if not xs:
        return None
    q = min(max(quantile, ys[0]), ys[-1])
    for i, y in enumerate(ys):
        if y >= q:
            if i == 0:
                return float(xs[0])
            x0, x1 = xs[i - 1], xs[i]
            y0, y1 = ys[i - 1], ys[i]
            if y1 <= y0:
                return float(x1)
            t = (q - y0) / (y1 - y0)
            return float(x0 + t * (x1 - x0))
    return float(xs[-1])


# def _format_latency_us_tick(x: float, _pos: float | None) -> str:
#     """Readable µs labels on a log x-axis."""
#     if not np.isfinite(x) or x <= 0:
#         return ""
#     if x >= 100:
#         return f"{x:.0f}"
#     if x >= 10:
#         return f"{x:.0f}"
#     if x >= 1:
#         s = f"{x:.1f}"
#         return s.rstrip("0").rstrip(".")
#     s = f"{x:.2f}"
#     return s.rstrip("0").rstrip(".")


def annotate_p50(ax: plt.Axes, data: list[tuple[int, int]], color: str, series_index: int) -> None:
    p50 = latency_us_at_cdf(data, 0.5)
    if p50 is None or p50 <= 0:
        return
    ax.vlines(
        p50,
        0,
        0.5,
        colors=color,
        linestyles=(0, (4, 3)),
        linewidth=1.0,
        alpha=0.9,
        zorder=1,
    )
    # Log-axis text placement (re-enable with ``finish_axes`` log x):
    # x_text = float(p50) * (1.14 ** (1.0 + 0.35 * series_index))
    x_text = float(p50) + 0.18
    ax.text(
        x_text,
        0.06 + series_index * 0.17,
        f"{p50:.1f}",
        ha="center",
        va="bottom",
        fontsize=21,
        color=color,
        zorder=10,
        bbox={
            "boxstyle": "round,pad=0.22",
            "facecolor": "white",
            "edgecolor": "none",
        },
    )


def finish_axes(ax: plt.Axes, *, ylabel: str = "CDF") -> None:
    ax.set_xlabel("Latency (µs)", fontsize=28)
    ax.set_ylabel(ylabel, fontsize=28)
    ax.set_ylim(0, 1)
    ax.tick_params(axis="both", labelsize=26)
    ax.xaxis.set_major_formatter(FormatStrFormatter("%.1f"))
    ax.yaxis.set_major_formatter(FormatStrFormatter("%.1f"))
    ax.grid(True, which="both", linestyle=":", alpha=0.85)
    ax.set_axisbelow(True)
    # Log x-axis (disabled). Re-enable: import np, FuncFormatter, LogLocator; define
    # ``_format_latency_us_tick`` above; then:
    # ax.set_xscale("log")
    # ax.xaxis.set_major_locator(LogLocator(base=10))
    # ax.xaxis.set_minor_locator(LogLocator(base=10, subs=tuple(range(2, 10))))
    # ax.xaxis.set_major_formatter(FuncFormatter(_format_latency_us_tick))
    # ax.xaxis.set_minor_formatter(FuncFormatter(_format_latency_us_tick))
    # ax.grid(True, which="major", linestyle=":", alpha=0.88)
    # ax.grid(True, which="minor", linestyle=":", alpha=0.38)


def plot_panel(
    ax: plt.Axes,
    title: str,
    remote: dict[str, list[tuple[int, int]]],
    local: dict[str, list[tuple[int, int]]],
    remote_modes: list[tuple[str, str]],
    local_series: list[tuple[str, str, str, str]],
    *,
    extra_series: list[tuple[list[tuple[int, int]], str, str, str]] | None = None,
) -> None:
    pal = rpmalloc_tab20_palette()
    series: list[tuple[dict[str, list[tuple[int, int]]], str, str, str, str]] = [
        (remote, remote_modes[0][0], remote_modes[0][1], pal["dram"][0], "-"),
        (remote, remote_modes[1][0], remote_modes[1][1], pal["cxl"][0], "-"),
    ]
    for mode, label, color, linestyle in local_series:
        series.append((local, mode, label, color, linestyle))

    p99_us: list[float] = []
    ann_idx = 0
    for source, mode, label, color, linestyle in series:
        if mode not in source:
            print(f"Warning: mode {mode} not found")
            continue
        xs, ys = latency_us_cdf_points(source[mode])
        if not xs:
            continue
        ax.plot(
            xs,
            ys,
            linewidth=2.3,
            color=color,
            label=label,
            linestyle=linestyle,
            zorder=2,
        )
        annotate_p50(ax, source[mode], color, ann_idx)
        ann_idx += 1
        p99 = latency_us_at_cdf(source[mode], 0.99)
        if p99 is not None:
            p99_us.append(p99)

    if extra_series:
        for cdf_rows, _label, color, linestyle in extra_series:
            xs, ys = latency_us_cdf_points(cdf_rows)
            if not xs:
                continue
            ax.plot(
                xs,
                ys,
                linewidth=2.3,
                color=color,
                label=_label,
                linestyle=linestyle,
                zorder=2,
            )
            annotate_p50(ax, cdf_rows, color, ann_idx)
            ann_idx += 1
            p99 = latency_us_at_cdf(cdf_rows, 0.99)
            if p99 is not None:
                p99_us.append(p99)

    if p99_us:
        ax.set_xlim(0, max(p99_us) * 1.02)

    ax.set_title(title, fontsize=27, pad=6)
    finish_axes(ax)


def main() -> None:
    required = [
        LOCAL_LOG_EMPTY,
        REMOTE_LOG_EMPTY,
    ]
    if any(not p.is_file() for p in required):
        raise FileNotFoundError("Missing IPC CDF logs (see plot_cdf_all.py paths)")

    print(f"Using local log (empty + read, 1t): {LOCAL_LOG_EMPTY}")
    local_cdf_empty = parse_cdf_cycles_to_ns(LOCAL_LOG_EMPTY)

    print(f"Using remote log (empty + read): {REMOTE_LOG_EMPTY}")
    remote_cdf_empty = parse_cdf_cycles_to_ns(REMOTE_LOG_EMPTY)

    # --- MSI (temporarily off): uncomment to overlay MSI on Empty IPC + legend entry "MSI".
    # msi_cdf = load_msi_cdf_from_ns_log(MSI_INTERRUPT_LOG)
    # if msi_cdf is None:
    #     print(
    #         f"Note: no MSI CDF ({MSI_INTERRUPT_LOG} missing or empty) — Empty IPC panel has no MSI curve"
    #     )
    # else:
    #     print(f"MSI CDF: {len(msi_cdf)} samples from {MSI_INTERRUPT_LOG}")

    for name, d in [
        ("local (machine0)", local_cdf_empty),
        ("remote (machine1)", remote_cdf_empty),
    ]:
        print(f"{name} modes:")
        for mode in sorted(d):
            print(f"  {mode}: {len(d[mode])} samples")

    apply_rpmalloc_rcparams()

    fig = plt.figure(figsize=(8.4, 4.0))
    gs = fig.add_gridspec(2, 2, height_ratios=[0.34, 1.0], hspace=0.22, wspace=0.32)
    ax_legend = fig.add_subplot(gs[0, :])
    ax_empty = fig.add_subplot(gs[1, 0])
    ax_read = fig.add_subplot(gs[1, 1])

    pal = rpmalloc_tab20_palette()
    handles = [
        Line2D([0], [0], color="#2ca02c", linewidth=1.4, linestyle="--"),
        Line2D([0], [0], color=pal["dram"][0], linewidth=1.4, linestyle="-"),
        Line2D([0], [0], color=pal["cxl"][0], linewidth=1.4, linestyle="-"),
    ]
    labels = [
        "Local",
        "Remote",
        "Remote-Conc.",
    ]
    # if msi_cdf is not None:
    #     handles.append(Line2D([0], [0], color="#d62728", linewidth=1.4, linestyle="-."))
    #     labels.append("MSI")
    ax_legend.axis("off")
    ax_legend.legend(
        handles,
        labels,
        loc=(-0.02, 0.02),
        ncol=len(handles),
        frameon=False,
        fontsize=26,
        handlelength=1.25,
        columnspacing=1.05,
        handletextpad=0.45,
    )

    plot_panel(
        ax_empty,
        "Empty IPC",
        remote_cdf_empty,
        local_cdf_empty,
        [("cross_empty", "Remote IPC"), ("cross_empty_4t", "Remote IPC (Conc.)")],
        [
            ("direct_empty", "Local", "#2ca02c", "--"),
        ],
        # extra_series=(
        #     [(msi_cdf, "MSI", "#d62728", "-.")] if msi_cdf is not None else None
        # ),
    )
    plot_panel(
        ax_read,
        r"Read 4KiB",
        remote_cdf_empty,
        local_cdf_empty,
        [("cross", "Remote IPC"), ("cross_4t", "Remote IPC (Conc.)")],
        [
            ("direct", "Local", "#2ca02c", "--"),
        ],
    )
    ax_read.set_ylabel("")
    ax_read.tick_params(axis="y", left=False, labelleft=False)

    fig.subplots_adjust(left=0.10, right=0.99, top=0.96, bottom=0.14)
    fig.savefig(OUTPUT_PDF, dpi=300, format="pdf", bbox_inches="tight")
    print(f"Saved: {OUTPUT_PDF}")
    plt.close(fig)


if __name__ == "__main__":
    main()
