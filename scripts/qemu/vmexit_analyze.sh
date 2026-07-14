#!/bin/bash

# Analyze recorded vmexit data
# Usage: ./vmexit_analyze.sh [perf_data_file]

set -e

perf_data_file=${1:-/tmp/vmexit_stats_*/perf.data}

if [ ! -f "$perf_data_file" ]; then
    echo "Error: perf.data file does not exist: $perf_data_file"
    echo ""
    echo "Usage: $0 [perf_data_file]"
    echo "Or run vmexit_stats.sh first to generate data"
    exit 1
fi

# Check whether running as root
if [ "$EUID" -ne 0 ]; then 
    echo "Warning: some operations may require root privileges"
fi

echo "Analyzing vmexit data: $perf_data_file"
echo ""

# Generate statistics report
echo "=== vmexit statistics summary ==="
sudo perf kvm stat report -i "$perf_data_file" 2>&1 | head -50
echo ""

# Show top vmexit reasons
echo "=== Top vmexit reasons ==="
sudo perf kvm stat report -i "$perf_data_file" 2>&1 | grep -A 20 "VM-EXIT" || \
sudo perf kvm stat report -i "$perf_data_file" 2>&1 | grep -i exit
echo ""

# Use perf script to inspect detailed events
echo "=== Detailed vmexit events (first 20) ==="
sudo perf script -i "$perf_data_file" 2>&1 | grep -i "kvm_exit\|vmexit" | head -20 || \
echo "No detailed events found; you may need to record with perf kvm stat record"
echo ""

# Try using perf report
if command -v perf &> /dev/null; then
    echo "=== Analyze with perf report ==="
    echo "Run: sudo perf report -i $perf_data_file"
    echo "Or: sudo perf kvm stat report -i $perf_data_file"
fi
