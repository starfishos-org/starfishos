# IPC Benchmark Timing Configuration

## Overview

Timing data collection is **always enabled** in both client and server for CDF (Cumulative Distribution Function) generation. Use flags to control **output verbosity**.

## Client-Side (polling_client_test.c)

```c
#define ENABLE_BREAKDOWN 0   // Set to 1 to output [BREAKDOWN_BEGIN/END]
```

**Behavior**:
- **ENABLE_BREAKDOWN=0** (default):
  - Always outputs: `[SUMMARY]` and `[CDF_BEGIN]...[CDF_END]`
  - Per-iteration timing is collected via `rdtsc()` for CDF calculation
  - No detailed breakdown data

- **ENABLE_BREAKDOWN=1**:
  - Outputs: `[SUMMARY]`, `[CDF]`, and `[BREAKDOWN_BEGIN]...[BREAKDOWN_END]`
  - Includes per-component breakdown: alloc time, enqueue time, wait time

## Server-Side (polling_server.c, polling_resp.c)

```c
#define ENABLE_SRV_TIMING 0   // Set to 1 to collect/dump per-request timing
```

**Behavior**:
- **ENABLE_SRV_TIMING=0** (default):
  - No dequeue/handle timing collection overhead
  - Minimal instrumentation in hot path

- **ENABLE_SRV_TIMING=1**:
  - Collects dequeue and handle timing (via `clock_gettime(CLOCK_MONOTONIC)`)
  - Stores up to 50k samples in global buffers
  - Dumped via `polling_print_debug_info()` (POLLING_PRINT_DEBUG_INFO request)

## Usage Examples

### Minimal (CDF only, no breakdown)
```bash
# Recompile with ENABLE_BREAKDOWN=0, ENABLE_SRV_TIMING=0
./dsm-scripts/ipc-test/test_polling_cross.sh 3
# Output: [SUMMARY] and [CDF] for analysis with plot_cdf_all.py
```

### Detailed (with breakdown, no server timing)
```bash
# Edit polling_client_test.c: #define ENABLE_BREAKDOWN 1
./chbuild build
./dsm-scripts/ipc-test/test_polling_cross.sh 3
# Output: [SUMMARY], [CDF], [BREAKDOWN] for component analysis
```

### With server-side timing
```bash
# Edit polling_server.c: #define ENABLE_SRV_TIMING 1
# Edit polling_resp.c: #define ENABLE_SRV_TIMING 1
./chbuild build
./dsm-scripts/ipc-test/test_polling_cross.sh 3
# Output: includes [SRV_TIMING_BEGIN/END] blocks with server stats
```

## Collected Data

### Client-side per-iteration (in memory, cycles via rdtsc):
- `t_total`: Full roundtrip (allocation → enqueue → wait)
- `t_alloc`: dq_alloc_node() time
- `t_enqueue`: dq_enqueue() time
- `t_wait`: dq_wait_for_done() time

### Server-side per-request (when ENABLE_SRV_TIMING=1, nanoseconds):
- `srv_t_deq[]`: durable_dequeue() latency
- `srv_t_handle[]`: handle_polling_request() latency

## Output Format

### Always (default):
```
[SUMMARY] mode=direct total=3000 threads=3 breakdown=0
[SUMMARY] p50=100 p75=150 p90=200 p99=500 max=5000 (cycles)
[CDF_BEGIN] mode=direct count=3000
[CDF] 0 50
[CDF] 1 75
...
[CDF_END]
```

### With ENABLE_BREAKDOWN=1:
```
[BREAKDOWN_BEGIN] mode=polling_empty count=3000
[BD] 1000 200 300 500   # t_total t_alloc t_enqueue t_wait
[BD] 1100 210 310 580
...
[BREAKDOWN_END]
```

### With ENABLE_SRV_TIMING=1:
```
[SRV_TIMING_BEGIN] count=50000
[SRV_TIM] 12345 98765   # dequeue_ns handle_ns
[SRV_TIM] 12456 99000
...
[SRV_TIMING_END]
```
