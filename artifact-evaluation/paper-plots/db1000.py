"""Paper DBx1000 auto-scale figure (from p3os-paper/eval/db1000.py)."""
from __future__ import annotations

from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
from config import SYSTEM_NAME, draw_fig


def draw_db1000(
    csv_path: str | Path = "./db1000/db1000-p3os-tigon.csv",
    out_dir: str | Path | None = None,
):
    data = pd.read_csv(csv_path)
    data = data.sort_values(['module', 'machines'])

    plt.rcdefaults()
    plt.rcParams['ps.useafm'] = True
    plt.rcParams.update({'font.size': 22, 'figure.figsize': (3.8, 4.16)})

    fig, ax1 = plt.subplots()

    styles = {
        'P3OS-mixed': {'color': '#d62728', 'marker': 'x', 'linestyle': '-', 'label': SYSTEM_NAME + ' Mixed'},
        'P3OS-all_cxl': {'color': '#1f77b4', 'marker': 's', 'linestyle': '-.', 'label': SYSTEM_NAME + ' CXL'},
        'tigon': {'color': '#ff7f0e', 'marker': '^', 'linestyle': ':', 'label': 'Tigon'},
        'linux': {'color': 'silver', 'marker': 'd', 'linestyle': '--', 'label': 'Ideal'},
    }

    for module, group in data.groupby('module'):
        group = group.sort_values('machines')
        style = styles.get(module, {'color': '#1f77b4', 'marker': 's', 'linestyle': '-', 'label': str(module)})
        ax1.plot(
            group['machines'].values,
            group['performance_mops'].values,
            label=style['label'],
            color=style['color'],
            linestyle=style['linestyle'],
            marker=style['marker'],
            markersize=8,
            markeredgewidth=2,
            linewidth=2.2,
        )

    ax1.set_ylabel('Thp (Mops/s)')
    ax1.set_xlabel('#Machines')

    x_ticks = sorted(data['machines'].unique())
    ax1.set_xticks(x_ticks)
    ax1.set_ylim(0)

    ax1.grid(True, which='both', axis='both', linestyle=':')
    fig.tight_layout()

    draw_fig('db1000', out_dir=out_dir)
    plt.close(fig)


if __name__ == '__main__':
    draw_db1000()
