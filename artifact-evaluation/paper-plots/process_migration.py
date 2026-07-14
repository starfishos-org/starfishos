import matplotlib.pyplot as plt
import pandas as pd
import numpy as np
from config import draw_fig

def preprocess_data():
	data = pd.read_csv('./process-migration.csv')

	# Data preprocessing: combine related columns
	data['prepare-vmspace'] = data['copy_vmspace']
	data['prepare-cap-group'] = data['copy_cap_group']
	data['prepare-threads'] = data['copy_thread']
	data['prepare-other'] = (data['PREPARE'] - data['copy_vmspace'] - data['copy_cap_group'] - data['copy_thread'] + data['PREPARE_KVS'])
	data['stop-all-threads'] = data['CKPT_STOP_ALL_THREADS']
	data['ckpt-threads'] =  data['CKPT_THREADS']
	data['ckpt-other'] = data['CKPT_OTHER'] + data['CKPT_CAP_GROUP']
	data['restore-other'] = data['RESTORE'] + data['RESTORE_KVS']
	data['restore-threads'] = data['RESTORE_PROMOTE_THREADS']
	data['start-all-threads'] = data['RESTORE_START_ALL_THREADS']
	return data

def draw_process_migration_large(data, x, width, colors, stages):
    plt.rcParams.update({'figure.figsize': (10, 6)})
    plt.rcParams.update({'font.size': 38})
    
    fig, ax = plt.subplots(1, 1)
    
    large_stages = ['prepare-other', 'prepare-cap-group', 'prepare-threads', 'prepare-vmspace']
    bottom = np.zeros(len(data.index))
    
    # First compute raw totals to determine the cutoff
    original_bottom = np.zeros(len(data.index))
    for i, stage in enumerate(large_stages):
        values = np.array(data[stage] / 1000.0 / 1000.0)  # ensure numpy array
        original_bottom += values
    
    # Convert to numpy array and compute axis-break position
    original_bottom = np.array(original_bottom)
    if len(original_bottom) > 1:
        non_last_max = float(np.max(original_bottom[:-1]))
    else:
        non_last_max = float(original_bottom[0])
    
    cutoff = non_last_max * 1.2
    last_total = float(original_bottom[-1])  # should work correctly now
    
    # Draw bars; only truncate the top segment of the last bar
    for i, stage in enumerate(large_stages):
        values = np.array(data[stage] / 1000.0 / 1000.0)  # ensure numpy array
        
        # Truncate the top segment of the last bar
        if last_total > cutoff and i == len(large_stages) - 1:  # only handle the last stage (top segment)
            last_idx = len(data.index) - 1
            if last_idx < len(values):  # safety check
                # current bottom height
                current_bottom = float(bottom[last_idx])
                # if this segment would exceed cutoff, truncate it
                if current_bottom + values[last_idx] > cutoff:
                    # keep only the portion up to cutoff height
                    values[last_idx] = max(0, cutoff - current_bottom)
        
        ax.bar(x, values, width, bottom=bottom,
               color=colors[stages.index(stage)], edgecolor='black', label=stage)
        bottom += values
    
    # ensure bottom is a numpy array
    bottom = np.array(bottom)
    
    # Set Y-axis range and add axis-break markers
    if last_total > cutoff:
        ax.set_ylim(0, cutoff * 1.18)  # leave a little room for the axis-break symbol
        
        # Add axis-break symbols and value label on top of the last bar
        last_idx = len(x) - 1
        bar_top = cutoff  # bar height is now the cutoff
        
        # Add axis-break symbols (// style)
        break_y = bar_top
        break_width = width / 3
        # left slash
        ax.plot([x[last_idx] - break_width * 2, x[last_idx] + break_width * 2], 
               [(break_y * 0.95) - cutoff*0.03, (break_y * 0.95) + cutoff*0.03], 'k-', lw=3)
        # right slash
        ax.plot([x[last_idx] - break_width * 2, x[last_idx] + break_width * 2], 
               [(break_y * 0.92) - cutoff*0.03, (break_y * 0.92) + cutoff*0.03], 'k-', lw=3)
        
        # Annotate the true value above the axis-break symbol
        ax.text(x[last_idx], bar_top + cutoff*0.05, f"{last_total:.1f}", 
               ha='center', va='bottom', fontsize=32)
    
    ax.set_ylabel('Time (ms)', labelpad=50)
    # keep only ticks <= 30
    ticks = ax.get_yticks()          # get default ticks
    ax.set_yticks([t for t in ticks if t <= 20])  
    ax.set_xticks(x)
    ax.set_xticklabels(data.iloc[:, 0], rotation=30, ha='right')
    ax.grid(True, which='both', axis='y', linestyle=':')
    ax.legend(bbox_to_anchor=(1.02, 1), loc='upper left', frameon=False, fontsize=39)
    
    draw_fig("process-migration-data-large")
    plt.cla()

def draw_process_migration_large_v0(data, x, width, colors, stages):
	# Axis break: keep only the lower y-axis; upper shows the overflow of the last bar
	plt.rcParams.update({'figure.figsize': (10, 4)})
	plt.rcParams.update({'font.size': 38})
	fig, (ax_top, ax_bottom) = plt.subplots(2, 1, sharex=True, gridspec_kw={'height_ratios': [1, 3]})
	fig.subplots_adjust(hspace=0.05)

	large_stages = ['prepare-other', 'prepare-cap-group', 'prepare-threads', 'prepare-vmspace']
	bottom = np.zeros(len(data.index))
	for i, stage in enumerate(large_stages):
		values = data[stage] / 1000.0 / 1000.0
		ax_top.bar(x, values, width, bottom=bottom,
				color=colors[stages.index(stage)], edgecolor='black', label=stage)
		ax_bottom.bar(x, values, width, bottom=bottom,
				color=colors[stages.index(stage)], edgecolor='black', label=stage)
		bottom += values

	# Dynamic break position: lower axis near other bars; upper compresses the overflow
	non_last_max = float(np.max(bottom[:-1])) if len(bottom) > 1 else float(bottom[0])
	cutoff = non_last_max * 1.1 if non_last_max > 0 else (float(np.max(bottom)) * 0.2)
	ax_bottom.set_ylim(0, cutoff)

	last_idx = len(x) - 1
	last_total = float(bottom[last_idx])  # ms
	if last_total <= cutoff:
		# If the last bar does not clearly overflow, slightly stretch the upper axis
		top_min = cutoff * 0.9
		top_max = cutoff * 1.05
	else:
		# Show only the top 10% of the overflow beyond cutoff, compressed
		span = max(last_total - cutoff, 1e-6)
		top_min = max(cutoff * 1.05, last_total - span * 0.1)
		top_max = last_total * 1.05
	ax_top.set_ylim(top_min, top_max)

	# Keep only the lower y-axis
	ax_top.spines['bottom'].set_visible(False)
	ax_bottom.spines['top'].set_visible(False)
	ax_top.tick_params(labeltop=False, bottom=False)
	ax_top.tick_params(left=False, labelleft=False)
	ax_bottom.xaxis.tick_bottom()
 
	# Axes and grid (lower only)
	ax_bottom.set_ylabel('Time (ms)', labelpad=50)
	ax_bottom.set_xticks(x)
	ax_bottom.set_xticklabels(data.iloc[:, 0], rotation=30, ha='right')
	ax_bottom.grid(True, which='both', axis='y', linestyle=':')

	# Legend
	ax_top.legend(bbox_to_anchor=(1.02, 1), loc='upper left', frameon=False, fontsize=39)

	# Draw a horizontal line and value label on top of the last bar (upper axis)
	xmin = x[last_idx] - width / 2.0
	xmax = x[last_idx] + width / 2.0
	ax_top.hlines(y=last_total, xmin=xmin, xmax=xmax, colors='black', linestyles='--', linewidth=2)
	ax_top.text(x[last_idx], last_total, f"{last_total:.1f} ms", ha='center', va='bottom')

	# Save
	draw_fig("process-migration-data-large")
	plt.cla()
 
def draw_process_migration_small(data, x, width, colors, stages):

	# Create the second plot - small values
	plt.rcParams.update({'figure.figsize': (8, 3)})
	plt.rcParams.update({'font.size': 17})
	fig3 = plt.figure()
	ax3 = fig3.add_subplot(111)

	# Draw the small-value plot
	small_stages = ['stop-all-threads', 'ckpt-threads', 'ckpt-other', 'restore-threads', 'restore-other', 'start-all-threads']
	bottom2 = np.zeros(len(data.index))
	for i, stage in enumerate(small_stages):
			ax3.bar(x, data[stage] / 1000.0, width, bottom=bottom2,
							color=colors[stages.index(stage)], edgecolor='black', label=stage)
			bottom2 += data[stage] / 1000.0

	# Customize the small-value plot
	ax3.set_ylabel('Time (us)')
	ax3.set_xticks(x)
	ax3.set_xticklabels(data.iloc[:, 0], rotation=30, ha='right')
	ax3.grid(True, which='both', axis='y', linestyle=':')

	ax3.legend(handles=ax3.get_legend_handles_labels()[0][::-1], labels=ax3.get_legend_handles_labels()[1][::-1], frameon=False, loc='upper left', bbox_to_anchor=(1.02, 1), fontsize=18.5)

	# Adjust layout
	plt.tight_layout()

	# Save the small-value plot
	draw_fig("process-migration-data-small")
	plt.close()

def draw_process_migration():
	data = preprocess_data()
	x = np.arange(len(data.index))
	width = 0.5
 
	# label_map = {
	#     'prepare-copy-vmspace': 'prepare vmspace',
	#     'prepare-copy-cap_group': 'prepare cap_group',
	#     'prepare-copy-other': 'prepare other',
	#     'prepare-other': 'prepare other',
	#     'stop-all-threads': 'stop all threads',
	#     'ckpt-threads': 'ckpt threads',
	#     'ckpt-other': 'ckpt other',
	#     'restore-threads': 'restore threads',
	# }

	blue_colors = [plt.cm.Blues(i) for i in [0.2, 0.4, 0.6, 0.8]]
	red_colors = [plt.cm.Reds(i) for i in [0.2, 0.5, 0.8]]
	purple_colors = [plt.cm.Purples(i) for i in [0.2, 0.5, 0.8]]
	stw_colors = [plt.cm.tab20(i) for i in [6,7,8,9]]
	colors = blue_colors + red_colors + purple_colors
	stages = ['prepare-vmspace', 'prepare-cap-group', 'prepare-threads', 'prepare-other', 'stop-all-threads', 'ckpt-threads', 'ckpt-other', 'restore-threads', 'restore-other', 'start-all-threads']
	draw_process_migration_large(data, x, width, colors, stages)
	draw_process_migration_small(data, x, width, colors, stages)

if __name__ == "__main__":
    draw_process_migration()