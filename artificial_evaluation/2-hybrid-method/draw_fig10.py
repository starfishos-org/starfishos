import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import sys
import seaborn as sns

my_palette = sns.color_palette("RdGy",5)
colors = [my_palette[i] for i in range(len(my_palette))]

# Create a DataFrame from the CSV file
# df = pd.read_csv('result-moti.csv', index_col=0)
df = pd.read_csv(sys.argv[1], index_col=0)
# df = df.transpose()
# print(df)

plt.rcdefaults()
plt.rcParams.update({'font.size': 22, 'figure.figsize': (8, 4)})

# Normalize the DataFrame
df_norm = df.divide(df['base (no checkpoint)'], axis=0)

# Select the columns to plot
cols_to_plot = ['base (no checkpoint)', '+ checkpoint', '+ page fault', '+ page memcpy', '+ hybrid copy']
df_plot = df_norm[cols_to_plot]

# Create the bar chart
# colors = ['#E64B35','#4DBBD6','#00A086','#3D5488']
ax = df_plot.plot(kind='bar', color=colors, stacked=False, width=0.8, edgecolor='black')

# Configure the chart
ax.set_ylabel('Normalized Run Time')
# ax.set_title('Benchmark Results')
ax.set_xticklabels(df.index, rotation=0)

# plt.yticks(np.arange(0, 16, 2))
plt.grid(True, axis='y', linestyle=':')
plt.legend(fontsize=18, frameon=False, loc='upper right')
plt.tight_layout()
# plt.show()
plt.savefig('./result/fig10.jpg', format='jpg', dpi=1000)
