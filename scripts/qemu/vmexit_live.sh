#!/bin/bash

# 实时监控 vmexit 开销的脚本
# 使用方法: ./vmexit_live.sh [interval]
# interval: 刷新间隔（秒），默认 1 秒

set -e

interval=${1:-1}

# 检查是否以 root 权限运行
if [ "$EUID" -ne 0 ]; then 
    echo "错误: 此脚本需要 root 权限"
    echo "请使用: sudo $0"
    exit 1
fi

# 检查 perf 是否可用
if ! command -v perf &> /dev/null; then
    echo "错误: perf 工具未安装"
    exit 1
fi

echo "实时监控 vmexit 开销 (刷新间隔: ${interval}秒)"
echo "按 Ctrl+C 停止"
echo ""

# 使用 perf kvm stat live 实时显示
perf kvm stat live --interval $interval





