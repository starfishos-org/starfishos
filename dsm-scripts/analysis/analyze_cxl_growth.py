#!/usr/bin/env python3
"""
Parse CXL state from two VMSPACE STATS dumps in exec_log0.log, focusing only on
CXL segments that appear in the 2nd dump but not the 1st. Data-structure VAs come
from [PR][part=0] lines in the same file (e.g. around line 417). Output: size of
new CXL, VA ranges, and possible data structures. Format matches exec_log0.log:
concrete CXL segments (e.g. 0x3007d2f07000-0x3007d2f0c000 -> CXL), not whole VMRs.
"""

import re
import sys

PAGE_SIZE = 0x1000  # 4KB

def parse_cxl_line(line):
    """Parse a '   VA or VA-VA -> CXL' line; return (start_va, end_va) half-open, or None."""
    line = line.strip()
    if ' -> CXL' not in line or 'machines:' in line:
        return None
    # Format: "0x3007d2f07000-0x3007d2f0c000 -> CXL" or "0x3007d2f07000 -> CXL"
    range_m = re.search(r'0x([0-9a-fA-F]+)-0x([0-9a-fA-F]+)\s*->\s*CXL', line)
    single_m = re.search(r'0x([0-9a-fA-F]+)\s*->\s*CXL', line)
    if range_m:
        start = int(range_m.group(1), 16)
        end = int(range_m.group(2), 16)
        if start > end:
            start, end = end, start
        return (start, end)
    if single_m:
        va = int(single_m.group(1), 16)
        return (va, va + PAGE_SIZE)
    return None

def collect_cxl_ranges(lines, start_lineno, end_lineno):
    """Collect all CXL segments from lines[start_lineno:end_lineno); return [(start,end), ...] half-open."""
    ranges = []
    for i in range(start_lineno, min(end_lineno, len(lines))):
        t = parse_cxl_line(lines[i])
        if t:
            ranges.append(t)
    return ranges

def find_vmspace_blocks(lines, require_pagerank=False):
    """Find start/end line numbers of VMSPACE STATS blocks. Supports Process: /pagerank or Process: unknown, etc.
    Block range: from the Process: line until just before the next '=========='.
    require_pagerank: match only Process: /pagerank; otherwise match any Process: line.
    """
    blocks = []
    i = 0
    while i < len(lines):
        if '[VMSPACE STATS] Process:' in lines[i]:
            if require_pagerank and '/pagerank' not in lines[i]:
                i += 1
                continue
            start = i
            j = i + 1
            found_summary = False
            while j < len(lines):
                if '[VMSPACE STATS] Summary Statistics:' in lines[j]:
                    found_summary = True
                if found_summary and '[VMSPACE STATS] ==========================================' in lines[j]:
                    end = j
                    break
                j += 1
            else:
                end = len(lines)
            blocks.append((start, end))
            if not require_pagerank and len(blocks) >= 2:
                break
            if require_pagerank and len(blocks) == 2:
                break
            i = j
            continue
        i += 1
    return blocks

def find_pagerank_blocks(lines):
    """Legacy wrapper: find Process: /pagerank blocks."""
    return find_vmspace_blocks(lines, require_pagerank=True)

def merge_intervals(intervals):
    """Merge overlapping/adjacent intervals; return sorted non-overlapping [(s,e), ...] half-open."""
    if not intervals:
        return []
    sorted_i = sorted(intervals, key=lambda x: (x[0], x[1]))
    out = [list(sorted_i[0])]
    for s, e in sorted_i[1:]:
        if s <= out[-1][1]:
            out[-1][1] = max(out[-1][1], e)
        else:
            out.append([s, e])
    return [tuple(x) for x in out]

def subtract_intervals(outer, to_subtract):
    """Subtract to_subtract from outer. Both are [(s,e), ...] half-open.
    Return the parts of outer not covered by to_subtract (may split into multiple segments).
    """
    result = []
    for a, b in outer:
        current = [(a, b)]
        for cs, ce in to_subtract:
            next_current = []
            for (x, y) in current:
                if y <= cs or x >= ce:
                    next_current.append((x, y))
                else:
                    if x < cs:
                        next_current.append((x, min(y, cs)))
                    if ce < y:
                        next_current.append((max(x, ce), y))
            current = next_current
        result.extend(current)
    return merge_intervals(result)

def get_cxl_blocks_from_log(lines, single_block_ok=False):
    """Locate VMSPACE blocks in the log and return CXL interval lists.
    single_block_ok: if True, with only 1 block return (None, ranges); else return (None, None).
    """
    blocks = find_vmspace_blocks(lines, require_pagerank=False)
    if len(blocks) >= 2:
        (s1, e1), (s2, e2) = blocks[0], blocks[1]
        return collect_cxl_ranges(lines, s1, e1), collect_cxl_ranges(lines, s2, e2)
    if single_block_ok and len(blocks) == 1:
        s, e = blocks[0]
        return None, collect_cxl_ranges(lines, s, e)
    return None, None

def find_last_vmspace_block(lines):
    """Find start/end line numbers of the last [VMSPACE STATS] Process: ... block.

    Unlike find_vmspace_blocks, this does not stop after finding two blocks; it always keeps the last one.
    """
    last_block = None
    i = 0
    while i < len(lines):
        if '[VMSPACE STATS] Process:' in lines[i]:
            start = i
            j = i + 1
            found_summary = False
            while j < len(lines):
                if '[VMSPACE STATS] Summary Statistics:' in lines[j]:
                    found_summary = True
                if found_summary and '[VMSPACE STATS] ==========================================' in lines[j]:
                    end = j
                    break
                j += 1
            else:
                end = len(lines)
            last_block = (start, end)
            i = j
            continue
        i += 1
    return last_block

# Match name: [0x...-0x...) or name=[0x...-0x...); name may include [0], etc.
_DS_RANGE_PATTERN = re.compile(
    r'([a-zA-Z_][a-zA-Z0-9_]*(?:\[\d+\])?)\s*[:=]\s*\[\s*0x([0-9a-fA-F]+)\s*-\s*0x([0-9a-fA-F]+)\s*\)',
    re.MULTILINE
)

def parse_pr_line(line):
    """Parse name: [0xstart-0xend) or name=[0xstart-0xend) from a [PR][part=0]-style line.
    Return [(name, start_va, end_va), ...], half-open intervals.
    """
    result = []
    for m in _DS_RANGE_PATTERN.finditer(line):
        name, start_s, end_s = m.group(1), m.group(2), m.group(3)
        start_va = int(start_s, 16)
        end_va = int(end_s, 16)
        result.append((name, start_va, end_va))
    return result

def find_pr_line_in_log(lines, part=0):
    """Find a [PR][part=N] line that contains VA ranges in the log line list.

    Prefer a line that parse_pr_line can extract at least one [0x...-0x...) from;
    if none have VAs, fall back to the first line containing [PR][part=N]; else None.
    """
    prefix = f'[PR][part={part}]'
    first_match = None
    for line in lines:
        if prefix not in line:
            continue
        if first_match is None:
            first_match = line
        # If this line already has VA ranges, use it
        if parse_pr_line(line):
            return line
    return first_match

def collect_all_ds_ranges(lines):
    """Collect data-structure VA ranges from all [PR][part=N] lines in the log.

    Return [(name, start_va, end_va), ...]; name is kept as-is (e.g. may include [0]).
    """
    all_ds = []
    for line in lines:
        if '[PR][part=' not in line:
            continue
        parsed = parse_pr_line(line)
        if parsed:
            all_ds.extend(parsed)
    return all_ds

def accumulate_ds_original_sizes(ds_list):
    """Sum each data structure's 'original size' from its own VA ranges.

    ds_list: [(name, start_va, end_va), ...]
    Return: { name: total_size_bytes }
    """
    totals = {}
    for name, start_va, end_va in ds_list:
        size = max(0, end_va - start_va)
        totals[name] = totals.get(name, 0) + size
    return totals

def overlaps(seg_start, seg_end, ds_start, ds_end):
    """Whether two half-open intervals overlap."""
    return seg_start < ds_end and seg_end > ds_start

def format_size(size_bytes):
    if size_bytes >= 1024 * 1024 * 1024:
        return f"{size_bytes / (1024**3):.2f} GB"
    if size_bytes >= 1024 * 1024:
        return f"{size_bytes / (1024**2):.2f} MB"
    if size_bytes >= 1024:
        return f"{size_bytes / 1024:.2f} KB"
    return f"{size_bytes} B"

def main():
    log0_path = 'exec_log0.log'
    single_mode = '--single' in sys.argv
    if '--single' in sys.argv:
        sys.argv.remove('--single')
    if len(sys.argv) >= 2:
        log0_path = sys.argv[1]

    with open(log0_path, 'r') as f:
        log0_lines = f.readlines()

    if single_mode:
        # Single-block mode: analyze only CXL segments from the last VMSPACE STATS dump
        last_block = find_last_vmspace_block(log0_lines)
        if last_block is None:
            print("No [VMSPACE STATS] Process: ... block found in log")
            sys.exit(1)
        s, e = last_block
        cxl1, cxl2 = None, collect_cxl_ranges(log0_lines, s, e)
    else:
        # Default mode: diff the first two VMSPACE STATS dumps (new CXL in the 2nd vs the 1st)
        cxl1, cxl2 = get_cxl_blocks_from_log(log0_lines, single_block_ok=False)
    if cxl2 is None:
        print("Need at least two [VMSPACE STATS] Process: ... blocks; or use --single for one block")
        sys.exit(1)

    # Single-block: analyze cxl2 directly; two-block: new_ranges = cxl2 - cxl1
    if cxl1 is None:
        new_ranges = merge_intervals(cxl2)
        title = "CXL segments (single-block mode: all CXL from this dump)"
    else:
        merged1 = merge_intervals(cxl1)
        merged2 = merge_intervals(cxl2)
        new_ranges = subtract_intervals(merged2, merged1)
        if not new_ranges:
            print("No new CXL segments (2nd dump has nothing beyond the 1st)")
            return
        title = "New CXL segments (CXL memory present in the 2nd dump but not the 1st)"

    # Parse data-structure VA ranges from all [PR][part=N] lines in the same exec_log0.log
    ds_list = collect_all_ds_ranges(log0_lines)
    if not ds_list:
        print("(No parseable data-structure VA ranges in log; [PR][part=*] lines may be missing or mismatched)")
    ds_original_totals = accumulate_ds_original_sizes(ds_list) if ds_list else {}

    total_new_bytes = sum(e - s for s, e in new_ranges)
    print("=" * 60)
    print(title)
    print("=" * 60)
    print(f"Total new size: {format_size(total_new_bytes)} ({total_new_bytes} bytes, {total_new_bytes // PAGE_SIZE} pages)")
    print()

    # Sort CXL segments by size descending
    new_ranges = sorted(new_ranges, key=lambda r: r[1] - r[0], reverse=True)

    # Aggregate size by possible data structure (printed at the end)
    ds_totals = {}
    for start, end in new_ranges:
        size = end - start
        matching_ds = [
            name for name, ds_start, ds_end in ds_list
            if overlaps(start, end, ds_start, ds_end)
        ]
        for name in matching_ds:
            ds_totals[name] = ds_totals.get(name, 0) + size
        if not matching_ds:
            ds_totals.setdefault("(no match)", 0)
            ds_totals["(no match)"] += size

    for start, end in new_ranges:
        size = end - start
        # Match exec_log0: single page prints start only; multi-page prints start-end
        if size <= PAGE_SIZE:
            va_str = f"0x{start:x}-0x{end:x}" if size == PAGE_SIZE else f"0x{start:x}"
        else:
            va_str = f"0x{start:x}-0x{end:x}"
        matching_ds = [
            name for name, ds_start, ds_end in ds_list
            if overlaps(start, end, ds_start, ds_end)
        ]
        ds_str = ", ".join(matching_ds) if matching_ds else "(no matching data structure)"
        print(f"[CXL segment] {va_str} -> CXL")
        print(f"  size: {format_size(size)} ({size} bytes)")
        print(f"  VA range: [{hex(start)}, {hex(end)})")
        print(f"  possible data structures: {ds_str}")
        print()

    print("=" * 60)
    print("CXL size by data structure (compact, with ratio)")
    print("=" * 60)
    for name in sorted(ds_totals.keys(), key=lambda n: (n == "(no match)", n)):
        added_bytes = ds_totals[name]
        original_bytes = ds_original_totals.get(name, 0)
        if original_bytes > 0:
            ratio = added_bytes / original_bytes * 100.0
            ratio_str = f"{ratio:.2f}%"
        else:
            ratio_str = "N/A"
        original_human = format_size(original_bytes)
        added_human = format_size(added_bytes)
        print(
            f"{name}: "
            f"orig={original_human} ({original_bytes}B), "
            f"added={added_human} ({added_bytes}B), "
            f"ratio={ratio_str}"
        )

if __name__ == '__main__':
    main()
