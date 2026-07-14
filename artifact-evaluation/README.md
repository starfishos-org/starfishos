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
files, refreshes the lightweight CXL metadata, and only rewrites hostfs when
its hostfs metadata is missing or no longer matches the configured dataset.
Thus it does not repeatedly copy the existing large Twitter dataset. To force
a full rebuild:

```bash
./artifact-evaluation/prepare.sh recreate
```

After that, run any complete evaluation from the repository root:

```bash
./artifact-evaluation/0-basic/run_msi.sh
./artifact-evaluation/0-basic/run_mlc.sh
./artifact-evaluation/1-ipc-cdf/run.sh
./artifact-evaluation/2-sched-notify-latency/run.sh
./artifact-evaluation/3-memory-allocator/run.sh
./artifact-evaluation/7-recover-fs/run.sh
```

Each test script checks that the global environment is present. It does not
recreate the large backing files. `run.sh` is the normal entry point: it runs
the benchmark, processes its results, and invokes that evaluation's `plot.py`.

To regenerate figures from existing results without booting QEMU, run:

```bash
python3 artifact-evaluation/1-ipc-cdf/plot.py
python3 artifact-evaluation/2-sched-notify-latency/plot.py
python3 artifact-evaluation/3-memory-allocator/plot.py
```

All three figure entry points are named `plot.py`. IPC and sched/notify parse
their raw QEMU logs inside `plot.py`. The allocator `run.sh` first converts its
raw logs to `allocator_results.csv`, so its `plot.py` reads that CSV. Explicit
input and output options are documented in each evaluation's README.

Complete evaluations build by default. This is intentional: IPC temporarily
enables instrumentation, sched/notify must include its microbenchmark, and the
allocator test switches among several compile-time configurations. Do not set
`SKIP_BUILD=1` unless an existing image is known to match the required source
and configuration; the allocator evaluation always performs its required
configuration-specific builds.

## Available evaluations

| Directory | Measurement | Main outputs |
| --- | --- | --- |
| `0-basic` | ivshmem MSI delivery latency and host Intel MLC bandwidth | MSI CSV summaries and MLC logs |
| `1-ipc-cdf` | Direct and cross-machine IPC latency distributions | CSV summaries and CDF figures |
| `2-sched-notify-latency` | Local versus cross-machine scheduling and notification wakeup latency | Four-metric sample CSV, summary CSV, latency figure |
| `3-memory-allocator` | Buddy, LLFree, and LLFree+CR allocator throughput | Throughput CSV and allocator scaling figures |
| `7-recover-fs` | Kill machine 0, recover its tmpfs and reopen LevelDB on machine 1 | Recovery-stage CSVs and LevelDB recovery timeline |

Each subdirectory has a README describing its workload, configuration matrix,
environment overrides, raw logs, and figure-regeneration command. Generated
files are written under `out/<timestamp>/` for IPC and directly in the
evaluation directory for sched/notify and allocator.
