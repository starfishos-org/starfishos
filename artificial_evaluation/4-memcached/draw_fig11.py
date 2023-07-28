import os, sys
import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator, FormatStrFormatter, MaxNLocator
import seaborn as sns

my_palette = sns.color_palette("RdGy",4)
four_colors = [my_palette[i] for i in range(len(my_palette))]

freq_set = [1, 5, 10, 50]

def draw(csv_file, title, ofn, show_label=False):
    x_list = freq_set
    df = pd.read_csv(csv_file, index_col=0)
    df = df.transpose()

    plt.rcdefaults()
    plt.rcParams.update({'font.size': 24, 'figure.figsize': (5, 4)})
    plt.figure()

    bench_list = [
        ('P50', 'P50-TreeSLS', four_colors[0], '-', 'x'),
        ('P95', 'P95-TreeSLS', four_colors[3], '-', 'x'),
        # ('P99', 'TreeSLS-P99', '#d13c74', '-', 'o')
    ]

    for (tar, label, color, linestyle, marker) in bench_list:
        # print(df[tar][1:])
        plt.plot(x_list, df[tar][1:], label=label, c=color, ls=linestyle, marker=marker, markersize=10, markeredgewidth=2, linewidth=2)
        
        plt.axhline(y=df[tar][0], xmin=0, xmax=x_list[-1], ls='--', c=color, linewidth=2, label=tar+'-baseline')

        if show_label:
            plt.legend(frameon=False, fontsize=22)

    plt.xticks([1, 5, 10, 50], [1, 5, 10, 50])
    plt.xlabel('Checkpoint Interval (ms)')
    plt.ylim(ymin=0)
    plt.ylabel('Latency (us)')

    plt.grid(True, 'major', linestyle=':')

    plt.tight_layout()
    plt.savefig(ofn, dpi=1200, format='jpg', bbox_inches='tight')
    # plt.show()

if __name__ == "__main__":
    draw(sys.argv[1]+'memcached-GET.csv', 'GET', "./result/fig11b.jpg", True)
    draw(sys.argv[1]+'memcached-SET.csv', 'SET', "./result/fig11a.jpg", True)
