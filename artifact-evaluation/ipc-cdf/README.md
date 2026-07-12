# IPC CDF artifact script

This directory contains the end-to-end artifact script for the first IPC test
used by the paper evaluation: local direct IPC vs cross-machine polling IPC.

Run from the repository root:

```bash
./artifact-evaluation/ipc-cdf/run.sh
```

The script:

1. Temporarily enables client breakdown and server timing instrumentation.
2. Rebuilds ChCore.
3. Boots two QEMU machines through the existing DSM/ivshmem setup.
4. Runs:
   - `direct_empty`
   - `direct`
   - `cross_empty`
   - `cross`
   - `cross_empty_4t`
   - `cross_4t`
5. Parses the QEMU logs.
6. Writes CSV files and figures under `artifact-evaluation/ipc-cdf/out/<timestamp>/`.

Useful outputs:

- `logs/machine0.log`, `logs/machine1.log`: raw logs from this run.
- `results/summary.csv`: p50/p75/p90/p99/max latency summary in us.
- `results/cdf.csv`: per-sample CDF points in us.
- `results/breakdown.csv`: median client-side breakdown in us.
- `results/server_timing.csv`: median server dequeue/handle timing in us.
- `figures/ipc_cdf.pdf`: CDF figure.
- `figures/ipc_read_breakdown.pdf`: Read 4KiB median breakdown figure.

To parse an existing run without rerunning QEMU:

```bash
python3 artifact-evaluation/ipc-cdf/parse_and_plot.py \
  --log-dir artifact-evaluation/ipc-cdf/out/<timestamp>/logs \
  --out-dir artifact-evaluation/ipc-cdf/out/<timestamp>
```
