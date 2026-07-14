"""Paper Figure 13 state-partition bars (from p3os-paper/eval/state_partition.py).

Drawing logic is unchanged; only CSV / output paths are parameterized for AE.
"""
from __future__ import annotations

from pathlib import Path

import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from config import draw_fig


def draw_state_partition(
    csv_path: str | Path = "state_partition.csv",
    out_dir: str | Path | None = None,
):
    colormap = plt.colormaps['tab20c']

    colors = [colormap(0), colormap(1), colormap(2), colormap(3)]

    benchs = [
        "leveldb",
        "dbx1000",
        "pca",
        "matrix_multiply",
        "linear_regression",
        "word_count",
    ]

    configs = [
        "All_CXL",
        "Kernel_DRAM_User_CXL",
        "Kernel_Page_CXL_Other_DRAM",
        "All_DRAM",
    ]

    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({'font.size': 26, 'figure.figsize': (13, 4.8)})
    plt.figure()
    # DBx1000: K-mix/U-mix vs K-mix/U-share throughput ratio = P3OS-mixed@2 / P3OS-all_cxl@2 (db1000-p3os-tigon.csv).
    df = pd.read_csv(csv_path)

    # Convert DataFrame to dict with configs as keys
    data = df.to_dict(orient='dict')

    data = {k: {configs[i]: v[i] for i in range(len(v))} for k, v in data.items()}

    # new name for benchmarks
    data_map = {
        "leveldb": "LevelDB",
        "dbx1000": "DBx1000",
        "pca": "PCA",
        "matrix_multiply": "Matrix Mult.",
        "linear_regression": "Linear Reg.",
        "word_count": "Word Count",
    }

    config_map = {
        "All_CXL": "Share",
        "Kernel_DRAM_User_CXL": "K-mix/U-share",
        "Kernel_Page_CXL_Other_DRAM": "K-mix/U-mix",
        "All_DRAM": "Private",
    }

    # Normalize to All_DRAM (Private): Private = 1.0, higher bar = closer to ideal.
    # Throughput: value / private_throughput. Exec time: private_time / value
    # (same as (1/time) / (1/time_private)).
    normalized_data = {bench: {config: 0.0 for config in configs} for bench in benchs}

    throughput_benchs = {"leveldb", "dbx1000"}

    for bench in benchs:
        if "All_DRAM" not in data.get(bench, {}) or data[bench]["All_DRAM"] == 0.0:
            print(f"Warning: Cannot normalize {bench} - All_DRAM data is missing or zero")
            continue

        baseline = data[bench]["All_DRAM"]

        for config in configs:
            if data[bench][config] == 0.0:
                normalized_data[bench][config] = 0.0
            else:
                if bench in throughput_benchs:
                    normalized_data[bench][config] = data[bench][config] / baseline
                else:
                    normalized_data[bench][config] = baseline / data[bench][config]

    # Get the number of benchmarks and configurations
    n_benchs = len(benchs)
    n_configs = len(configs)
    width = 0.8 / n_configs  # Width of each bar

    # Set up the x positions for the bars
    x = np.arange(n_benchs)

    # Plot bars for each configuration
    for i, config in enumerate(configs):
        values = [normalized_data[bench][config] for bench in benchs]
        plt.bar(x + (i - n_configs/2 + 0.5) * width, values, width, label=config_map[config], color=colors[i], edgecolor='black')

    # Add labels, title and legend
    # plt.xlabel('Benchmarks')
    plt.ylabel('Norm. Perf.')
    plt.yticks([0, 0.25, 0.5, 0.75, 1.0])
    plt.ylim(0, 1.05)
    plt.grid(axis='y', linestyle='--', linewidth=0.8, alpha=0.5)
    plt.gca().set_axisbelow(True)
    # plt.title('Performance Comparison Across Different Configurations')
    plt.xticks(x, [data_map[bench] for bench in benchs], rotation=15, ha='center')
    plt.legend(frameon=False, fontsize=26, loc='upper center', ncol=4, columnspacing=0.6, handletextpad=0.3, labelspacing=1.5, bbox_to_anchor=(0.49, 1.3) )

    # Adjust layout and save the figure
    plt.tight_layout()
    # plt.show()
    draw_fig("state_partition", out_dir=out_dir)
    # AE also gathers the historical fig13 name.
    if out_dir is not None:
        out = Path(out_dir)
        for ext in (".eps", ".pdf"):
            src = out / f"state_partition{ext}"
            dst = out / f"fig13-state-partition{ext}"
            if src.is_file():
                dst.write_bytes(src.read_bytes())


if __name__ == "__main__":
    draw_state_partition()
