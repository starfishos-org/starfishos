#!/bin/bash

# Live monitoring script for vmexit overhead
# Usage: ./vmexit_live.sh [interval]
# interval: refresh interval in seconds (default: 1)

set -e

interval=${1:-1}

# Check whether running as root
if [ "$EUID" -ne 0 ]; then 
    echo "Error: this script requires root privileges"
    echo "Please run: sudo $0"
    exit 1
fi

# Check whether perf is available
if ! command -v perf &> /dev/null; then
    echo "Error: perf tool is not installed"
    exit 1
fi

echo "Live monitoring vmexit overhead (refresh interval: ${interval}s)"
echo "Press Ctrl+C to stop"
echo ""

# Use perf kvm stat live for real-time display
perf kvm stat live --interval $interval
