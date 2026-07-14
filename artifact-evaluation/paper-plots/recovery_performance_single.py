import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np  # required library
from config import draw_fig

def draw_recovery_performance_single():
    # Read CSV file
    data = pd.read_csv('recovery.csv')

    # Read recover_detail.csv
    detail_data = pd.read_csv('recover_detail.csv')

    colormap = plt.colormaps['tab20']

    blue_color = colormap(0)
    red_color = colormap(4)
    green_color = colormap(6)

    font_size = 20

    # Three-stage colors: same tab20 palette as the main axis spans; stacked bar uses saturated colors matching the tinted backgrounds
    stage_colors = [blue_color, green_color, red_color]

    unit = 10 / 1000  # convert unit to seconds

    # Build time axis (each row is 10ms, in seconds)
    time = [i * unit for i in range(len(data))]

    # Convert throughput unit to kops/s
    data['ops'] = data['ops'] / 10

    # Find the region where ops is 0 (each row is 10ms)
    zero_region = data['ops'] == 0
    zero_start = int(zero_region.idxmax())  # index of first 0 ≈ crash / drop-to-zero time
    ms_per_sample = 10.0
    stage_ms = detail_data['time'].to_numpy(dtype=float)
    stage_end_ms = np.cumsum(stage_ms)
    total_recovery_ms = float(stage_end_ms[-1])
    # Align by recover_detail cumulative duration: LevelDB ready ≈ crash + total_recovery_ms
    leveldb_ready_idx = int(
        np.clip(
            round(zero_start + total_recovery_ms / ms_per_sample),
            0,
            len(data) - 1,
        )
    )
    t_crash = time[zero_start]
    t_stage_end = [t_crash + ms / 1000.0 for ms in stage_end_ms]
    full_perf = int(data['ops'][leveldb_ready_idx:].gt(300).idxmax())

    plt.rcdefaults()
    plt.rcParams["ps.useafm"] = True
    # Widen the canvas so the x-axis has more room (time range still set by xlim)
    plt.rcParams.update({'font.size': font_size, 'figure.figsize': (11.0, 3.2)})

    # Create figure and axes
    fig, ax1 = plt.subplots()

    items = detail_data['item']

    def _tint(rgb, a):
        """Light tint (EPS has no alpha; mix colors to approximate axvspan)."""
        r, g, b = rgb[0], rgb[1], rgb[2]
        return (1.0 - a) + a * r, (1.0 - a) + a * g, (1.0 - a) + a * b

    span_colors = [
        _tint(stage_colors[0][:3], 0.18),
        _tint(stage_colors[1][:3], 0.2),
        _tint(stage_colors[2][:3], 0.18),
    ]
    t_prev = t_crash
    for k, t_end in enumerate(t_stage_end):
        ax1.axvspan(t_prev, t_end, facecolor=span_colors[k], edgecolor='none', zorder=1)
        t_prev = t_end

    for tb in t_stage_end[:-1]:
        ax1.axvline(
            tb,
            color='0.45',
            linestyle='--',
            linewidth=0.9,
            zorder=2,
        )

    # Plot the first axes (raw throughput curve)
    ax1.plot(time, data['ops'], label='THP (kops/s)', color=blue_color, linewidth=2, zorder=3)
    ax1.set_ylabel('THP (kops/s)', fontsize=font_size + 3, labelpad=2)
    ax1.set_ylim(bottom=0)
    ax1.grid(True, linestyle='--', alpha=1.0, zorder=0)  # no grid transparency

    t_leveldb_ready = t_stage_end[-1]
    ax1.axvline(
        t_leveldb_ready,
        color=red_color,
        linestyle='-',
        linewidth=1.4,
        zorder=4,
    )
    ymax = float(data['ops'].max())
    y_leveldb = float(data['ops'].iloc[leveldb_ready_idx])
    ax1.annotate(
        'LevelDB Start',
        xy=(t_leveldb_ready, y_leveldb),
        xytext=(t_leveldb_ready + 0.12, ymax * 0.22),
        arrowprops=dict(facecolor=red_color, arrowstyle='->'),
        fontsize=font_size,
        color=red_color,
        zorder=5,
    )

    recovered_time = time[full_perf]
    ax1.axvline(
        recovered_time,
        color=red_color,
        linestyle=':',
        linewidth=1.6,
        zorder=4,
    )

    # Build data for the horizontal stacked bar
    # Compress the visual width of the breakdown bar so it stays as an inset
    # summary instead of dominating the main throughput timeline.
    times_value = (detail_data['time'] / 180).to_numpy(copy=True)
    if len(times_value) >= 2:
        times_value[1] *= 0.45
    if len(times_value) >= 3:
        times_value[2] *= 0.55

    # Draw horizontal stacked bar on the right of the line chart
    bar_height = 32
    bar_y = 205
    # Shift slightly right from center (data coords, seconds like the x-axis)
    breakdown_bar_shift = 0.22

    left = (3.23 - float(np.sum(times_value))) / 2.0 + breakdown_bar_shift
    times = detail_data['time']

    # Draw the horizontal stacked bar
    for i, t in enumerate(times_value):
        ax1.barh(
            bar_y,
            t,
            height=bar_height,
            left=left,
            color=stage_colors[i % len(stage_colors)],
            edgecolor='black',
        )
        ax1.annotate(
            f'{items.iloc[i]}\n{int(round(times.iloc[i]))}ms',
            xy=(left + t * 0.5, bar_y),
            xytext=(
                left + t * 0.5 + 0.09,
                bar_y + (bar_height * 1.2 if i % 2 == 0 else bar_height * -1.2),
            ),
            ha='center',
            va='bottom' if i % 2 == 0 else 'top',
            fontsize=font_size,
            arrowprops=dict(arrowstyle='->', facecolor='black', shrinkA=0, shrinkB=0),
        )
        left += t

    plt.xlim(0, 3.23)
    ax1.xaxis.set_major_locator(mticker.MultipleLocator(1))
    ax1.xaxis.set_major_formatter(mticker.FormatStrFormatter('%d'))
    ax1.text(
        1.12,
        -0.055,
        'Time (s)',
        transform=ax1.transAxes,
        ha='right',
        va='top',
        fontsize=font_size + 3,
        clip_on=False,
    )

    # Adjust layout and save the figure
    fig.tight_layout()
    draw_fig('recovery-performance-single')
    plt.close()

if __name__ == "__main__":
    draw_recovery_performance_single()
