
import sys
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from brokenaxes import brokenaxes
from matplotlib.axis import Axis
import seaborn as sns

csv_file=sys.argv[1]

# Create a DataFrame from the CSV file
df = pd.read_csv(csv_file, index_col=0)
# df = df.transpose()
print(df)

# Create the bar chart
# colors = ['#BCCCA3', '#0072BD', '#8682BD', '#D96A73', '#FABC55', 'grey']
my_palette = sns.color_palette("RdGy",6)
colors = [my_palette[i] for i in range(len(my_palette))]

# x = df.transpose().columns
# print(x)

def draw_fig(col, name):     
    # create three bar charts for the data
    # for i, col in enumerate(df.columns):
    # print(col, df[col])
    plt.figure() 
    plt.rcdefaults()
    plt.rcParams.update({'font.size': 22, 'figure.figsize': (4, 4)})

    x = np.arange(len(df))
    y = df[col]
    # print(x, y)
    if col == 'Throughput':
        y = y/1000
        # plt.xticks([0.25, 2], ["TreeSLS", "Aurora"])
        plt.bar(x, y, color=colors, edgecolor='black')
        plt.ylabel("Throughput (Kops/s)")
    else:
        plt.bar(x, y, color=colors, edgecolor='black')
        plt.ylabel("Latency (ms)")


    # plt.xticks(rotation=30)
    # display the chartsa
    plt.grid(True, axis='y', linestyle=':')
    plt.tight_layout()
    plt.savefig('./result/fig13{}.jpg'.format(name), format='jpg', dpi=1000)
    # plt.show()

def draw_legend(col):
    y = df[col]
    x = np.arange(len(df))

    # create the figure and subplots
    fig, ax = plt.subplots()

    # plot the data on the ax
    ax.bar(x, y, color=colors, width=0, edgecolor='black', label=df.index)

    # add the legend outside the plot area
    legend = ax.legend(loc='upper left', fontsize=12, frameon=False, ncol=1)

    legend_fig = legend.figure
    legend_fig.canvas.draw()
    bbox = legend.get_window_extent().transformed(legend_fig.dpi_scale_trans.inverted())
    legend_fig.savefig('./result/fig13d.jpg', format='jpg', dpi=1000, bbox_inches=bbox)


# plt.legend(fontsize=11, frameon=False, ncol=1)
# plt.legend(loc='upper left')
if __name__ == "__main__":
    draw_fig('Throughput', 'a')
    draw_fig('P50', 'b')
    draw_fig('P99', 'c')
    draw_legend('P99')