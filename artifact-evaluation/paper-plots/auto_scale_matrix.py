"""Paper auto-scale matrix figure (from p3os-paper/eval/auto_scale_matrix.py).

Drawing logic unchanged; data / output paths parameterized for AE.
"""
from __future__ import annotations

import re
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator
from config import SYSTEM_NAME, draw_fig


def load_mapreduce_data(file_path):
  pattern = re.compile(r"RESULT:\s+N=(\d+)\s+CONFIG=(\w+)\s+TIME=(\d+)")
  data = {'MIXED': [], 'CXL': [], 'TCP': [], 'IDEAL': []}

  with open(file_path, 'r') as f:
    for line in f:
      match = pattern.search(line)
      if not match:
        continue

      machine_count = int(match.group(1))
      config = match.group(2)
      runtime = int(match.group(3))

      if config not in data:
        continue

      throughput = 100 * (1 / (runtime / 1000 / 1000))
      data[config].append((machine_count, throughput))

  for config in data:
    data[config].sort(key=lambda item: item[0])

  return data


def draw_auto_scale_matrix(
    data_path: str | Path = "./mapreduce/4000size.txt",
    out_dir: str | Path | None = None,
):
  data = load_mapreduce_data(data_path)

  plt.rcdefaults()
  plt.rcParams["ps.useafm"] = True
  plt.rcParams.update({'font.size': 22, 'figure.figsize': (3.8, 4.16)})

  fig, ax1 = plt.subplots()

  styles = {
      'MIXED': {'color': '#d62728', 'marker': 'x', 'linestyle': '-', 'label': f'{SYSTEM_NAME} Mixed'},
      'CXL': {'color': '#1f77b4', 'marker': 's', 'linestyle': '-.', 'label': f'{SYSTEM_NAME} CXL'},
      'TCP': {'color': 'black', 'marker': 'o', 'linestyle': '--', 'label': 'Distributed'},
      'IDEAL': {'color': 'silver', 'marker': 'd', 'linestyle': '--', 'label': 'Ideal'},
  }

  x_ticks = [1, 2, 4, 6, 8]
  for config in ['MIXED', 'CXL', 'TCP', 'IDEAL']:
    points = [item for item in data[config] if item[0] in x_ticks]
    if not points:
      continue
    x_values = [item[0] for item in points]
    y_values = [item[1] for item in points]
    style = styles[config]
    ax1.plot(
        x_values,
        y_values,
        label=style['label'],
        color=style['color'],
        linestyle=style['linestyle'],
        marker=style['marker'],
        markersize=8,
        markeredgewidth=2,
        linewidth=2.2,
    )

  ax1.set_ylabel('1/Run Time (1/s * 100)')
  ax1.set_xticks([1, 2, 4, 6, 8])
  ax1.set_xlabel('#Machines')
  ax1.set_ylim(0, 9)
  ax1.yaxis.set_major_locator(MultipleLocator(2))
  ax1.grid(True, which='both', axis='both', linestyle=':')

  fig.tight_layout()

  draw_fig("auto-scale-matrix", out_dir=out_dir)
  plt.close(fig)


if __name__ == "__main__":
  draw_auto_scale_matrix()
