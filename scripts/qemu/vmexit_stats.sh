#!/bin/bash

# QEMU/KVM vmexit 开销统计脚本
# 使用方法:
#   ./vmexit_stats.sh [vm_id] [command]
#   例如: ./vmexit_stats.sh 0 "./build/simulate.sh 0"

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 检查是否以 root 权限运行（perf kvm 需要 root）
if [ "$EUID" -ne 0 ]; then 
    echo -e "${YELLOW}警告: perf kvm 通常需要 root 权限，建议使用 sudo 运行${NC}"
fi

# 检查 perf 是否可用
if ! command -v perf &> /dev/null; then
    echo -e "${RED}错误: perf 工具未安装${NC}"
    echo "请安装: sudo apt-get install linux-perf 或 sudo yum install perf"
    exit 1
fi

# 检查 KVM 是否可用
if [ ! -e /sys/module/kvm ]; then
    echo -e "${RED}错误: KVM 模块未加载${NC}"
    exit 1
fi

vm_id=${1:-0}
shift
command="$@"

if [ -z "$command" ]; then
    echo "使用方法: $0 [vm_id] [command]]"
    echo "示例: $0 0 './build/simulate.sh 0'"
    exit 1
fi

output_dir="/tmp/vmexit_stats_$$"
mkdir -p "$output_dir"

echo -e "${GREEN}开始统计 vmexit 开销...${NC}"
echo "VM ID: $vm_id"
echo "命令: $command"
echo "输出目录: $output_dir"
echo ""

# 方法1: 使用 perf kvm stat 实时统计
echo -e "${GREEN}方法1: 使用 perf kvm stat 实时统计${NC}"
echo "在另一个终端运行以下命令查看实时统计:"
echo -e "${YELLOW}sudo perf kvm stat live${NC}"
echo ""

# 方法2: 使用 perf kvm stat record 记录并分析
echo -e "${GREEN}方法2: 记录 vmexit 事件并分析${NC}"

# 启动 perf kvm stat record 在后台
perf_pid_file="$output_dir/perf.pid"
perf_data_file="$output_dir/perf.data"

# 启动 perf kvm stat record
sudo perf kvm stat record -o "$perf_data_file" > "$output_dir/perf_record.log" 2>&1 &
perf_pid=$!
echo $perf_pid > "$perf_pid_file"
echo "Perf 记录进程 PID: $perf_pid"

# 等待一下让 perf 启动
sleep 1

# 执行用户命令
echo -e "${GREEN}执行命令: $command${NC}"
eval "$command" &
cmd_pid=$!

# 等待命令完成
wait $cmd_pid
cmd_exit_code=$?

# 停止 perf 记录
echo "停止 perf 记录..."
sudo kill -INT $perf_pid 2>/dev/null || true
wait $perf_pid 2>/dev/null || true

sleep 1

# 生成报告
echo ""
echo -e "${GREEN}生成 vmexit 统计报告...${NC}"

# 方法2a: 使用 perf kvm stat report 生成报告
if [ -f "$perf_data_file" ]; then
    echo ""
    echo -e "${GREEN}=== vmexit 统计报告 ===${NC}"
    sudo perf kvm stat report -i "$perf_data_file" > "$output_dir/vmexit_report.txt" 2>&1
    cat "$output_dir/vmexit_report.txt"
    echo ""
    echo "详细报告已保存到: $output_dir/vmexit_report.txt"
else
    echo -e "${YELLOW}警告: perf.data 文件未生成${NC}"
fi

# 方法3: 使用 perf stat 统计 KVM tracepoints
echo ""
echo -e "${GREEN}方法3: 使用 perf stat 统计 KVM tracepoints${NC}"
echo "统计 KVM 相关事件..."

# 列出可用的 KVM tracepoints
kvm_events=$(sudo perf list | grep -i kvm || echo "")

if [ -n "$kvm_events" ]; then
    echo "可用的 KVM 事件:"
    echo "$kvm_events" | head -20
    echo ""
    
    # 统计常见的 vmexit 相关事件
    echo -e "${GREEN}统计常见 vmexit 事件:${NC}"
    sudo perf stat -e kvm:kvm_exit,kvm:kvm_entry,kvm:kvm_vcpu_wakeup \
        -o "$output_dir/perf_stat.txt" \
        sleep 0.1 2>&1 || true
    
    # 如果命令还在运行，可以附加统计
    if [ $cmd_exit_code -eq 0 ]; then
        echo "命令执行完成，退出码: $cmd_exit_code"
    fi
else
    echo -e "${YELLOW}警告: 未找到 KVM tracepoints，可能需要加载 debugfs${NC}"
    echo "尝试: sudo mount -t debugfs none /sys/kernel/debug"
fi

# 方法4: 使用 /sys/kernel/debug/tracing (如果可用)
echo ""
echo -e "${GREEN}方法4: 使用 ftrace 统计 (如果可用)${NC}"
if [ -d /sys/kernel/debug/tracing ]; then
    echo "ftrace 可用，可以手动使用以下命令:"
    echo "  echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable"
    echo "  cat /sys/kernel/debug/tracing/trace_pipe"
else
    echo -e "${YELLOW}ftrace 不可用，需要挂载 debugfs${NC}"
fi

echo ""
echo -e "${GREEN}=== 统计完成 ===${NC}"
echo "所有输出文件保存在: $output_dir"
echo ""
echo "查看详细报告:"
echo "  cat $output_dir/vmexit_report.txt"
echo ""
echo "使用 perf kvm stat report 分析:"
echo "  sudo perf kvm stat report -i $perf_data_file"
echo ""
echo "使用 perf script 查看详细事件:"
echo "  sudo perf script -i $perf_data_file | grep -i exit | head -20"

exit $cmd_exit_code





