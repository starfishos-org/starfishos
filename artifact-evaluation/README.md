# Artifact evaluation

Run the global environment preparation before running individual artifact
tests:

```bash
./artifact-evaluation/prepare.sh
```

The global preparation creates and initializes:

- CXL shared-memory backing file
- 8 NUMA backing files, serving as local DRAM of 8 machines
- hostfs shared-memory backing file and metadata
- ivshmem doorbell server

The preparation script is idempotent by default: it creates missing backing
files and refreshes metadata, but it does not rewrite existing large files. To
force a full rebuild:

```bash
./artifact-evaluation/prepare.sh recreate
```

## One-click: run everything

```bash
./artifact-evaluation/run-all.sh                 # all experiments + all figures
./artifact-evaluation/run-all.sh ipc-cdf ipc-cdf-8m   # or a subset
```

`run-all.sh` runs global preparation, executes every experiment sequentially
(a failure doesn't stop the rest), prints a per-experiment status summary,
and gathers every generated figure under `artifact-evaluation/out/<ts>/figures/`.

## Experiments

Each experiment is a one-click script: it edits build configs as needed,
rebuilds ChCore, boots QEMU cluster(s), runs the workloads, parses the logs,
and emits CSVs + figures under `<experiment>/out/<timestamp>/`. All modified
build/source files are restored on exit. See each subdirectory's README for
details.

| Directory | Paper figure | What it measures |
|---|---|---|
| [`ipc-cdf/`](ipc-cdf/) | **Fig 11** | Local vs cross-machine IPC latency CDF + breakdown. `NUM_MACHINES=8 ./ipc-cdf/run.sh` runs the reviewer-requested 8-machine variant. |
| [`allocator-throughput/`](allocator-throughput/) | **Fig 12** | Kernel slab / buddy (Buddy vs LLFree vs LLFree+CR) and userspace rpmalloc throughput, DRAM vs CXL. |
| [`state-partition/`](state-partition/) | **Fig 13** | 6 applications on a 2-machine cluster under 4 state-partition configs, normalized to Private. |
| [`dbx1000-cross-warehouse/`](dbx1000-cross-warehouse/) | reviewer request | DBx1000 TPC-C throughput + CXL/DRAM footprint as the cross-warehouse transaction ratio sweeps 15/50/80% on 8 machines. |

Each test script checks that the global environment is present; it does not
recreate the large backing files. Experiments rebuild the OS with different
configs, so run them **one at a time**:

```bash
./artifact-evaluation/ipc-cdf/run.sh
NUM_MACHINES=8 ./artifact-evaluation/ipc-cdf/run.sh
./artifact-evaluation/allocator-throughput/run.sh
./artifact-evaluation/state-partition/run.sh
./artifact-evaluation/dbx1000-cross-warehouse/run.sh
```

Shared helpers (environment checks, tmux/QEMU cluster management, config
editing) live in [`common.sh`](common.sh).
