#!/usr/bin/env python3
"""
Parse VMSPACE STATS from log file and visualize virtual address space distribution
"""

import re
import sys
from collections import defaultdict

try:
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False
    print("Warning: matplotlib not found. Visualization will be skipped.")
    print("Install with: pip3 install matplotlib")

def format_size(pages):
    """Convert page count to human-readable size (KB/MB/GB)"""
    PAGE_SIZE = 0x1000  # 4KB per page
    size_bytes = pages * PAGE_SIZE
    size_mb = size_bytes / (1024 * 1024)
    size_gb = size_bytes / (1024 * 1024 * 1024)
    
    if size_gb >= 1.0:
        return f"{size_gb:.2f} GB"
    else:
        return f"{size_mb:.2f} MB"

def parse_single_stats_section(lines, start_idx):
    """Parse a single VMSPACE STATS section from lines starting at start_idx"""
    pages = []
    vmrs = []
    process_name = None
    in_stats = False
    seen_summary = False
    
    PAGE_SIZE = 0x1000
    
    i = start_idx
    while i < len(lines):
        line = lines[i]
        
        # Check if we're entering VMSPACE STATS section
        if '[VMSPACE STATS] ==========================================' in line:
            if not in_stats:
                in_stats = True
            else:
                # End of stats section - only break if we've seen the summary
                if seen_summary:
                    break
                # Otherwise, this might be a continuation, keep going
            i += 1
            continue
        
        if not in_stats:
            i += 1
            continue
        
        # Check if we're exiting VMSPACE STATS section
        if '[VMSPACE STATS] Summary Statistics:' in line:
            seen_summary = True
            # Continue to read until closing separator
            i += 1
            continue
        
        # Extract process name
        if '[VMSPACE STATS] Process:' in line:
            match = re.search(r'Process:\s*(.+)', line)
            if match:
                process_name = match.group(1).strip()
            i += 1
            continue
        
        # Parse VMR region
        if '[VMSPACE STATS] VMR:' in line:
            match = re.search(r'VA=0x([0-9a-fA-F]+)-0x([0-9a-fA-F]+),\s*size=0x([0-9a-fA-F]+)', line)
            if match:
                start_va = int(match.group(1), 16)
                end_va = int(match.group(2), 16)
                size = int(match.group(3), 16)
                vmrs.append({
                    'start': start_va,
                    'end': end_va,
                    'size': size
                })
            i += 1
            continue
        
        # Parse page mapping
        if '[VMSPACE STATS]' in line and '->' in line:
            if 'machines:' not in line and 'CXL' not in line:
                i += 1
                continue
            
            va_range_match = re.search(r'0x([0-9a-fA-F]+)-0x([0-9a-fA-F]+)\s*->', line)
            va_single_match = re.search(r'0x([0-9a-fA-F]+)\s*->', line)
            
            if not va_range_match and not va_single_match:
                i += 1
                continue
            
            location = None
            machines = []
            is_cxl = False
            
            if re.search(r'->\s*CXL', line):
                is_cxl = True
                machines_match = re.search(r'machines:\s*\[', line)
                if machines_match:
                    start_pos = machines_match.end()
                    bracket_count = 1
                    end_pos = start_pos
                    while end_pos < len(line) and bracket_count > 0:
                        if line[end_pos] == '[':
                            bracket_count += 1
                        elif line[end_pos] == ']':
                            bracket_count -= 1
                        end_pos += 1
                    if bracket_count == 0:
                        machines_str = line[start_pos:end_pos-1]
                        all_digits = re.findall(r'\d+', machines_str)
                        machines = [int(mid) for mid in all_digits]
                        if machines:
                            location = f'Machine {machines[0]}'
                            is_cxl = False
                else:
                    location = 'CXL'
            else:
                machines_match = re.search(r'machines:\s*\[', line)
                if machines_match:
                    start_pos = machines_match.end()
                    bracket_count = 1
                    end_pos = start_pos
                    while end_pos < len(line) and bracket_count > 0:
                        if line[end_pos] == '[':
                            bracket_count += 1
                        elif line[end_pos] == ']':
                            bracket_count -= 1
                        end_pos += 1
                    if bracket_count == 0:
                        machines_str = line[start_pos:end_pos-1]
                        all_digits = re.findall(r'\d+', machines_str)
                        machines = [int(mid) for mid in all_digits]
                        if machines:
                            location = f'Machine {machines[0]}'
            
            if location:
                if va_range_match:
                    start_va = int(va_range_match.group(1), 16)
                    end_va = int(va_range_match.group(2), 16)
                    if start_va > end_va:
                        start_va, end_va = end_va, start_va
                    
                    range_size = end_va - start_va
                    MAX_REASONABLE_RANGE = 0x40000000  # 1GB
                    if range_size > MAX_REASONABLE_RANGE:
                        print(f"Warning: Skipping suspiciously large range: 0x{start_va:016x}-0x{end_va:016x} (size: {range_size / (1024*1024):.2f} MB)")
                        i += 1
                        continue
                    
                    pages.append({
                        'va': start_va,
                        'end_va': end_va,
                        'location': location,
                        'machines': machines,
                        'is_cxl': is_cxl,
                        'is_range': True
                    })
                else:
                    va = int(va_single_match.group(1), 16)
                    pages.append({
                        'va': va,
                        'end_va': va,
                        'location': location,
                        'machines': machines,
                        'is_cxl': is_cxl,
                        'is_range': False
                    })
        
        i += 1
    
    return process_name, pages, vmrs, i

def pages_to_set(pages):
    """Convert pages list to a set of (va, location) tuples for comparison"""
    PAGE_SIZE = 0x1000
    page_set = set()
    
    for page in pages:
        va_start = page['va']
        va_end = page.get('end_va', va_start)
        if va_start > va_end:
            va_start, va_end = va_end, va_start
        
        location = page.get('location')
        if not location:
            continue
        
        # Expand range to individual pages
        for va in range(va_start, va_end + PAGE_SIZE, PAGE_SIZE):
            page_set.add((va, location))
    
    return page_set

def find_new_pages(first_pages, second_pages):
    """Find pages that exist in second_pages but not in first_pages"""
    first_set = pages_to_set(first_pages)
    second_set = pages_to_set(second_pages)
    
    new_pages_set = second_set - first_set
    
    # Convert back to page format, preserving range information where possible
    new_pages = []
    PAGE_SIZE = 0x1000
    
    # Group consecutive pages with same location into ranges
    sorted_new = sorted(new_pages_set, key=lambda x: (x[0], x[1]))
    
    if not sorted_new:
        return []
    
    current_range_start = sorted_new[0][0]
    current_range_end = sorted_new[0][0]
    current_location = sorted_new[0][1]
    
    for va, location in sorted_new[1:]:
        if location == current_location and va == current_range_end + PAGE_SIZE:
            # Continue current range
            current_range_end = va
        else:
            # End current range, start new one
            # Extract machine ID from location if it's a Machine location
            machines = []
            is_cxl = (current_location == 'CXL')
            if 'Machine' in current_location:
                try:
                    machine_id = int(current_location.split()[-1])
                    machines = [machine_id]
                except ValueError:
                    pass
            
            if current_range_start == current_range_end:
                new_pages.append({
                    'va': current_range_start,
                    'end_va': current_range_start,
                    'location': current_location,
                    'machines': machines,
                    'is_cxl': is_cxl,
                    'is_range': False
                })
            else:
                new_pages.append({
                    'va': current_range_start,
                    'end_va': current_range_end,
                    'location': current_location,
                    'machines': machines,
                    'is_cxl': is_cxl,
                    'is_range': True
                })
            current_range_start = va
            current_range_end = va
            current_location = location
    
    # Add last range
    # Extract machine ID from location if it's a Machine location
    machines = []
    is_cxl = (current_location == 'CXL')
    if 'Machine' in current_location:
        try:
            machine_id = int(current_location.split()[-1])
            machines = [machine_id]
        except ValueError:
            pass
    
    if current_range_start == current_range_end:
        new_pages.append({
            'va': current_range_start,
            'end_va': current_range_start,
            'location': current_location,
            'machines': machines,
            'is_cxl': is_cxl,
            'is_range': False
        })
    else:
        new_pages.append({
            'va': current_range_start,
            'end_va': current_range_end,
            'location': current_location,
            'machines': machines,
            'is_cxl': is_cxl,
            'is_range': True
        })
    
    return new_pages

def count_cxl_pages(pages):
    """Count total number of CXL pages from a pages list"""
    if not pages:
        return 0
    
    PAGE_SIZE = 0x1000
    total_pages = 0
    MAX_REASONABLE_RANGE = 0x40000000  # 1GB
    
    for page in pages:
        location = page.get('location')
        if location != 'CXL':
            continue
        
        if page.get('is_range', False):
            va_start = page['va']
            va_end = page.get('end_va', va_start)
            if va_start > va_end:
                va_start, va_end = va_end, va_start
            
            range_size = va_end - va_start
            if range_size > MAX_REASONABLE_RANGE:
                print(f"Warning: Skipping suspiciously large CXL range when counting pages: 0x{va_start:016x}-0x{va_end:016x} (size: {range_size / (1024*1024):.2f} MB)")
                continue
            
            page_count = (va_end - va_start) // PAGE_SIZE + 1
            if page_count <= 0:
                print(f"Warning: Invalid CXL page range: 0x{va_start:016x}-0x{va_end:016x}, skipping")
                continue
            
            total_pages += page_count
        else:
            total_pages += 1
    
    return total_pages

def print_page_distribution_simple(label, pages):
    """Print a compact page distribution summary for a given pages list"""
    if not pages:
        return
    
    PAGE_SIZE = 0x1000
    MAX_REASONABLE_RANGE = 0x40000000  # 1GB
    location_counts = defaultdict(int)
    
    for page in pages:
        if page.get('is_range', False):
            va_start = page['va']
            va_end = page.get('end_va', va_start)
            if va_start > va_end:
                va_start, va_end = va_end, va_start
            
            range_size = va_end - va_start
            if range_size > MAX_REASONABLE_RANGE:
                print(f"Warning: Skipping suspiciously large range in distribution: 0x{va_start:016x}-0x{va_end:016x} (size: {range_size / (1024*1024):.2f} MB)")
                continue
            
            page_count = (va_end - va_start) // PAGE_SIZE + 1
            if page_count <= 0:
                print(f"Warning: Invalid page range in distribution: 0x{va_start:016x}-0x{va_end:016x}, skipping")
                continue
        else:
            page_count = 1
        
        location = page.get('location')
        if location:
            location_counts[location] += page_count
    
    if not location_counts:
        return
    
    print(label)
    for loc in sorted(location_counts.keys()):
        page_count = location_counts[loc]
        print(f"  {loc}: {page_count} pages ({format_size(page_count)})")

def parse_log_file(log_file):
    """Parse log file and extract VMSPACE STATS information, comparing two outputs"""
    # Read all lines first
    with open(log_file, 'r') as f:
        lines = f.readlines()
    
    # Find all VMSPACE STATS section starts
    stats_starts = []
    for i, line in enumerate(lines):
        if '[VMSPACE STATS] ==========================================' in line:
            stats_starts.append(i)
    
    if len(stats_starts) < 2:
        print("Warning: Found less than 2 VMSPACE STATS sections. Will parse only the first one.")
        if len(stats_starts) == 0:
            return None, [], []
        # Fall back to original behavior
        process_name, pages, vmrs, _ = parse_single_stats_section(lines, stats_starts[0])
        # Also report initial CXL page count
        cxl_pages = count_cxl_pages(pages)
        print(f"Initial CXL pages: {cxl_pages} ({format_size(cxl_pages)})")
        return process_name, pages, vmrs
    
    # Parse first stats section, starting from the first separator
    print(f"Found {len(stats_starts)} VMSPACE STATS sections (raw separators). Parsing first section...")
    first_name, first_pages, first_vmrs, first_end = parse_single_stats_section(lines, stats_starts[0])

    # Do not use stats_starts[1] directly; find the real start of the next section after first_end
    second_start_candidates = [i for i in stats_starts if i > first_end]
    if not second_start_candidates:
        # No real second section found; use only the first section result
        print("Warning: Could not find a second complete VMSPACE STATS section after the first one.")
        # Keep return format consistent with multi-section logic: return first section pages / vmrs
        process_name, pages, vmrs, _ = parse_single_stats_section(lines, stats_starts[0])
        cxl_pages = count_cxl_pages(pages)
        print(f"Initial CXL pages: {cxl_pages} ({format_size(cxl_pages)})")
        return process_name, pages, vmrs

    second_start = second_start_candidates[0]
    print(f"Parsing second section starting from line {second_start} (after first section ends at line {first_end}).")
    second_name, second_pages, second_vmrs, second_end = parse_single_stats_section(lines, second_start)
    
    print(f"First stats: {len(first_pages)} page entries")
    print(f"Second stats: {len(second_pages)} page entries")
    
    # Print compact page distribution for the beginning and end snapshots
    print_page_distribution_simple("Page distribution (First stats):", first_pages)
    print_page_distribution_simple("Page distribution (Second stats):", second_pages)
    
    # Find new pages (pages in second but not in first)
    new_pages = find_new_pages(first_pages, second_pages)
    
    print(f"New pages (second - first): {len(new_pages)} page entries")

    # Also print absolute page counts (unique pages) for first and second snapshots
    first_total_pages = len(pages_to_set(first_pages))
    second_total_pages = len(pages_to_set(second_pages))
    new_total_pages = len(pages_to_set(new_pages))
    print(f"First stats total pages (unique VAs): {first_total_pages} ({format_size(first_total_pages)})")
    print(f"Second stats total pages (unique VAs): {second_total_pages} ({format_size(second_total_pages)})")
    print(f"New pages total (unique VAs): {new_total_pages} ({format_size(new_total_pages)})")
    
    # Use second process name (or first if second is "unknown")
    process_name = second_name if second_name and second_name != "unknown" else first_name
    
    return process_name, new_pages, second_vmrs

def visualize_vmspace(pages, vmrs, process_name, output_file='vmspace_distribution.png'):
    """Visualize virtual address space distribution"""
    if not HAS_MATPLOTLIB:
        print("Cannot visualize: matplotlib not installed")
        return
    
    if not pages:
        print("No pages found to visualize")
        return
    
    PAGE_SIZE = 0x1000
    
    # Sort pages by VA (no expansion needed - draw ranges directly)
    pages.sort(key=lambda x: x['va'])
    
    # Group consecutive ranges/pages into segments (to handle gaps)
    va_segments = []
    if pages:
        current_segment = [pages[0]]
        for i in range(1, len(pages)):
            # Get the end VA of the last item in current segment
            last_item = current_segment[-1]
            last_end = last_item.get('end_va', last_item['va'])
            
            # Get the start VA of current item
            current_start = pages[i]['va']
            
            # Check if this VA is consecutive (within PAGE_SIZE of the end of last item)
            if current_start - last_end <= PAGE_SIZE:
                current_segment.append(pages[i])
            else:
                # Gap detected, start new segment
                va_segments.append(current_segment)
                current_segment = [pages[i]]
        va_segments.append(current_segment)
    
    # Prepare data for plotting - get min/max from all ranges
    min_va = min(min(p['va'], p.get('end_va', p['va'])) for p in pages)
    max_va = max(max(p['va'], p.get('end_va', p['va'])) for p in pages)
    
    # Create color mapping
    color_map = {
        'CXL': 'red',
    }
    machine_colors = ['blue', 'green', 'orange', 'purple', 'brown', 'pink', 'gray', 'olive']
    
    # Count pages per location (calculate actual page count from ranges)
    # Use location field directly to avoid double counting
    location_counts = defaultdict(int)
    for page in pages:
        if page.get('is_range', False):
            # Calculate number of pages in range
            va_start = page['va']
            va_end = page['end_va']
            # Ensure va_start <= va_end
            if va_start > va_end:
                va_start, va_end = va_end, va_start
            page_count = (va_end - va_start) // PAGE_SIZE + 1
            # Safety check: ensure page_count is positive
            if page_count <= 0:
                print(f"Warning: Invalid page range: 0x{va_start:016x}-0x{va_end:016x}, skipping")
                continue
        else:
            page_count = 1
        
        # Count based on location field only (location already reflects CXL or Machine)
        location = page.get('location')
        if location:
            location_counts[location] += page_count
    
    # Create figure with main plot and side bar chart
    fig = plt.figure(figsize=(12, 5))
    gs = fig.add_gridspec(1, 2, width_ratios=[5, 1], wspace=0.3)
    ax1 = fig.add_subplot(gs[0])  # Main address space plot
    ax2 = fig.add_subplot(gs[1])  # Side statistics bar chart
    
    # Plot 1: Address space distribution
    title = f'New Pages Only (Second - First) - {process_name}'
    ax1.set_title(title, fontsize=14, fontweight='bold')
    ax1.set_xlabel('Virtual Address (hex)', fontsize=12)
    ax1.set_ylabel('Location', fontsize=12)
    
    # Plot each page/range
    y_positions = {}
    y_pos = 0
    for location in sorted(set(p['location'] for p in pages)):
        y_positions[location] = y_pos
        y_pos += 1
    
    # Identify major VA segments: group consecutive segments into major ranges
    # Major segments are separated by large gaps (> 1GB)
    MAJOR_GAP_THRESHOLD = 0x40000000  # 1GB threshold for major segment separation
    
    major_segments = []  # List of (segments_list, seg_min, seg_max)
    current_major_segments = []
    current_major_min = None
    current_major_max = None
    
    for segment in va_segments:
        seg_min = min(p['va'] for p in segment)
        seg_max = max(p.get('end_va', p['va']) for p in segment)
        
        if current_major_min is None:
            # First segment
            current_major_segments = [segment]
            current_major_min = seg_min
            current_major_max = seg_max
        elif seg_min - current_major_max <= MAJOR_GAP_THRESHOLD:
            # Continue current major segment (small gap)
            current_major_segments.append(segment)
            current_major_max = max(current_major_max, seg_max)
        else:
            # Large gap detected - start new major segment
            major_segments.append((current_major_segments, current_major_min, current_major_max))
            current_major_segments = [segment]
            current_major_min = seg_min
            current_major_max = seg_max
    
    # Add last major segment
    if current_major_segments:
        major_segments.append((current_major_segments, current_major_min, current_major_max))
    
    # Create VA to display position mapping - each major segment is continuous
    display_pos = 0
    final_display_pos = 0
    segment_mappings = []  # List of (seg_min, seg_max, display_start, major_seg_label) tuples
    major_segment_info = []  # List of (major_start_va, major_end_va, display_start, display_end, label)
    
    for major_idx, (segments_list, major_min, major_max) in enumerate(major_segments):
        major_label = f"0x{major_min:x}"
        major_display_start = display_pos
        
        # Map all segments within this major segment continuously
        for segment in segments_list:
            seg_min = min(p['va'] for p in segment)
            seg_max = max(p.get('end_va', p['va']) for p in segment)
            seg_size = seg_max - seg_min
            segment_mappings.append((seg_min, seg_max, display_pos, major_label))
            display_pos += seg_size
        
        major_display_end = display_pos
        major_segment_info.append((major_min, major_max, major_display_start, major_display_end, major_label))
        
        # Add small gap between major segments (but not after the last one)
        if major_idx < len(major_segments) - 1:
            gap_size = (major_display_end - major_display_start) * 0.02  # 2% gap
            display_pos += gap_size
        final_display_pos = display_pos
    
    # Helper function to convert VA to display position
    def va_to_display_pos(va):
        for seg_min, seg_max, display_start, label in segment_mappings:
            if seg_min <= va <= seg_max:
                return display_start + (va - seg_min)
        # Fallback (shouldn't happen)
        return va
    
    # Draw pages/ranges - directly draw ranges without expansion using Rectangle patches
    # Collect all rectangles first, then add them in batch
    rectangles = []
    for page in pages:
        va_start = page['va']
        va_end = page.get('end_va', va_start)
        
        # Ensure va_start <= va_end
        if va_start > va_end:
            va_start, va_end = va_end, va_start
        
        # Get display positions
        va_start_display = va_to_display_pos(va_start)
        va_end_display = va_to_display_pos(va_end)
        
        # Calculate width in display units
        width = max(1, va_end_display - va_start_display)  # At least 1 unit wide
        
        location = page['location']
        y_pos = y_positions[location]
        
        # Determine color
        if page['is_cxl']:
            color = color_map['CXL']
        elif 'Machine' in location:
            machine_id = int(location.split()[-1])
            color = machine_colors[machine_id % len(machine_colors)]
        else:
            color = 'gray'
        
        # Create rectangle patch
        # Use thinner edge and no edge color to avoid visual artifacts
        rect = mpatches.Rectangle(
            (va_start_display, y_pos - 0.4),  # (x, y) bottom-left corner
            width,  # width
            0.8,  # height
            facecolor=color,
            edgecolor='none',  # No edge to avoid visual artifacts
            linewidth=0,
            alpha=0.9  # More opaque
        )
        rectangles.append(rect)
    
    # Add all rectangles at once (much faster)
    for rect in rectangles:
        ax1.add_patch(rect)
    
    # Set y-axis labels
    ax1.set_yticks(list(y_positions.values()))
    ax1.set_yticklabels(list(y_positions.keys()))
    
    # Set x-axis limits to only show mapped address segments (truncate gaps)
    # Always truncate gaps, even for single segment
    display_min = 0
    display_max = final_display_pos
    padding = (display_max - display_min) * 0.01 if display_max > display_min else 0x1000
    ax1.set_xlim(display_min - padding, display_max + padding)
    
    # Set x-axis ticks to show only major segment boundaries (start and end of each segment)
    x_ticks = []
    x_tick_labels = []
    for major_start_va, major_end_va, major_display_start, major_display_end, label in major_segment_info:
        x_ticks.append(major_display_start)
        x_tick_labels.append(f'{label}')  # Show segment start address
        x_ticks.append(major_display_end)
        x_tick_labels.append(f'0x{major_end_va:x}')  # Show segment end address
    
    ax1.set_xticks(x_ticks)
    ax1.set_xticklabels(x_tick_labels, rotation=45, ha='right')
    
    # Add vertical lines and labels for major segment boundaries
    for major_start_va, major_end_va, major_display_start, major_display_end, label in major_segment_info:
        # Draw vertical line at major segment start
        ax1.axvline(x=major_display_start, color='gray', linestyle='--', linewidth=1, alpha=0.5)
        # Add label above the plot
        ax1.text(major_display_start, max(y_positions.values()) + 0.5, 
                f'{label}', ha='center', va='bottom', fontsize=10, fontweight='bold',
                bbox=dict(boxstyle='round,pad=0.5', facecolor='lightyellow', alpha=0.8, edgecolor='black'))
    
    # Optionally add break markers between segments to indicate gaps in address space
    # Disabled by default to avoid visual clutter - uncomment to enable
    # if len(va_segments) > 1:
    #     segment_ranges = []
    #     for segment in va_segments:
    #         seg_min = min(p['va'] for p in segment)
    #         seg_max = max(p.get('end_va', p['va']) for p in segment)
    #         segment_ranges.append((seg_min, seg_max))
    #     
    #     display_pos_accum = 0
    #     for i in range(len(segment_ranges) - 1):
    #         seg_min, seg_max = segment_ranges[i]
    #         seg_size = seg_max - seg_min
    #         display_pos_accum += seg_size
    #         
    #         # Calculate break position (middle of the gap)
    #         gap_start = display_pos_accum
    #         gap_size = seg_size * 0.05  # Gap size
    #         break_x = gap_start + gap_size / 2
    #         
    #         # Draw break markers at top and bottom, spanning all y positions
    #         y_min = min(y_positions.values()) - 0.5
    #         y_max = max(y_positions.values()) + 0.5
    #         break_height = 0.2  # Height of break marker
    #         
    #         # Draw simple break markers: two diagonal lines at top and bottom
    #         # Top break marker (//)
    #         ax1.plot([break_x - break_height, break_x], [y_max - break_height * 0.5, y_max], 
    #                 'k-', linewidth=1.5, clip_on=False, zorder=10)
    #         ax1.plot([break_x, break_x + break_height], [y_max, y_max - break_height * 0.5], 
    #                 'k-', linewidth=1.5, clip_on=False, zorder=10)
    #         
    #         # Bottom break marker (//)
    #         ax1.plot([break_x - break_height, break_x], [y_min + break_height * 0.5, y_min], 
    #                 'k-', linewidth=1.5, clip_on=False, zorder=10)
    #         ax1.plot([break_x, break_x + break_height], [y_min, y_min + break_height * 0.5], 
    #                 'k-', linewidth=1.5, clip_on=False, zorder=10)
    #         
    #         display_pos_accum += gap_size  # Add gap
    ax1.tick_params(axis='x', rotation=45)
    
    # Remove grid to avoid visual clutter
    ax1.grid(False)
    
    # Set background color to white to avoid any blue tint
    ax1.set_facecolor('white')
    
    # Plot 2: Statistics bar chart (transposed, on the right side)
    ax2.set_title('Page Count', fontsize=12, fontweight='bold')
    ax2.set_xlabel('Number of Pages', fontsize=10)
    ax2.set_ylabel('Location', fontsize=10)
    
    # Sort locations to match left plot order (CXL at bottom)
    # Get the order from y_positions (same as left plot)
    locations_ordered = []
    for location in sorted(set(p['location'] for p in pages)):
        if location in location_counts:
            locations_ordered.append(location)
    
    # Reverse to match barh order (top to bottom matches left plot's bottom to top)
    locations_ordered.reverse()
    
    counts = [location_counts[loc] for loc in locations_ordered]
    colors_bar = []
    for loc in locations_ordered:
        if loc == 'CXL':
            colors_bar.append(color_map['CXL'])
        elif 'Machine' in loc:
            machine_id = int(loc.split()[-1])
            colors_bar.append(machine_colors[machine_id % len(machine_colors)])
        else:
            colors_bar.append('gray')
    
    # Transpose: use barh instead of bar
    bars = ax2.barh(locations_ordered, counts, color=colors_bar, alpha=0.7, edgecolor='black')
    
    # Add value labels on bars (count written in the figure with size units)
    for i, bar in enumerate(bars):
        width = bar.get_width()
        page_count = int(width)
        size_str = format_size(page_count)
        ax2.text(width, bar.get_y() + bar.get_height()/2.,
                f'{page_count}\n({size_str})',
                ha='left', va='center', fontsize=9, fontweight='bold')
    
    ax2.grid(True, alpha=0.3, axis='x')
    ax2.tick_params(axis='y', labelsize=9)
    
    # Add legend
    legend_elements = []
    legend_elements.append(mpatches.Patch(color=color_map['CXL'], label='CXL (Shared Memory)'))
    for i in range(max((int(loc.split()[-1]) for loc in locations_ordered if 'Machine' in loc), default=-1) + 1):
        if f'Machine {i}' in locations_ordered:
            legend_elements.append(mpatches.Patch(color=machine_colors[i % len(machine_colors)], 
                                                 label=f'Machine {i}'))
    
    fig.legend(handles=legend_elements, loc='upper left', fontsize=10, bbox_to_anchor=(0, 1))
    
    plt.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Visualization saved to {output_file}")
    plt.close()

def print_statistics(pages, vmrs, process_name):
    """Print statistics about the virtual address space"""
    if not pages:
        return
    
    PAGE_SIZE = 0x1000
    
    # Count pages per location (calculate actual page count from ranges)
    # Use location field directly to avoid double counting
    location_counts = defaultdict(int)
    total_page_count = 0
    for page in pages:
        if page.get('is_range', False):
            # Calculate number of pages in range
            va_start = page['va']
            va_end = page['end_va']
            # Ensure va_start <= va_end
            if va_start > va_end:
                va_start, va_end = va_end, va_start
            
            # Sanity check: filter out obviously wrong ranges
            # A reasonable VA range should not exceed 1GB (0x40000000)
            range_size = va_end - va_start
            MAX_REASONABLE_RANGE = 0x40000000  # 1GB
            if range_size > MAX_REASONABLE_RANGE:
                print(f"Warning: Skipping suspiciously large range in statistics: 0x{va_start:016x}-0x{va_end:016x} (size: {range_size / (1024*1024):.2f} MB)")
                continue
            
            page_count = (va_end - va_start) // PAGE_SIZE + 1
            # Safety check: ensure page_count is positive
            if page_count <= 0:
                print(f"Warning: Invalid page range: 0x{va_start:016x}-0x{va_end:016x}, skipping")
                continue
        else:
            page_count = 1
        
        total_page_count += page_count
        
        # Count based on location field only (location already reflects CXL or Machine)
        location = page.get('location')
        if location:
            location_counts[location] += page_count
    
    min_va = min(min(p['va'], p.get('end_va', p['va'])) for p in pages)
    max_va = max(max(p['va'], p.get('end_va', p['va'])) for p in pages)
    
    print("\n=== Statistics ===")
    print(f"Process: {process_name}")
    print(f"Total page entries: {len(pages)}")
    print(f"Total pages: {total_page_count} ({format_size(total_page_count)})")
    print(f"Address range: 0x{min_va:016x} - 0x{max_va:016x}")
    print("\nPage distribution:")
    for loc in sorted(location_counts.keys()):
        page_count = location_counts[loc]
        print(f"  {loc}: {page_count} pages ({format_size(page_count)})")
    
    print(f"\nTotal VMR regions: {len(vmrs)}")
    total_vmr_size = sum(vmr['size'] for vmr in vmrs)
    print(f"Total VMR size: 0x{total_vmr_size:016x} ({total_vmr_size / (1024*1024):.2f} MB)")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 parse_vmspace_stats.py <log_file> [output_image]")
        sys.exit(1)
    
    log_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else 'vmspace_distribution.png'
    
    print(f"Parsing log file: {log_file}")
    result = parse_log_file(log_file)
    
    if result is None:
        print("No VMSPACE STATS found in log file")
        sys.exit(1)
    
    process_name, pages, vmrs = result
    
    if not pages:
        print("No new pages found (second stats has no additional pages compared to first)")
        print("This might mean:")
        print("  1. No pages were added between the two stats outputs")
        print("  2. Only one stats section was found in the log")
        sys.exit(0)
    
    PAGE_SIZE = 0x1000
    # Calculate total pages with validation
    total_pages = 0
    for p in pages:
        va_start = p['va']
        va_end = p.get('end_va', va_start)
        # Ensure va_start <= va_end
        if va_start > va_end:
            va_start, va_end = va_end, va_start
        page_count = (va_end - va_start) // PAGE_SIZE + 1
        if page_count > 0:
            total_pages += page_count
    
    print(f"\n=== New Pages (Second - First) ===")
    print(f"Found {len(pages)} page entries (ranges) for process: {process_name}")
    print(f"Total new pages: {total_pages}")
    print(f"Found {len(vmrs)} VMR regions (from second stats)")
    
    # Print statistics
    print_statistics(pages, vmrs, process_name)
    
    # Visualize if matplotlib is available
    if HAS_MATPLOTLIB:
        # Update title to indicate these are new pages
        visualize_vmspace(pages, vmrs, f"{process_name} (New Pages Only)", output_file)
    else:
        print("\nTo generate visualization, install matplotlib:")
        print("  pip3 install matplotlib")

if __name__ == '__main__':
    main()

