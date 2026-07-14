import matplotlib.pyplot as plt
import pandas as pd
import numpy as np
from matplotlib.patches import Patch
from config import SYSTEM_NAME, draw_fig

def draw_real():

    tab20 = plt.colormaps['tab20']

    # Define hatch patterns for different groups
    hatch_patterns = ['+', 'x', 'o', 'O', '.', '*']
    # StarfishOS cross-machine bars: use a hatch pattern
    starfish_hatch = '\\\\'

    def lighten(color, amount: float = 0.35):
        """Mix color with white to make blues look like a light-blue.

        amount in [0, 1]: 0 keeps the original color; 1 makes it pure white.
        """
        r, g, b, *rest = color
        r2 = r + (1 - r) * amount
        g2 = g + (1 - g) * amount
        b2 = b + (1 - b) * amount
        a = rest[0] if rest else 1.0
        return (r2, g2, b2, a)

    # Keep the same palette layout, but make both blue entries identical.
    # In this figure, tab20(0)/tab20(1) correspond to the two blue colors
    # used by (Exclusive / Co-located). User request: unify them.
    blue_color = lighten(tab20(0))
    colors = [blue_color, blue_color, tab20(6), tab20(7)]

    # five_colors = [colormap(i) for i in range(5)]

    reversed_items = ['matrix', 'word-count', 'pca', 'linear-regression', 'gemini', 'kmeans', 'string-match', 'cnn']

    sorted_items = [

    'dbx1000',
    'matrix',
        'leveldb',


    'word-count',
    'linear-regression',
        'redis',
    
    'string-match',
    'pca',
    'memcached',

    'gemini',
    'cnn',
        'kmeans',
    ]

    dram_groups = [
    ['leveldb', 'matrix'],
    ['dbx1000', 'word-count'],
    ['linear-regression', 'redis'],
    ['memcached', 'pca'],
    ['string-match', 'gemini'],
    ['cnn', 'kmeans'],
    ]

    p3os_cross_items = ['cnn', 'linear-regression', 'matrix', 'pca']

    label_map = {
    'leveldb': 'LevelDB',
    'dbx1000': 'DBx1000',
    'linear-regression': 'Linear Reg.',
    'matrix': 'Matrix Mult.',
    'redis': 'Redis',
    'pca': 'PCA',
    'word-count': 'Word Count',
    'memcached': 'Memcached',
    'gemini': 'GeminiGraph',
    'string-match': 'String Match',
    'kmeans': 'KMeans',
    'cnn': 'CNN',
    }

    # Read the CSV file
    data = pd.read_csv('./real.csv', header=None, names=['benchmark', 'value'])

    # Split the benchmark names and types
    data[['name', 'type']] = data['benchmark'].str.rsplit('-', n=1, expand=True)

    # Pivot the data for easier plotting
    plot_data = data.pivot(index='name', columns='type', values='value')

    # Normalize the data (divide each value by the corresponding 'single' value)
    normalized_data = plot_data.copy()

    normalized_data['p3os'] = normalized_data['p3os'] / normalized_data['single']
    normalized_data['stress'] = normalized_data['stress'] / normalized_data['single']
    normalized_data['single'] = normalized_data['single'] / normalized_data['single']  # All 1's

    # For reversed_items, take the reciprocal of the normalized values
    # This is used when lower values are better (e.g., execution time)
    for item in reversed_items:
        if item in normalized_data.index:
            normalized_data.loc[item, 'stress'] = 1 / normalized_data.loc[item, 'stress']
            normalized_data.loc[item, 'p3os'] = 1 / normalized_data.loc[item, 'p3os']

    # Cap values at 1
    normalized_data[normalized_data > 1.1] = 1.01

    # Set dbx1000 values to 1
    dbx1000_items = ['dbx1000', 'word-count', 'string-match', 'gemini']
    for item in dbx1000_items:
        if item in normalized_data.index:
            normalized_data.loc[item, 'stress'] = 1.0

    # Create a new order based on dram_groups
    # new_order = []
    # for group in dram_groups:
    #     new_order.extend(group)
    new_order = sorted_items

    # Reorder the data according to dram_groups
    normalized_data = normalized_data.reindex(new_order)

    # Set up the plot style
    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    plt.rcParams.update({'font.size': 22, 'figure.figsize': (11, 3.5)})

    # Create figure and axis
    fig, ax = plt.subplots()

    # Set the width of each bar and positions of the bars
    width = 0.38  # reduced from 0.15 to 0.12
    x = np.arange(len(normalized_data.index))  # add 0.8 scale factor to reduce spacing

    # Create bars with hatch patterns
    # single_bars = ax.bar(x - width, normalized_data['single'], width, label='Single', 
    #                     color=colors[0], edgecolor='black')

    # Add hatch patterns for stress bars based on dram_groups
    stress_hatches = {}
    for i, group in enumerate(dram_groups):
        for item in group:
            stress_hatches[item] = hatch_patterns[i]

    stress_bars = ax.bar(x - width/2, normalized_data['stress'], width, label='Stress', 
                        color=colors[1], edgecolor='black')
    for i, item in enumerate(normalized_data.index):
        if item in stress_hatches and i % 3 != 0:
            stress_bars[i].set_hatch(stress_hatches[item])
        # Set deep blue color for dbx1000
        if item in dbx1000_items:
            stress_bars[i].set_facecolor(colors[0])

    # Create StarfishOS bars (only cross-machine are hatched)
    for i, item in enumerate(normalized_data.index):
        bar = ax.bar(
            x[i] + width/2,
            normalized_data['p3os'].iloc[i],
            width,
            color=colors[3],  # Same facecolor for Starfish and Starfish-Cross
            edgecolor='black'
        )
        if item in p3os_cross_items:
            bar[0].set_hatch(starfish_hatch)

    # Add percentage labels above bars
    for i, (stress_val, p3os_val) in enumerate(zip(normalized_data['stress'], normalized_data['p3os'])):
        # Calculate improvement percentages
        stress_improvement = (stress_val - 1) * 100
        p3os_improvement = (p3os_val - 1) * 100
        
        # Add labels above bars
        ax.text(x[i] - width * 0.6, stress_val + 0.05, f'{int(stress_improvement)}%', 
                ha='center', va='bottom', fontsize=20, rotation=90)
        ax.text(x[i] + width * 0.6, p3os_val + 0.05, f'{int(p3os_improvement)}%', 
                ha='center', va='bottom', fontsize=20, rotation=90)

    # Customize the plot
    ax.set_ylabel('Normalized Perf')
    ax.set_xticks(x)
    ax.set_xticklabels([label_map[item] for item in normalized_data.index], rotation=90, ha='center', fontsize=20)

    # Set y-axis limits to leave space for labels
    ax.set_ylim(0, 1.3)

    # Legend: unify traditional (blue) and starfish (red) labels.
    # User request: do not add a separate hatch legend item for Starfish-Cross.
    legend_handles = [
        Patch(facecolor=colors[0], edgecolor='black', label='Traditional'),
        Patch(facecolor=colors[3], edgecolor='black', label='StarfishOS'),
    ]

    # Move legend outside the plot to the middle right
    ax.legend(handles=legend_handles, frameon=False, fontsize=20, bbox_to_anchor=(0.5, 1.15), loc='center', ncol=len(legend_handles), columnspacing=0.5)

    # Add grid
    ax.grid(True, which='both', axis='y', linestyle=':')

    # Add vertical lines to separate groups of 3 items
    for i in range(2, len(x), 3):  # Start from index 2, every 3 items
        if i < len(x) - 1:
            ax.axvline(x=(x[i] + x[i+1]) / 2, color='black', linestyle='-', linewidth=1)

    # Adjust layout to prevent label cutoff and make room for legend
    # plt.subplots_adjust(bottom=0.2, top=0.85)

    # Save and show the plot
    draw_fig("real")

if __name__ == "__main__":
    draw_real()