#!/usr/bin/env python3
"""
解析 exec_log0.log 中两次 VMSPACE STATS 的 CXL 状态，只关心「第 2 次比第 1 次多的」CXL 段；
数据结构 VA 直接来自同一文件中的 [PR][part=0] 行（如第 417 行），输出：新增 CXL 的大小、
VA 区间、可能落在的数据结构。输出格式与 exec_log0.log 一致：具体的 CXL 段
（如 0x3007d2f07000-0x3007d2f0c000 -> CXL），而不是整块 VMR。
"""

import re
import sys

PAGE_SIZE = 0x1000  # 4KB

def parse_cxl_line(line):
    """解析一行 '   VA或VA-VA -> CXL'，返回 (start_va, end_va) 左闭右开，或 None。"""
    line = line.strip()
    if ' -> CXL' not in line or 'machines:' in line:
        return None
    # 格式: "0x3007d2f07000-0x3007d2f0c000 -> CXL" 或 "0x3007d2f07000 -> CXL"
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
    """从 lines 的 [start_lineno, end_lineno) 中收集所有 CXL 段，返回 [(start,end), ...] 左闭右开。"""
    ranges = []
    for i in range(start_lineno, min(end_lineno, len(lines))):
        t = parse_cxl_line(lines[i])
        if t:
            ranges.append(t)
    return ranges

def find_vmspace_blocks(lines, require_pagerank=False):
    """找到 VMSPACE STATS 块的起止行号。支持 Process: /pagerank 或 Process: unknown 等。
    段范围：从 Process: 所在行到下一个 '==========' 之前。
    require_pagerank: 仅匹配 Process: /pagerank；否则匹配任意 Process: 行。
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
    """兼容旧接口：找 Process: /pagerank 的块。"""
    return find_vmspace_blocks(lines, require_pagerank=True)

def merge_intervals(intervals):
    """合并重叠/相邻的区间，返回有序不重叠的 [(s,e), ...] 左闭右开。"""
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
    """从 outer 区间列表中减去 to_subtract。outer 和 to_subtract 均为 [(s,e), ...] 左闭右开。
    返回 outer 中不在 to_subtract 内的部分（可能切成多段）。
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
    """从 log 定位 VMSPACE 块并返回 CXL 区间列表。
    single_block_ok: 若为 True，仅 1 个块时也返回 (None, ranges)；否则返回 (None, None)。
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
    """找到最后一次出现的 [VMSPACE STATS] Process: ... 块的起止行号。

    与 find_vmspace_blocks 不同，这里不会在找到两个块后提前退出，而是始终保留最后一个块。
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

# 匹配 name: [0x...-0x...) 或 name=[0x...-0x...)，name 可含 [0] 等
_DS_RANGE_PATTERN = re.compile(
    r'([a-zA-Z_][a-zA-Z0-9_]*(?:\[\d+\])?)\s*[:=]\s*\[\s*0x([0-9a-fA-F]+)\s*-\s*0x([0-9a-fA-F]+)\s*\)',
    re.MULTILINE
)

def parse_pr_line(line):
    """解析 [PR][part=0] 这类行中的 name: [0xstart-0xend) 或 name=[0xstart-0xend)。
    返回 [(name, start_va, end_va), ...]，区间为左闭右开。
    """
    result = []
    for m in _DS_RANGE_PATTERN.finditer(line):
        name, start_s, end_s = m.group(1), m.group(2), m.group(3)
        start_va = int(start_s, 16)
        end_va = int(end_s, 16)
        result.append((name, start_va, end_va))
    return result

def find_pr_line_in_log(lines, part=0):
    """在 log 行列表中查找 [PR][part=N] 且包含 VA 区间的行。

    优先返回能被 parse_pr_line 解析出至少一个 [0x...-0x...) 的那一行；
    若没有带 VA 的行，则退化为返回第一条包含 [PR][part=N] 的行；都没有则返回 None。
    """
    prefix = f'[PR][part={part}]'
    first_match = None
    for line in lines:
        if prefix not in line:
            continue
        if first_match is None:
            first_match = line
        # 这一行如果已经有 VA 区间，就直接用它
        if parse_pr_line(line):
            return line
    return first_match

def collect_all_ds_ranges(lines):
    """在 log 中收集所有 [PR][part=N] 行里的数据结构 VA 区间。

    返回 [(name, start_va, end_va), ...]，其中 name 不做额外拼接（保持原样，如含 [0] 等）。
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
    """根据数据结构自己的 VA 区间，统计每个数据结构的「原始大小」总和。

    ds_list: [(name, start_va, end_va), ...]
    返回: { name: total_size_bytes }
    """
    totals = {}
    for name, start_va, end_va in ds_list:
        size = max(0, end_va - start_va)
        totals[name] = totals.get(name, 0) + size
    return totals

def overlaps(seg_start, seg_end, ds_start, ds_end):
    """区间是否重叠（左闭右开）。"""
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
        # 单块模式：只分析「最后一次」 VMSPACE STATS 的 CXL 段
        last_block = find_last_vmspace_block(log0_lines)
        if last_block is None:
            print("log 中未找到任何 [VMSPACE STATS] Process: ... 块")
            sys.exit(1)
        s, e = last_block
        cxl1, cxl2 = None, collect_cxl_ranges(log0_lines, s, e)
    else:
        # 默认模式：分析「前两次」 VMSPACE STATS 的差分（第 2 次相较第 1 次的新增 CXL）
        cxl1, cxl2 = get_cxl_blocks_from_log(log0_lines, single_block_ok=False)
    if cxl2 is None:
        print("需要至少两段 [VMSPACE STATS] Process: ... 块；或使用 --single 分析单块")
        sys.exit(1)

    # 单块模式：直接分析 cxl2 的 CXL 段；双块模式：分析 new_ranges = cxl2 - cxl1
    if cxl1 is None:
        new_ranges = merge_intervals(cxl2)
        title = "CXL 段（单块模式：本次打印的全部 CXL）"
    else:
        merged1 = merge_intervals(cxl1)
        merged2 = merge_intervals(cxl2)
        new_ranges = subtract_intervals(merged2, merged1)
        if not new_ranges:
            print("没有新增的 CXL 段（第 2 次相对第 1 次无新增）")
            return
        title = "新增 CXL 段（第 2 次比第 1 次多的 CXL 内存）"

    # 从同一 exec_log0.log 中的所有 [PR][part=N] 行解析数据结构 VA 区间
    ds_list = collect_all_ds_ranges(log0_lines)
    if not ds_list:
        print("(未在 log 中找到可解析的数据结构 VA 区间，[PR][part=*] 行可能缺失或格式不匹配)")
    ds_original_totals = accumulate_ds_original_sizes(ds_list) if ds_list else {}

    total_new_bytes = sum(e - s for s, e in new_ranges)
    print("=" * 60)
    print(title)
    print("=" * 60)
    print(f"新增总大小: {format_size(total_new_bytes)} ({total_new_bytes} bytes, {total_new_bytes // PAGE_SIZE} pages)")
    print()

    # 按 CXL 段大小降序排序
    new_ranges = sorted(new_ranges, key=lambda r: r[1] - r[0], reverse=True)

    # 按可能数据结构汇总大小（用于最后打印）
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
            ds_totals.setdefault("(无匹配)", 0)
            ds_totals["(无匹配)"] += size

    for start, end in new_ranges:
        size = end - start
        # 与 exec_log0 一致：单页只打 start，多页打 start-end
        if size <= PAGE_SIZE:
            va_str = f"0x{start:x}-0x{end:x}" if size == PAGE_SIZE else f"0x{start:x}"
        else:
            va_str = f"0x{start:x}-0x{end:x}"
        matching_ds = [
            name for name, ds_start, ds_end in ds_list
            if overlaps(start, end, ds_start, ds_end)
        ]
        ds_str = ", ".join(matching_ds) if matching_ds else "(无匹配数据结构)"
        print(f"[CXL 段] {va_str} -> CXL")
        print(f"  大小: {format_size(size)} ({size} bytes)")
        print(f"  VA 区间: [{hex(start)}, {hex(end)})")
        print(f"  可能数据结构: {ds_str}")
        print()

    print("=" * 60)
    print("按数据结构汇总的 CXL 大小（紧凑显示，含占比）")
    print("=" * 60)
    for name in sorted(ds_totals.keys(), key=lambda n: (n == "(无匹配)", n)):
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
