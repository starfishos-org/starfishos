# 5 — Auto-scaling applications (paper Fig auto-scale)

Reproduces three figures, each throughput/runtime vs machine count (1–8):

- `auto-scale-matrix.eps` — Matrix-multiply MapReduce
- `db1000.eps` — DBx1000 TPC-C
- `gemini-chcore.eps` — GeminiGraph PageRank

Each compares StarfishOS **Mixed** (`MIXED_DEFAULT_CXL`) and **CXL** (all-CXL)
against external baselines.

## How it works

`run.sh` first sweeps both StarfishOS placements and then invokes
`run_baselines.py` to collect every non-Starfish curve:

| Baseline | Source (`test-on-linux/`) | Appears in |
|---|---|---|
| Ideal (Linux DRAM) — Matrix | `phoenix` | matrix (IDEAL) |
| Ideal (Linux DRAM) — DBx1000 | `dbx1000` | db1000 (linux) |
| Ideal (Linux DRAM) — Gemini | `GeminiGraph` | gemini (LINUX-DRAM) |
| Distributed (local TCP) — Matrix | AE `matrix_tcp_mpi.c` | matrix (TCP) |
| Distributed (Linux-MPI) — Gemini | `ggraph-distri` | gemini (DISTRIBUTED) |
| Tigon (external DB) — DBx1000 | `../deps/tigon` submodule | db1000 (tigon) |

The Matrix distributed runner forces MPI onto loopback `tcp,self`, so it measures the
paper's local multi-process TCP path rather than MPI shared-memory transport.
The Tigon runner uses the public
[`starfishos-org/tigon`](https://github.com/starfishos-org/tigon) submodule at
`artifact-evaluation/deps/tigon`, pinned to the portable SPR1 artifact commit.
The actual commit,
ancestry of the paper revision `16f8007fa15bc853397b04e0747efc4f8c21ef25`,
and dirty status are recorded in `logs/tigon-source.txt`. Override the source
with `TIGON_DIR=/path/to/tigon` when developing against another checkout.

## Data-file formats (identical to `p3os-paper/eval/`)

- matrix: lines `RESULT: N=<m> CONFIG=<MIXED|CXL|TCP|IDEAL> TIME=<us>`
- db1000: CSV `module,machines,performance_mops` (modules `P3OS-mixed`,
  `P3OS-all_cxl`, `tigon`, `linux`)
- gemini: CSV `machines,MIXED_DEFAULT_CXL,CXL,DRAM,LINUX-DRAM,DISTRIBUTED`
  (values `<seconds>s`)

## Run

```bash
./artifact-evaluation/prepare.sh                 # once
./artifact-evaluation/5-auto-scale/run.sh
```

Output: `out/<timestamp>/figures/{auto-scale-matrix,db1000,gemini-chcore}.{eps,pdf,png}`
plus the paper's shared `auto-scale-legend.{eps,pdf}`.

Linux Gemini logs retain every raw timing and checksum record. The collector
requires the complete warmup-plus-measurement set and appends an `AE_QUALITY`
record. Numerically divergent or high-latency measured samples are additionally
listed as `AE_WARNING` records but remain in the reported average. Detection is
relative to the measured-sample median (relative tolerances `5e-5` for PageRank
checksums and `5e-2` for high latency).

Linux baseline build state is isolated under `out/<timestamp>/linux-build`.
Phoenix and Gemini use out-of-source CMake trees. DBx1000 is built from a clean
materialization of its current tracked and nonignored-untracked files, so local
edits are honored while deleted files and ignored compiler caches stay deleted
or excluded; its source checkout and existing build cache are never modified.

Env overrides: `APPS` (`matrix db1000 gemini`), `MACHINES` (`1 2 4 6 8`),
`CONFIGS` (`Mixed CXL`), `TIMEOUT`, `OUT_DIR`, `RUN_BASELINES` (default `1`),
`BASELINE_STAGES` (`linux,matrix-tcp,tigon`), `TIGON_DIR`, `TIGON_SETUP`.
`TIGON_IMAGE_ATTEMPTS` controls transient image-build retries (default `3`).
Verbose image-builder output is connected directly to
`logs/tigon-image-build-attempt<N>.log`; the controller prints only periodic
status lines and never retries a build interrupted by `SIGINT`, `SIGTERM`, or
`SIGHUP`.

The runner holds a nonblocking per-user lock for its full lifetime. A second
auto-scale invocation, including one from another checkout, exits with the
recorded holder PID/repository instead of cleaning or restarting shared QEMU,
tmux, ivshmem, or host-tuning state underneath the active run.

The first Tigon run performs its upstream host/VM setup and therefore needs
passwordless/non-interactive `sudo`, 8 VM capacity, and the hardware layout
described by Tigon. Set `TIGON_SETUP=0` only when those VMs are already running
and its binary has already been synchronized. `prepare.sh` initializes the
submodule automatically for `auto-scale` and `paper` runs.

## Re-plot only / verify against paper data

The drawing logic is copied verbatim from `p3os-paper/eval/{auto_scale_matrix,
db1000,gemini_graph}.py` and is validated — it reproduces all three figures
from the paper's own data files:

```bash
python3 artifact-evaluation/5-auto-scale/plot.py --out-dir /tmp/as-check \
  --matrix-data /mnt/disk1/yjs/p3os-paper/eval/mapreduce/4000size.txt \
  --db1000-data /mnt/disk1/yjs/p3os-paper/eval/db1000/db1000-p3os-tigon.csv \
  --gemini-data /mnt/disk1/yjs/p3os-paper/eval/gemini_graph/data.log
```

## Completeness contract

The normal path requires all 5 machine counts for every plotted series: 20
Matrix points, 20 DBx1000 points, and 20 Gemini points. Missing logs, metrics,
or points terminate the run/plot with an error. `--allow-partial` exists only
for debugging an interrupted collection.
