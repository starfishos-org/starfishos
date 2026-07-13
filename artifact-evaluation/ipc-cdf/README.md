# IPC CDF artifact script

This directory contains the end-to-end artifact script for the first IPC test
used by the paper evaluation: local direct IPC vs cross-machine polling IPC.

Run from the repository root:

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/ipc-cdf/run.sh
```

This reproduces paper **Figure 11** on the default 2-machine cluster. For the
reviewer-requested 8-machine variant (same workloads — direct IPC on machine
0, cross IPC from machine 1 — but with an 8-machine cluster joined to the
same CXL pool):

```bash
NUM_MACHINES=8 ./artifact-evaluation/ipc-cdf/run.sh
```

Outputs go to `out/<timestamp>-m<NUM_MACHINES>/` so the two variants can be
compared side by side.

`artifact-evaluation/prepare.sh` is global. Run it before the individual
artifact tests; repeated default runs reuse existing large backing files. It
prepares:

- CXL shared memory file
- 8 NUMA backing files
- hostfs shared memory file and metadata
- CXL/NUMA magic headers
- ivshmem doorbell server

The IPC test script:

1. Checks that the global AE environment has been prepared.
2. Resets DSM metadata for this run.
3. Temporarily enables client breakdown and server timing instrumentation.
4. Rebuilds ChCore.
5. Boots two QEMU machines through the prepared DSM/ivshmem setup.
6. Runs:
   - `direct_empty`
   - `direct`
   - `cross_empty`
   - `cross`
   - `cross_empty_4t`
   - `cross_4t`
7. Parses the QEMU logs.
8. Writes CSV files and figures under `artifact-evaluation/ipc-cdf/out/<timestamp>/`.

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
