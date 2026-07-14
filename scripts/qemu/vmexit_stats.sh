#!/bin/bash

# QEMU/KVM vmexit overhead statistics script
# Usage:
#   ./vmexit_stats.sh [vm_id] [command]
#   Example: ./vmexit_stats.sh 0 "./build/simulate.sh 0"

set -e

# Colored output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check whether running as root (perf kvm typically needs root)
if [ "$EUID" -ne 0 ]; then 
    echo -e "${YELLOW}Warning: perf kvm usually requires root; consider running with sudo${NC}"
fi

# Check whether perf is available
if ! command -v perf &> /dev/null; then
    echo -e "${RED}Error: perf tool is not installed${NC}"
    echo "Install with: sudo apt-get install linux-perf or sudo yum install perf"
    exit 1
fi

# Check whether KVM is available
if [ ! -e /sys/module/kvm ]; then
    echo -e "${RED}Error: KVM module is not loaded${NC}"
    exit 1
fi

vm_id=${1:-0}
shift
command="$@"

if [ -z "$command" ]; then
    echo "Usage: $0 [vm_id] [command]]"
    echo "Example: $0 0 './build/simulate.sh 0'"
    exit 1
fi

output_dir="/tmp/vmexit_stats_$$"
mkdir -p "$output_dir"

echo -e "${GREEN}Starting vmexit overhead statistics...${NC}"
echo "VM ID: $vm_id"
echo "Command: $command"
echo "Output directory: $output_dir"
echo ""

# Method 1: live stats with perf kvm stat
echo -e "${GREEN}Method 1: live stats with perf kvm stat${NC}"
echo "In another terminal, run the following for live stats:"
echo -e "${YELLOW}sudo perf kvm stat live${NC}"
echo ""

# Method 2: record with perf kvm stat record and analyze
echo -e "${GREEN}Method 2: record vmexit events and analyze${NC}"

# Start perf kvm stat record in the background
perf_pid_file="$output_dir/perf.pid"
perf_data_file="$output_dir/perf.data"

# Start perf kvm stat record
sudo perf kvm stat record -o "$perf_data_file" > "$output_dir/perf_record.log" 2>&1 &
perf_pid=$!
echo $perf_pid > "$perf_pid_file"
echo "Perf record process PID: $perf_pid"

# Wait briefly for perf to start
sleep 1

# Run the user command
echo -e "${GREEN}Running command: $command${NC}"
eval "$command" &
cmd_pid=$!

# Wait for the command to finish
wait $cmd_pid
cmd_exit_code=$?

# Stop perf recording
echo "Stopping perf recording..."
sudo kill -INT $perf_pid 2>/dev/null || true
wait $perf_pid 2>/dev/null || true

sleep 1

# Generate report
echo ""
echo -e "${GREEN}Generating vmexit statistics report...${NC}"

# Method 2a: generate report with perf kvm stat report
if [ -f "$perf_data_file" ]; then
    echo ""
    echo -e "${GREEN}=== vmexit statistics report ===${NC}"
    sudo perf kvm stat report -i "$perf_data_file" > "$output_dir/vmexit_report.txt" 2>&1
    cat "$output_dir/vmexit_report.txt"
    echo ""
    echo "Detailed report saved to: $output_dir/vmexit_report.txt"
else
    echo -e "${YELLOW}Warning: perf.data file was not generated${NC}"
fi

# Method 3: count KVM tracepoints with perf stat
echo ""
echo -e "${GREEN}Method 3: count KVM tracepoints with perf stat${NC}"
echo "Counting KVM-related events..."

# List available KVM tracepoints
kvm_events=$(sudo perf list | grep -i kvm || echo "")

if [ -n "$kvm_events" ]; then
    echo "Available KVM events:"
    echo "$kvm_events" | head -20
    echo ""
    
    # Count common vmexit-related events
    echo -e "${GREEN}Counting common vmexit events:${NC}"
    sudo perf stat -e kvm:kvm_exit,kvm:kvm_entry,kvm:kvm_vcpu_wakeup \
        -o "$output_dir/perf_stat.txt" \
        sleep 0.1 2>&1 || true
    
    # If the command finished, report its exit code
    if [ $cmd_exit_code -eq 0 ]; then
        echo "Command finished, exit code: $cmd_exit_code"
    fi
else
    echo -e "${YELLOW}Warning: no KVM tracepoints found; you may need to mount debugfs${NC}"
    echo "Try: sudo mount -t debugfs none /sys/kernel/debug"
fi

# Method 4: use /sys/kernel/debug/tracing (if available)
echo ""
echo -e "${GREEN}Method 4: count with ftrace (if available)${NC}"
if [ -d /sys/kernel/debug/tracing ]; then
    echo "ftrace is available; you can manually use:"
    echo "  echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable"
    echo "  cat /sys/kernel/debug/tracing/trace_pipe"
else
    echo -e "${YELLOW}ftrace is unavailable; mount debugfs first${NC}"
fi

echo ""
echo -e "${GREEN}=== Statistics complete ===${NC}"
echo "All output files are under: $output_dir"
echo ""
echo "View detailed report:"
echo "  cat $output_dir/vmexit_report.txt"
echo ""
echo "Analyze with perf kvm stat report:"
echo "  sudo perf kvm stat report -i $perf_data_file"
echo ""
echo "Inspect detailed events with perf script:"
echo "  sudo perf script -i $perf_data_file | grep -i exit | head -20"

exit $cmd_exit_code
