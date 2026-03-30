#!/bin/bash
# Quick benchmark: Run two configurations (CDF-only and breakdown) and compare

set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

NUM_THREADS=${1:-3}

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║   Quick IPC Benchmark (2 configs)      ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
echo ""
echo "Repo:    $REPO_ROOT"
echo "Threads: $NUM_THREADS"
echo ""

# ============================================================================
# Config 1: CDF Only (minimal output)
# ============================================================================
echo -e "${YELLOW}┌─ Config 1: CDF Only (no breakdown, no server timing) ─────┐${NC}"

python3 dsm-scripts/ipc-test/configure_timing.py \
    --breakdown 0 \
    --srv-timing 0

echo -e "${BLUE}[Build]${NC}"
./chbuild build 2>&1 | grep -i "polling_client\|Built\|error" | head -5

echo -e "${BLUE}[Run test]${NC}"
timeout 300 ./dsm-scripts/ipc-test/test_polling_cross.sh $NUM_THREADS 2>&1 | tail -15

cp exec_log0.log "exec_log_cdf_only_0.log" 2>/dev/null
cp exec_log1.log "exec_log_cdf_only_1.log" 2>/dev/null
echo -e "${GREEN}✓ Config 1 complete${NC}"

# ============================================================================
# Config 2: Breakdown (detailed client-side analysis)
# ============================================================================
echo ""
echo -e "${YELLOW}┌─ Config 2: Breakdown (detailed client analysis)──────────┐${NC}"

python3 dsm-scripts/ipc-test/configure_timing.py \
    --breakdown 1 \
    --srv-timing 0

echo -e "${BLUE}[Build]${NC}"
./chbuild build 2>&1 | grep -i "polling_client\|Built\|error" | head -5

echo -e "${BLUE}[Run test]${NC}"
timeout 300 ./dsm-scripts/ipc-test/test_polling_cross.sh $NUM_THREADS 2>&1 | tail -15

cp exec_log0.log "exec_log_breakdown_0.log" 2>/dev/null
cp exec_log1.log "exec_log_breakdown_1.log" 2>/dev/null
echo -e "${GREEN}✓ Config 2 complete${NC}"

# ============================================================================
# Results Summary
# ============================================================================
echo ""
echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║          Results Summary               ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"

echo ""
echo -e "${YELLOW}Config 1: CDF Only${NC}"
grep "SUMMARY.*mode" exec_log_cdf_only_0.log | head -3

echo ""
echo -e "${YELLOW}Config 2: Breakdown${NC}"
grep "SUMMARY.*mode" exec_log_breakdown_0.log | head -3

echo ""
echo -e "${BLUE}Generated Log Files:${NC}"
ls -lh exec_log_{cdf_only,breakdown}_*.log | awk '{print "  " $9 " (" $5 ")"}'

echo ""
echo -e "${BLUE}Analysis Commands:${NC}"
echo ""
echo -e "${YELLOW}1. Compare CDF data:${NC}"
echo "   python3 dsm-scripts/ipc-test/plot_cdf_all.py exec_log_cdf_only_0.log exec_log_cdf_only_1.log"

echo ""
echo -e "${YELLOW}2. View breakdown (alloc/enqueue/wait times):${NC}"
echo "   grep '[BD]' exec_log_breakdown_0.log | head -30"

echo ""
echo -e "${YELLOW}3. Compare mode-by-mode percentiles:${NC}"
echo "   echo 'Mode comparison:' && grep 'p50=' exec_log_*_0.log"

echo ""
echo -e "${GREEN}✓ Benchmark complete${NC}"
