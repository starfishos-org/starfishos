# IPC Benchmark Quick Start

## One-Command Benchmarks

### Quick Test (2 configs, CDF + Breakdown)
```bash
./dsm-scripts/ipc-test/quick_benchmark.sh 3
```
**Output**: Two sets of logs (`exec_log_cdf_only_*`, `exec_log_breakdown_*`)

### Full Test (3 configs, CDF + Breakdown + Server Timing)
```bash
./dsm-scripts/ipc-test/run_ipc_benchmarks.sh 3
```
**Output**: Three sets of logs with detailed analysis

---

## Manual Configuration (Advanced)

### View current flags
```bash
python3 dsm-scripts/ipc-test/configure_timing.py --list
```

### Set specific configuration
```bash
# CDF only (minimal)
python3 dsm-scripts/ipc-test/configure_timing.py --breakdown 0 --srv-timing 0

# With breakdown analysis
python3 dsm-scripts/ipc-test/configure_timing.py --breakdown 1 --srv-timing 0

# Full instrumentation
python3 dsm-scripts/ipc-test/configure_timing.py --breakdown 1 --srv-timing 1
```

Then compile and run:
```bash
./chbuild build
./dsm-scripts/ipc-test/test_polling_cross.sh 3
```

---

## Analyze Results

### View CDF data
```bash
grep '\[CDF\]' exec_log0.log | head -20
```

### View breakdown (alloc/enqueue/wait)
```bash
grep '\[BD\]' exec_log0.log | head -20
```

### View server-side timing
```bash
grep '\[SRV_TIM\]' exec_log0.log | head -20
```

### Generate plots
```bash
python3 dsm-scripts/ipc-test/plot_cdf_all.py exec_log0.log exec_log1.log
```

---

## Output Files

After running benchmarks:

| File | Contains |
|------|----------|
| `exec_log_cdf_only_0.log` | CDF data only |
| `exec_log_breakdown_0.log` | CDF + breakdown details |
| `exec_log_srv_timing_0.log` | CDF + breakdown + server timing |
| `*.pdf` | CDF plots |
| `*.csv` | Tabular data |

---

## Four IPC Modes

All benchmarks test these IPC approaches:

1. **direct** - Direct syscall read (baseline)
2. **direct_empty** - Empty syscall (pure IPC overhead)
3. **local** - Local polling queue (same machine)
4. **cross** - Cross-machine polling queue

Expected output:
```
[SUMMARY] mode=direct total=3000 threads=3 breakdown=0
[SUMMARY] p50=... p75=... p90=... p99=... max=... (cycles)
```

---

## Flags Explained

| Flag | Default | Effect |
|------|---------|--------|
| `ENABLE_BREAKDOWN` | 0 | Output `[BREAKDOWN_BEGIN/END]` with per-component times |
| `ENABLE_SRV_TIMING` | 0 | Collect server-side dequeue/handle timing |

- `ENABLE_BREAKDOWN=0`: Only CDF data (lightweight)
- `ENABLE_BREAKDOWN=1`: CDF + component breakdown (alloc/enqueue/wait)
- `ENABLE_SRV_TIMING=1`: Also collect server-side latencies

---

## Troubleshooting

### "No output files" after benchmark
- Check `exec_log0.log` for errors with `tail exec_log0.log`
- Ensure ivshmem-server is running: `ps aux | grep ivshmem`

### Plots not generated
- Ensure matplotlib is installed: `pip3 install matplotlib`
- Run analysis scripts manually: `python3 dsm-scripts/ipc-test/plot_cdf_all.py exec_log0.log exec_log1.log`

### Recompiling takes too long
- Use `quick_benchmark.sh` which rebuilds only when needed
- Or manually set flags with `configure_timing.py` and `./chbuild build`

---

## Complete Example Workflow

```bash
# Step 1: Quick test with both configurations
./dsm-scripts/ipc-test/quick_benchmark.sh 3

# Step 2: Check CDF comparison
python3 dsm-scripts/ipc-test/plot_cdf_all.py exec_log_cdf_only_0.log exec_log_cdf_only_1.log

# Step 3: View breakdown stats
echo "=== Breakdown Stats ===" && grep 'SUMMARY' exec_log_breakdown_0.log | grep p50

# Step 4: Compare all modes
echo "=== Mode Comparison ===" && grep 'mode=' exec_log_*_0.log | grep SUMMARY

# Step 5: Optional - detailed server timing
./dsm-scripts/ipc-test/configure_timing.py --breakdown 1 --srv-timing 1
./chbuild build
./dsm-scripts/ipc-test/test_polling_cross.sh 3
grep '[SRV_TIM]' exec_log0.log | head -20
```
