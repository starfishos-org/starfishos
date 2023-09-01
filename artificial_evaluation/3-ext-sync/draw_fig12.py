import os, sys
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator, FormatStrFormatter, MaxNLocator
import seaborn as sns

my_palette = sns.color_palette("RdGy",4)
four_colors = [my_palette[i] for i in range(len(my_palette))]

csv_file1 = sys.argv[1]
df1 = pd.read_csv(csv_file1, index_col=0)
df1 = df1.transpose()

csv_file2 = sys.argv[2]
df2 = pd.read_csv(csv_file2, index_col=0)
df2 = df2.transpose()
print(df1)
print(df2)

freq_set = [1, 5, 10]

def draw_thp(thp, title, ofn, show_label=False):
    x_list = freq_set

    plt.rcdefaults()
    plt.rcParams.update({'font.size': 20, 'figure.figsize': (4,4)})
    plt.figure()

    bench_list = [
        ('THP', 'TreeSLS', four_colors[0], '-', 'x'),
        ('THP-ext', 'TreeSLS-ExtSync', four_colors[1], '-', 'O'),
    ]
    
    for (tar, label, color, linestyle, marker) in bench_list:
        width = 0.4
        x = np.arange(0, 3)
        plt.bar(x - width/2, df1[thp][1:]/1000, label=label, color=color, width=width, edgecolor='black')
        plt.axhline(y=df1[thp][0]/1000, xmin=0, xmax=x_list[-1], ls='--', c='black', label='Baseline')
        plt.bar(x + width/2, df2[thp][1:]/1000, label=label, color=color, width=width, edgecolor='black')
    
        # plt.legend(fontsize=18, frameon=False, bbox_to_anchor=(1, -0.18), ncol=3)

    plt.xticks(np.arange(0, 3), ['1', '5', '10'])
    plt.xlabel('Checkpoint Interval (ms)')
    plt.ylabel('Throughput (Kops/s)')

    plt.grid(True, axis='y', linestyle=':')

    plt.tight_layout()

    plt.savefig(ofn, dpi=1200, format='jpg', bbox_inches='tight')
    # plt.show()

def draw_lat(lat, title, ofn, show_label=False):
    x_list = freq_set

    plt.rcdefaults()
    plt.rcParams.update({'font.size': 20, 'figure.figsize': (4,4)})
    plt.figure()

    bench_list = [
        (lat, 'TreeSLS', four_colors[0], '-', 'x'),
        (lat+'-ext', 'TreeSLS-ExtSync', four_colors[1], '-', 'o'),
    ]

    for (tar, label, _color, linestyle, marker) in bench_list:
        width = 0.4
        x = np.arange(0, 3)

        plt.bar(x - width/2, df1[lat][1:], label=label, color=_color, width=width, edgecolor='black')
        plt.axhline(y=df1[lat][0], xmin=0, xmax=x_list[-1], ls='--', c='black', label='Baseline')
        plt.bar(x + width/2, df2[lat][1:], label=label, color=_color, width=width, edgecolor='black')

    plt.xticks(np.arange(0, 3), [1, 5, 10])
    plt.xlabel('Checkpoint Interval (ms)')
    plt.ylim(ymin=0)
    plt.ylabel('Latency (ms)')

    plt.grid(True, axis='y', linestyle=':')

    plt.tight_layout()
    plt.savefig(ofn, dpi=1200, format='jpg', bbox_inches='tight')
    # plt.show()

def draw_legend(ofn):

    # create the figure and subplots
    fig, ax = plt.subplots()
    
    bench_list = [
        # ('P50', 'P50', four_colors[0], '-', 'x'),
        ('P95', 'TreeSLS', four_colors[0], '-', 'x'),
        # ('P50-ext', 'P50-ext', four_colors[1], '-', 'o'),
        ('P95-ext', 'TreeSLS-ExtSync', four_colors[1], '-', 'o'),
    ]

    for (tar, label, _color, linestyle, marker) in bench_list:
        # plot the data on the ax
        ax.bar(0, 0, color=_color, width=0, edgecolor='black', label=label)
    ax.axhline(y=0, xmin=0, xmax=0, ls='--', c='black', label='Baseline')
    # add the legend outside the plot area
    legend = ax.legend(bbox_to_anchor=(1.05, -0.18), fontsize=12, frameon=False, ncol=3)

    legend_fig = legend.figure
    legend_fig.canvas.draw()
    bbox = legend.get_window_extent().transformed(legend_fig.dpi_scale_trans.inverted())
    legend_fig.savefig(ofn, format='jpg', dpi=1000, bbox_inches=bbox)


if __name__ == "__main__":
    draw_lat('P50', 'Latency', "./result/fig12a.jpg")
    draw_thp('Throughput', "Throughput", "./result/fig12b.jpg")
    # draw_legend("./result/legend.jpg")
