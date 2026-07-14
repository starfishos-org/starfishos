# Artifact evaluation

## Hardware and software requirements

### Paper's Default Testbed

Hardware:
- CPU: 4 × Intel Xeon Gold 6418H, 24 physical cores each (96 CPUs total).
- DRAM: 4 × 64 GiB = 256 GiB.
- CXL: 128 GiB CXL 1.1 on the evaluation machine; 64 GiB is the minimum for the real-CXL setup.
- NUMA: 6 nodes; nodes 0–3 have CPUs, nodes 4–5 are memory-only.

Software:
- x86_64, Ubuntu 22.04/Linux 5.15
- QEMU 6.2 with KVM enabled
- Required tools: `numactl`, `tmux`, Python 3, GNU `make`, and Docker or the local `chbuild` toolchain.

Host dependency installation:

On Ubuntu/Debian, install qemu-6.2 and required host-side tools with:

```bash
bash artifact-evaluation/install-host-deps.sh
```

The script enables Docker and adds the invoking user to the `docker` group; log out and back in before using Docker without `sudo`. It downloads the official QEMU 6.2.0 source package, builds the x86_64/KVM target and ivshmem server, then installs `/usr/local/bin/qemu-6.2-system-x86_64` and `/usr/local/qemu-6.2/bin/ivshmem-server`.

### Hardware/Software Requirement

Hardware Requirement:
- Memory requirement: CXL >= 64GiB; DRAM >= 144GiB (8*16GiB DRAM + 16GiB hostfs)
- CPU requirement: >= 96 CPUs

*Note: you can change the CPU cores or memory size by changing the `CPU_NUM` and `DRAM_SIZE` variables in the `dsm-scripts/numa_sizes.conf` file, but some tests may not work properly.*

Software Requirement:
- Architecture: x86_64, other architectures are not supported yet.
- Operating System: Ubuntu 22.04/Linux 5.15, (*Note: Do not use Linux 6.1–6.4; our OS cannot boot on affected Linux versions because PCID is disabled due to the `INVLPG` issue described in [Phoronix's report](https://www.phoronix.com/news/Intel-Disable-PCID-ADL-RPL).*)
- QEMU: 6.2 with KVM enabled, other QEMU versions are unsupported yet as they do not have the feature of configuring ivshmem size that our artifact requires.
- Required tools: `numactl`, `tmux`, `Python 3` (`matplotlib` is required for plotting), `GNU make`, and `Docker`.


## Prepare the environment

Run the global environment preparation before running individual artifact tests:

```bash
./artifact-evaluation/prepare.sh
```

The global preparation creates and initializes:

- CXL shared-memory backing file
- 8 NUMA backing files, serving as local DRAM of 8 machines
- hostfs shared-memory backing file and metadata
- ivshmem doorbell server

Download the docker image from Docker Hub for building the artifact:

```bash
docker pull promisivia/treesls_chcore_builder:v2.3
```

To build the image from scratch instead, run this command from the repository root:

```bash
docker build -t promisivia/treesls_chcore_builder:v2.3 .
```

## Run the evaluations

After that, run any complete evaluation from the repository root:

```bash
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
evaluation directory for sched/notify, allocator, and filesystem recovery.
The filesystem-recovery evaluation keeps only its latest raw logs in its
fixed `logs/` directory.
