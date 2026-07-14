"""Paper auto-scale legend (from p3os-paper/eval/auto_scale_apps_legend.py)."""
from __future__ import annotations

from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D


def main(out_dir: str | Path | None = None):
    out = Path(out_dir) if out_dir is not None else Path(".")
    out.mkdir(parents=True, exist_ok=True)

    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True

    handles = [
        Line2D([0], [0], color="#d62728", marker="x", linestyle="-", linewidth=2.2, markersize=8, markeredgewidth=2),
        Line2D([0], [0], color="#1f77b4", marker="s", linestyle="-.", linewidth=2.2, markersize=7, markeredgewidth=1.8),
        Line2D([0], [0], color="black", marker="o", linestyle="--", linewidth=2.2, markersize=6, markeredgewidth=1.5),
        Line2D([0], [0], color="#ff7f0e", marker="^", linestyle=":", linewidth=2.2, markersize=7, markeredgewidth=1.5),
        Line2D([0], [0], color="silver", marker="d", linestyle="--", linewidth=2.2, markersize=6, markeredgewidth=1.5),
    ]
    labels = [
        "Starfish Mixed",
        "Starfish CXL",
        "Distributed",
        "Tigon",
        "Ideal",
    ]

    fig = plt.figure(figsize=(8.4, 0.52))
    ax = fig.add_subplot(111)
    ax.axis("off")
    fig.legend(
        handles,
        labels,
        loc="center",
        bbox_to_anchor=(0.5, 0.5),
        ncol=5,
        frameon=False,
        fontsize=15,
        handlelength=1.15,
        columnspacing=0.75,
        handletextpad=0.3,
    )
    ax.set_position([0, 0, 1, 1])
    fig.tight_layout(pad=0)
    fig.savefig(out / "auto-scale-legend.pdf", dpi=1200, format="pdf", bbox_inches="tight", pad_inches=0.001)
    fig.savefig(out / "auto-scale-legend.eps", dpi=1200, format="eps", bbox_inches="tight", pad_inches=0.001)
    plt.close(fig)


if __name__ == "__main__":
    main()
