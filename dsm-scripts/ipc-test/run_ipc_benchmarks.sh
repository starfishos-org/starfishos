#!/bin/bash
# Comprehensive IPC benchmark runner with automatic flag management
# Runs tests with different timing output configurations

set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

# Configuration
NUM_THREADS=${1:-3}
TESTS=("cdf_only" "breakdown" "srv_timing")

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}=== IPC Benchmark Suite ===${NC}"
echo "Repo: $REPO_ROOT"
echo "Threads: $NUM_THREADS"
echo ""

# Function to set flag in file
set_flag() {
    local file=$1
    local flag_name=$2
    local flag_value=$3

    if grep -q "#define $flag_name " "$file"; then
        sed -i "s/#define $flag_name .*/#define $flag_name $flag_value/" "$file"
        echo "  ✓ Set $flag_name=$flag_value in $(basename $file)"
    else
        echo "  ✗ Flag $flag_name not found in $file"
        exit 1
    fi
}

# Function to run a benchmark configuration
run_benchmark() {
    local test_name=$1
    local enable_breakdown=$2
    local enable_srv_timing=$3

    echo ""
    echo -e "${BLUE}[Test] $test_name${NC}"
    echo "  ENABLE_BREAKDOWN=$enable_breakdown"
    echo "  ENABLE_SRV_TIMING=$enable_srv_timing"

    # Set flags
    set_flag "user/system-servers/polling/polling_client_test.c" "ENABLE_BREAKDOWN" "$enable_breakdown"
    set_flag "user/system-servers/polling/polling_server.c" "ENABLE_SRV_TIMING" "$enable_srv_timing"

    # Compile
    echo -e "${BLUE}  [Compiling]${NC}"
    ./chbuild build 2>&1 | grep -E "polling|Built|error" | head -10

    # Run test
    echo -e "${BLUE}  [Running test]${NC}"
    ./dsm-scripts/ipc-test/test_polling_cross.sh $NUM_THREADS 2>&1 | tail -20

    # Copy logs with test-specific name
    cp exec_log0.log "exec_log_${test_name}_0.log" 2>/dev/null || true
    cp exec_log1.log "exec_log_${test_name}_1.log" 2>/dev/null || true

    echo -e "${GREEN}  ✓ Test complete${NC}"
}

# Run all configurations
echo -e "${BLUE}=== Running Benchmarks ===${NC}"

# Test 1: CDF only (minimal output)
run_benchmark "cdf_only" "0" "0"

# Test 2: With breakdown (detailed client-side)
run_benchmark "breakdown" "1" "0"

# Test 3: With server timing (requires rebuild)
run_benchmark "srv_timing" "1" "1"

echo ""
echo -e "${BLUE}=== Analysis ===${NC}"

# Analyze all logs
for test_name in "${TESTS[@]}"; do
    log0="exec_log_${test_name}_0.log"
    if [ -f "$log0" ]; then
        echo -e "${BLUE}${test_name}:${NC}"
        grep "SUMMARY\|Test.*PASSED\|polling_client: done" "$log0" | tail -5
    fi
done

echo ""
echo -e "${BLUE}=== Generated Files ===${NC}"
ls -lh exec_log_*.log *.csv *.pdf *.png 2>/dev/null | tail -20 || echo "No output files yet"

echo ""
echo -e "${GREEN}=== Analysis Commands ===${NC}"
echo "python3 dsm-scripts/ipc-test/parse_polling_latency.py exec_log_cdf_only_0.log"
echo "python3 dsm-scripts/ipc-test/plot_cdf_all.py exec_log_cdf_only_0.log exec_log_cdf_only_1.log"
echo ""
echo "View detailed breakdown:"
echo "grep '\[BREAKDOWN\]' exec_log_breakdown_0.log | head -20"
echo ""
echo "View server timing:"
echo "grep '\[SRV_TIMING\]' exec_log_srv_timing_0.log | head -20"
