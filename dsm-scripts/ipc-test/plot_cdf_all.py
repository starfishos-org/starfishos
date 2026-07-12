#!/usr/bin/env python3
"""
Generate CDF plots from polling_latency.log
- One plot for empty benchmarks (1, 4, 8 threads)
- One plot for read benchmarks (1, 4, 8 threads)
"""

import re
import matplotlib.pyplot as plt
from collections import defaultdict

def parse_cdf_from_log(logfile):
    """Parse CDF data from polling_latency.log"""
    cdf_data = defaultdict(list)
    current_mode = None

    with open(logfile, 'r') as f:
        for line in f:
            m = re.match(r'\[CDF_BEGIN\]\s+mode=(\S+)\s+count=(\d+)', line)
            if m:
                current_mode = m.group(1)
                continue

            if current_mode:
                m = re.match(r'\[CDF\]\s+(\d+)\s+(\d+)', line)
                if m:
                    idx = int(m.group(1))
                    latency = int(m.group(2))
                    cdf_data[current_mode].append((idx, latency))

                if '[CDF_END]' in line:
                    current_mode = None

    return cdf_data

def plot_cdf_comparison(cdf_data, modes, title, output_file):
    """Plot CDF for multiple modes on one graph"""
    fig, ax = plt.subplots(figsize=(12, 8))

    for mode in modes:
        if mode not in cdf_data:
            print(f"Warning: mode {mode} not found")
            continue

        data = cdf_data[mode]
        if not data:
            continue

        # data is list of (idx, latency) tuples
        # Convert to (percentile, latency)
        indices = [x[0] for x in data]
        latencies = [x[1] / 1000.0 for x in data]  # Convert ns to µs
        count = max(indices) + 1
        percentiles = [i * 100.0 / count for i in indices]

        ax.plot(percentiles, latencies, linewidth=2, label=mode, marker='o', markersize=2, alpha=0.8)

    ax.set_xlabel('Percentile (%)', fontsize=12)
    ax.set_ylabel('Latency (µs)', fontsize=12)
    ax.set_title(title, fontsize=14, fontweight='bold')
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=11, loc='upper left')
    ax.set_xlim(0, 100)

    plt.tight_layout()
    plt.savefig(output_file, dpi=150)
    print(f"Saved: {output_file}")
    plt.close()

def main():
    logfile = 'polling_latency.log'
    cdf_data = parse_cdf_from_log(logfile)

    if not cdf_data:
        print(f"No CDF data found in {logfile}")
        return

    print(f"Parsed {len(cdf_data)} benchmark modes:")
    for mode in sorted(cdf_data.keys()):
        print(f"  {mode}: {len(cdf_data[mode])} samples")

    # Plot 1: Empty benchmarks
    plot_cdf_comparison(
        cdf_data,
        ['empty_t1', 'empty_t4', 'empty_t8'],
        'Polling Latency: Empty Queue (no FS I/O)',
        'polling_cdf_empty.png'
    )

    # Plot 2: Read benchmarks
    plot_cdf_comparison(
        cdf_data,
        ['read_t1', 'read_t4', 'read_t8'],
        'Polling Latency: Read 4KiB File',
        'polling_cdf_read.png'
    )

if __name__ == '__main__':
    main()
