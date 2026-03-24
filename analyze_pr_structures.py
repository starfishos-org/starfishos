#!/usr/bin/env python3
import re
import sys

def parse_hex_addr(addr_str):
    """Parse hex address string to integer."""
    if addr_str.startswith('0x'):
        return int(addr_str, 16)
    return int(addr_str)

def calculate_size(start_addr_str, end_addr_str):
    """Calculate size between two addresses."""
    start = parse_hex_addr(start_addr_str)
    end = parse_hex_addr(end_addr_str)
    return end - start

def format_size(bytes_size):
    """Format byte size in human-readable format."""
    units = ['B', 'KB', 'MB', 'GB', 'TB']
    size = float(bytes_size)
    unit_idx = 0
    while size > 1024 and unit_idx < len(units) - 1:
        size /= 1024
        unit_idx += 1
    return f"{size:.2f} {units[unit_idx]}"

def parse_pr_output(log_line):
    """Parse [PR][part=...] output line."""
    # Extract all name: [start-end) or name=[start-end) patterns
    pattern = r'(\w+(?:\[\d+\])?)[:\=]\[([0x0-9a-f]+)-([0x0-9a-f]+)\)'
    matches = re.findall(pattern, log_line)

    structures = {}
    for name, start, end in matches:
        size = calculate_size(start, end)
        structures[name] = {
            'start': start,
            'end': end,
            'size': size,
            'size_fmt': format_size(size)
        }

    return structures

if __name__ == '__main__':
    log_file = sys.argv[1] if len(sys.argv) > 1 else '/home/wfn/chcore-cxl/exec_log0.log'

    with open(log_file, 'r') as f:
        for line in f:
            if '[PR][part=' in line:
                structures = parse_pr_output(line)
                print(f"Found {len(structures)} data structures:")
                print()

                # Group structures
                arrays = {}
                for name, info in sorted(structures.items()):
                    # Extract base name (remove [N] suffix)
                    base_name = re.sub(r'\[\d+\]', '', name)
                    if base_name not in arrays:
                        arrays[base_name] = []
                    arrays[base_name].append({
                        'name': name,
                        'info': info
                    })

                # Print by category
                categories = {
                    'curr/next': ['curr', 'next'],
                    'active': ['active_data'],
                    'degree': ['out_degree', 'in_degree'],
                    'degree_local': ['out_degree_local', 'in_degree_local'],
                    'partition': ['partition_offset', 'local_partition_offset'],
                    'adj_index': ['outgoing_adj_index'],
                    'adj_list': ['outgoing_adj_list'],
                    'adj_bitmap': ['outgoing_adj_bitmap'],
                    'compressed': ['compressed_outgoing_adj_index']
                }

                total_cxl = 0
                for category, names in categories.items():
                    for name in names:
                        if name in arrays:
                            print(f"\n{category}: {name}")
                            for entry in sorted(arrays[name], key=lambda e: e['name']):
                                info = entry['info']
                                print(f"  {entry['name']:30s} {info['size_fmt']:>12s}  ({hex(info['size'])})")
                                # Only count once per name
                                if '[1]' in entry['name'] or '[1]' not in str([e['name'] for e in arrays[name]]):
                                    if category in ['curr/next', 'degree_local', 'adj_list', 'active']:
                                        total_cxl += info['size']

                print(f"\n\nEstimated CXL growth from application structures: {format_size(total_cxl)}")
                print(f"(curr/next arrays, active set, degree_local, outgoing_adj_list[1])")
