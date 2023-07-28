import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import seaborn as sns
import sys

csv_file=sys.argv[1]

# Create a DataFrame from the CSV file
df = pd.read_csv(csv_file, index_col=0)
# df = df.transpose()
print(df)

plt.rcdefaults()
plt.rcParams.update({'font.size': 22, 'figure.figsize': (8, 4)})

# Normalize the DataFrame
# df_norm = df.divide(df['raw'], axis=0)

# Select the columns to plot
# cols_to_plot = ['chcore-baseline', 'chcore-1msckpt', 'linux-baseline', 'linux-NVM-WAL', 'linux-disk-WAL']
# cols_to_plot = ['TreeSLS-base','TreeSLS-1ms','Linux-base','Linux-WAL']
# df_plot = df[cols_to_plot]
df_plot = df[df.columns]

# Create the bar chart
# colors = ['#BCCCA3', '#0072BD', '#8682BD', '#D96A73', '#FABC55']
my_palette = sns.color_palette("RdGy",4)
colors = [my_palette[i] for i in range(len(my_palette))]

ax = df_plot.plot(kind='bar', stacked=False, color=colors, width=0.8, edgecolor='black')

# Configure the chart
ax.set_ylabel('Throughput (KTPS)')
# ax.set_title('Benchmark Results')
ax.set_xticklabels(df.index, rotation=0)

plt.grid(True, axis='y', linestyle=':')
plt.yticks(np.arange(0, 50, 10))
plt.legend(fontsize=18, frameon=False, bbox_to_anchor=(0.1, 1), ncol=2)
plt.tight_layout()
# plt.show()
plt.savefig(sys.argv[2], format='jpg', dpi=1000)
