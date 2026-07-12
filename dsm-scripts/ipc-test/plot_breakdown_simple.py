#!/usr/bin/env python3
"""
Plot breakdown of read 4KiB latency for polling IPC with 1 and 4 senders.
Uses pre-computed median values from benchmark.
"""

import matplotlib.pyplot as plt
import numpy as np

# Median latency breakdown from analyze_polling.py output (in nanoseconds)
# Format: alloc, enqueue, srv_dequeue, srv_handle, scheduling, wait, total

breakdown_data = {
    'read_t1': {
        'alloc': 636,
        'enqueue': 704,
        'srv_dequeue': 730,
        'srv_handle': 4597,
        'scheduling': 1192,
        'wait': 6519,
        'total': 7908,
    },
    'read_t4': {
        'alloc': 636,
        'enqueue': 718,
        'srv_dequeue': 782,
        'srv_handle': 5021,
        'scheduling': 18322,
        'wait': 24125,
        'total': 25732,
    },
}

def plot_comparison():
    """Plot breakdown comparison for read_t1 vs read_t4"""
    fig, ax = plt.subplots(figsize=(12, 8))

    modes = ['read_t1 (1 sender)', 'read_t4 (4 senders)']
    colors = {
        'alloc': '#FF6B6B',
        'enqueue': '#4ECDC4',
        'srv_dequeue': '#FFE66D',
        'srv_handle': '#95E1D3',
        'scheduling': '#F38181',
        'wait': '#AA96DA',
    }

    x = np.arange(len(modes))
    width = 0.5

    bottom = np.zeros(2)
    components = ['alloc', 'enqueue', 'srv_dequeue', 'srv_handle', 'scheduling', 'wait']

    for component in components:
        values = [breakdown_data[mode][component] / 1000.0 for mode in ['read_t1', 'read_t4']]  # Convert to µs
        ax.bar(x, values, width, bottom=bottom, label=component, color=colors.get(component, '#999999'))
        bottom += np.array(values)

    # Add total labels on top
    totals = [breakdown_data[mode]['total'] / 1000.0 for mode in ['read_t1', 'read_t4']]
    for i, total in enumerate(totals):
        ax.text(i, total + 0.5, f'{total:.2f}µs', ha='center', va='bottom', fontweight='bold', fontsize=12)

    ax.set_ylabel('Latency (µs)', fontsize=13, fontweight='bold')
    ax.set_title('Polling IPC: Read 4KiB Latency Breakdown\nComparison of 1 vs 4 Concurrent Senders',
                 fontsize=14, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels(modes, fontsize=12)
    ax.legend(fontsize=11, loc='upper left', ncol=2)
    ax.grid(True, axis='y', alpha=0.3)
    ax.set_ylim(0, max(totals) * 1.15)

    plt.tight_layout()
    plt.savefig('breakdown_read_comparison.png', dpi=150, bbox_inches='tight')
    print("Saved: breakdown_read_comparison.png")
    plt.close()

def plot_component_scaling():
    """Plot how each component scales from 1 to 4 senders"""
    fig, ax = plt.subplots(figsize=(12, 8))

    components = ['alloc', 'enqueue', 'srv_dequeue', 'srv_handle', 'scheduling', 'wait']
    colors = {
        'alloc': '#FF6B6B',
        'enqueue': '#4ECDC4',
        'srv_dequeue': '#FFE66D',
        'srv_handle': '#95E1D3',
        'scheduling': '#F38181',
        'wait': '#AA96DA',
    }

    x_labels = ['1 sender', '4 senders']
    x = np.arange(len(x_labels))
    width = 0.12

    for i, component in enumerate(components):
        values = [
            breakdown_data['read_t1'][component] / 1000.0,
            breakdown_data['read_t4'][component] / 1000.0,
        ]
        offset = (i - len(components)/2 + 0.5) * width
        ax.bar(x + offset, values, width, label=component, color=colors.get(component, '#999999'))

    ax.set_ylabel('Latency (µs)', fontsize=13, fontweight='bold')
    ax.set_title('Polling IPC: Latency Component Scaling (Read 4KiB)',
                 fontsize=14, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels(x_labels, fontsize=12)
    ax.legend(fontsize=10, loc='upper left', ncol=2)
    ax.grid(True, axis='y', alpha=0.3)

    plt.tight_layout()
    plt.savefig('breakdown_component_scaling.png', dpi=150, bbox_inches='tight')
    print("Saved: breakdown_component_scaling.png")
    plt.close()

def print_table():
    """Print breakdown table"""
    print("\n" + "="*90)
    print("Read 4KiB Latency Breakdown (Polling IPC)")
    print("="*90)
    print(f"{'Component':<20} {'1 Sender (ns)':<20} {'4 Senders (ns)':<20} {'Scaling':<15}")
    print("-"*90)

    components = ['alloc', 'enqueue', 'srv_dequeue', 'srv_handle', 'scheduling', 'wait', 'total']
    for component in components:
        v1 = breakdown_data['read_t1'][component]
        v4 = breakdown_data['read_t4'][component]
        scaling = f"{v4/v1:.1f}x" if v1 > 0 else "N/A"
        print(f"{component:<20} {v1:<20} {v4:<20} {scaling:<15}")
    print("="*90)

if __name__ == '__main__':
    print_table()
    plot_comparison()
    plot_component_scaling()
