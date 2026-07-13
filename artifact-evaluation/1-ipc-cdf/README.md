# IPC CDF artifact script

This directory contains the end-to-end artifact script for the first IPC test
used by the paper evaluation: local direct IPC vs cross-machine polling IPC.

## Run the complete evaluation

Run from the repository root:

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/1-ipc-cdf/run.sh
```

`artifact-evaluation/prepare.sh` is global. Run it before the individual
artifact tests; repeated default runs reuse existing large backing files. It
prepares:

- CXL shared memory file
- 8 NUMA backing files
- hostfs shared memory file and metadata
- CXL/NUMA magic headers
- ivshmem doorbell server

`run.sh` performs the complete workflow:

1. Checks that the global AE environment has been prepared.
2. Resets DSM metadata for this run.
3. Temporarily enables client breakdown and server timing instrumentation.
4. Rebuilds ChCore so the temporary IPC instrumentation is present in the
   image.
5. Boots two QEMU machines through the prepared DSM/ivshmem setup.
6. Runs:
   - `direct_empty`
   - `direct`
   - `cross_empty`
   - `cross`
   - `cross_empty_4t`
   - `cross_4t`
7. Invokes `plot.py`, which parses the QEMU logs and generates CSV files and
   figures.
8. Replaces the previous artifact under `artifact-evaluation/1-ipc-cdf/`.

Useful outputs:

- `logs/machine0.log`, `logs/machine1.log`: the only retained raw logs (the
  next run replaces them).
- `summary.csv`: p50/p75/p90/p99/max latency summary in us.
- `cdf.csv`: per-sample CDF points in us.
- `breakdown.csv`: median client-side breakdown in us.
- `server_timing.csv`: median server dequeue/handle timing in us.
- `ipc_cdf.pdf`: CDF figure.
- `ipc_read_breakdown.pdf`: Read 4KiB median breakdown figure.

## Regenerate results and figures only

To parse existing logs and redraw the figures without rebuilding or booting
QEMU:

```bash
python3 artifact-evaluation/1-ipc-cdf/plot.py
```

With no arguments, the script reads `logs/` and writes CSVs and PDFs directly
under `artifact-evaluation/1-ipc-cdf/`. To select another log location
explicitly:

```bash
python3 artifact-evaluation/1-ipc-cdf/plot.py \
  --log-dir artifact-evaluation/1-ipc-cdf/logs \
  --out-dir artifact-evaluation/1-ipc-cdf
```

In this evaluation, `plot.py` does include parsing: it reads both raw machine
logs, regenerates the CSV files, and then redraws the PDFs.

The runner builds by default. `SKIP_BUILD=1` should only be used when the
existing image is known to contain the required IPC instrumentation and match
the current source.
