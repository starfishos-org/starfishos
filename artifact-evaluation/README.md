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

Download the docker image from Docker Hub for building the artifact:

```bash
docker pull promisivia/treesls_chcore_builder:v2.3
```

(Optional) If you want to build the image from scratch instead, run this command from the repository root:

```bash
docker build -t promisivia/treesls_chcore_builder:v2.3 .
```

---

## One-click (sole entry point)

```bash
./artifact-evaluation/run-all.sh          # calls python3 artifact-evaluation/run_all.py is equivalent to the above
```

After cloning, this is usually **the only command you need**. On the first run it automatically:

| Step | Behavior | When it runs |
| --- | --- | --- |
| `prepare.sh` | CXL / 8×NUMA / hostfs / ivshmem doorbell | Every time (skips rebuild if already present) |
| first-time OS build | `scripts/quick-build.sh` (= compile step of `make prepare`) | **Only when** `.config` or `build/kernel.img` is missing |
| Paper experiments | Each directory's `run.sh` runs tests; `run_all.py` plots after each run | Every time |
| TODO stubs | Skipped when `run.sh` is missing (`process-migration`) | Every time |

```bash
python3 artifact-evaluation/run_all.py --dry-run          # print plan only
python3 artifact-evaluation/run_all.py --list             # list experiments
python3 artifact-evaluation/run_all.py --prepare-only     # environment prepare only
python3 artifact-evaluation/run_all.py --prepare-only --prepare-mode recreate
python3 artifact-evaluation/run_all.py --build-only       # first-time / forced build only
python3 artifact-evaluation/run_all.py --experiments-only ready
python3 artifact-evaluation/run_all.py --plot-only ipc-cdf   # re-plot without QEMU
python3 artifact-evaluation/run_all.py --no-plot ready       # run only, skip figures
python3 artifact-evaluation/run_all.py --force-base-build # force rebuild then run all
```

### Stage options

| Option | Effect |
| --- | --- |
| `--prepare-only` | Run `prepare.sh` only, then exit |
| `--build-only` | Do first-time / forced OS build only, then exit |
| `--gather-only` | List experiment output directories only (no QEMU) |
| `--experiments-only` | Skip prepare + build; run experiments only |
| `--prepare` / `--no-prepare` | Toggle prepare (on by default) |
| `--prepare-mode ensure\|recreate` | `ensure` is idempotent (default); `recreate` rebuilds backing files |
| `--build` / `--no-build` | Toggle first-time OS build check (on by default) |
| `--force-base-build` | Force `quick-build.sh` |
| `--run` / `--no-run` | Toggle experiment `run.sh` invocations |
| `--budget SECS` | Override timeout for all ready experiments |

### Paper figure status

| Experiment | Paper figure | Status |
| --- | --- | --- |
| `1-ipc-cdf` | IPC CDF + breakdown | ready |
| `3-memory-allocator` | `fig00-allocator-all` | ready |
| `4-state-partition` | `state_partition` | ready |
| `6-auto-scale` | `auto-scale-matrix` / `db1000` / `gemini-chcore` | ready |
| `8-process-migration` | `process-migration-data-*` | ready |
| `6-resource-util` | `real.eps` | ready¹ |
| `7-recover-fs` | `recovery-performance-single` | ready |
