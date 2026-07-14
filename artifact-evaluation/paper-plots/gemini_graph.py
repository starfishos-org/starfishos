import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
from config import SYSTEM_NAME, draw_fig

def parse_time(s):
    """Parse time string like '95.100607s' to float"""
    return float(s.strip().replace('s', ''))

def draw_gemini_graph():
    # Read data.log (CSV format)
    data = []
    with open('./gemini_graph/data.log', 'r') as f:
        lines = f.readlines()
        for line in lines:
            if line.strip() and not line.startswith('PageRank') and not line.startswith('Graph:') and not line.startswith('Date:') and not line.startswith('machines'):
                parts = line.strip().split(',')
                if len(parts) >= 6:
                    machine = int(parts[0])
                    mixed_cxl = parse_time(parts[1])
                    cxl = parse_time(parts[2])
                    dram = parse_time(parts[3])
                    linux_dram = parse_time(parts[4])
                    distributed = parse_time(parts[5])
                    data.append({
                        'machine': machine,
                        'mixed_cxl': mixed_cxl,
                        'cxl': cxl,
                        'linux_dram': linux_dram,
                        'distributed': distributed,
                    })

    df = pd.DataFrame(data)

    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({'font.size': 22, 'figure.figsize': (3.8, 4.16)})
    plt.figure()

    fig, ax1 = plt.subplots()

    # Number of machines
    x = df['machine'].values

    # Convert to throughput (1/time × 100)
    y_mixed_cxl = 100 * (1 / df['mixed_cxl'].values)
    y_cxl = 100 * (1 / df['cxl'].values)
    y_linux_dram = 100 * (1 / df['linux_dram'].values)
    y_distributed = 100 * (1 / df['distributed'].values)

    # Plot MIXED - solid red line
    ax1.plot(
        x,
        y_mixed_cxl,
        label=f'{SYSTEM_NAME} Mixed',
        color='#d62728',
        linestyle='-',
        marker='x',
        markersize=8,
        markeredgewidth=2,
        linewidth=2.2,
    )

    # Plot CXL - blue dash-dot line
    ax1.plot(
        x,
        y_cxl,
        label=f'{SYSTEM_NAME} CXL',
        color='#1f77b4',
        linestyle='-.',
        marker='s',
        markersize=7,
        markeredgewidth=1.8,
        linewidth=2.2,
    )

    # Plot Linux - silver dashed line
    ax1.plot(
        x,
        y_linux_dram,
        label='Ideal',
        color='silver',
        linestyle='--',
        marker='d',
        markersize=6,
        markeredgewidth=1.5,
        linewidth=2.2,
    )

    # Plot Distributed - black dashed line
    ax1.plot(
        x,
        y_distributed,
        label='Distributed',
        color='black',
        linestyle='--',
        marker='o',
        markersize=6,
        markeredgewidth=1.5,
        linewidth=2.2,
    )

    ax1.set_ylabel('1/Run Time (1/s * 100)')

    ax1.set_xticks(x)
    ax1.set_xlabel('#Machines')

    ax1.grid(True, which='both', axis='both', linestyle=':')
    fig.tight_layout()

    draw_fig("gemini-chcore")

if __name__ == "__main__":
    draw_gemini_graph()