#!/bin/bash

# 分析已记录的 vmexit 数据
# 使用方法: ./vmexit_analyze.sh [perf_data_file]

set -e

perf_data_file=${1:-/tmp/vmexit_stats_*/perf.data}

if [ ! -f "$perf_data_file" ]; then
    echo "错误: perf.data 文件不存在: $perf_data_file"
    echo ""
    echo "使用方法: $0 [perf_data_file]"
    echo "或者先运行 vmexit_stats.sh 生成数据"
    exit 1
fi

# 检查是否以 root 权限运行
if [ "$EUID" -ne 0 ]; then 
    echo "警告: 某些操作可能需要 root 权限"
fi

echo "分析 vmexit 数据: $perf_data_file"
echo ""

# 生成统计报告
echo "=== vmexit 统计摘要 ==="
sudo perf kvm stat report -i "$perf_data_file" 2>&1 | head -50
echo ""

# 显示 top vmexit 原因
echo "=== Top vmexit 原因 ==="
sudo perf kvm stat report -i "$perf_data_file" 2>&1 | grep -A 20 "VM-EXIT" || \
sudo perf kvm stat report -i "$perf_data_file" 2>&1 | grep -i exit
echo ""

# 使用 perf script 查看详细事件
echo "=== 详细 vmexit 事件 (前20个) ==="
sudo perf script -i "$perf_data_file" 2>&1 | grep -i "kvm_exit\|vmexit" | head -20 || \
echo "未找到详细事件，可能需要使用 perf kvm stat record 记录"
echo ""

# 尝试使用 perf report
if command -v perf &> /dev/null; then
    echo "=== 使用 perf report 分析 ==="
    echo "运行: sudo perf report -i $perf_data_file"
    echo "或: sudo perf kvm stat report -i $perf_data_file"
fi





