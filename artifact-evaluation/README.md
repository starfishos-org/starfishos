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
python3 artifact-evaluation/run_all.py
# Equivalent:
./artifact-evaluation/run_all.py
./artifact-evaluation/run-all.sh          # thin wrapper; calls run_all.py
```

After cloning, this is usually **the only command you need**. On the first run it automatically:

| Step | Behavior | When it runs |
| --- | --- | --- |
| `prepare.sh` | CXL / 8×NUMA / hostfs / ivshmem doorbell | Every time (skips rebuild if already present) |
| first-time OS build | `scripts/quick-build.sh` (= compile step of `make prepare`) | **Only when** `.config` or `build/kernel.img` is missing |
| Paper experiments | Each directory's `run.sh` runs tests and plots; figures gathered under `out/<ts>/figures/` | Every time |
| TODO placeholders | Unimplemented paper figures written to `TODO.md` / `FIGURES.md` | Every time |

```bash
python3 artifact-evaluation/run_all.py --dry-run          # print plan only
python3 artifact-evaluation/run_all.py --list             # list experiments
python3 artifact-evaluation/run_all.py --prepare-only     # environment prepare only
python3 artifact-evaluation/run_all.py --prepare-only --prepare-mode recreate
python3 artifact-evaluation/run_all.py --build-only       # first-time / forced build only
python3 artifact-evaluation/run_all.py --experiments-only ready
python3 artifact-evaluation/run_all.py --gather-only      # gather existing figures only
python3 artifact-evaluation/run_all.py --force-base-build # force rebuild then run all
```

### Stage options

| Option | Effect |
| --- | --- |
| `--prepare-only` | Run `prepare.sh` only, then exit |
| `--build-only` | Do first-time / forced OS build only, then exit |
| `--gather-only` | Collect figures from experiment dirs only; do not start QEMU |
| `--experiments-only` | Skip prepare + build; run experiments only |
| `--prepare` / `--no-prepare` | Toggle prepare (on by default) |
| `--prepare-mode ensure\|recreate` | `ensure` is idempotent (default); `recreate` rebuilds backing files |
| `--build` / `--no-build` | Toggle first-time OS build check (on by default) |
| `--force-base-build` | Force `quick-build.sh` |
| `--run` / `--no-run` | Toggle experiment QEMU runs |
| `--gather` / `--no-gather` | Toggle figure gathering |
| `--budget SECS` | Override timeout for all ready experiments |
| `--out DIR` | Set output directory |

### Paper figure status

| Experiment | Paper figure | Status |
| --- | --- | --- |
| `1-ipc-cdf` | IPC CDF + breakdown | ready |
| `3-memory-allocator` | `fig00-allocator-all` | ready |
| `4-state-partition` | `state_partition` | ready |
| `6-auto-scale` | `auto-scale-matrix` / `db1000` / `gemini-chcore` | ready¹ |
| `8-process-migration` | `process-migration-data-*` | ready |
| `9-resource-util` | `real.eps` | ready¹ |
| `7-recover-fs` | `recovery-performance-single` | ready |

¹ Plotting is validated (reproduces the paper figure from the paper's own data);
the QEMU data-collection path is scaffolded but not yet validated against a live
run — see each directory's README "Status / caveats". `6-auto-scale` and
`9-resource-util` additionally need external Linux/Tigon baselines (`test-on-linux/`)
and extra demos enabled.

### Optional modes

```bash
python3 artifact-evaluation/run_all.py ready     # run the implemented paper experiments only
python3 artifact-evaluation/run_all.py all       # paper + extras
python3 artifact-evaluation/run_all.py ipc-cdf   # named subset
```

| Option / Env | Meaning |
| --- | --- |
| `--dry-run` / `DRY_RUN=1` | Print plan only |
| `--prepare-only` | Prepare only |
| `--prepare-mode` / `PREPARE_MODE` | `ensure` (default) or `recreate` |
| `--no-prepare` / `SKIP_PREPARE=1` | Skip prepare |
| `--build-only` | OS build only |
| `--no-build` / `SKIP_BASE_BUILD=1` | Skip OS build check |
| `--force-base-build` / `FORCE_BASE_BUILD=1` | Force `quick-build.sh` |
| `--experiments-only` | Skip prepare+build; run experiments only |
| `--gather-only` | Gather figures only |
| `--budget SECS` | Global experiment timeout |
| `--out DIR` | Output directory |
| `BUDGET_<NAME>=secs` | Per-experiment timeout override |

Implementation: orchestration lives in [`run_all.py`](run_all.py); each experiment's QEMU flow remains in that directory's `run.sh` / `plot.py`.

**Plots must match the paper**: paper figures are drawn by [`paper-plots/`](paper-plots/) (vendored from `p3os-paper/eval/`); each experiment's `plot.py` / `parse_and_plot.py` only parses logs, exports paper-format CSV, then calls `paper-plots`. Do not invent a separate AE plotting style.

---

## Low-level prepare scripts (usually not run separately)

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/prepare.sh recreate
```

## Per-experiment entry points

```bash
./artifact-evaluation/1-ipc-cdf/run.sh
./artifact-evaluation/2-sched-notify-latency/run.sh
./artifact-evaluation/2-sched-notify-latency/run_linux.sh   # host Linux baseline (also run by one-click)
./artifact-evaluation/0-basic/run_msi.sh
./artifact-evaluation/0-basic/run_mlc.sh                    # host MLC / Table 1 (also run by one-click)
./artifact-evaluation/3-memory-allocator/run.sh
./artifact-evaluation/4-state-partition/run.sh
./artifact-evaluation/5-dbx1000-cross-warehouse/run.sh
./artifact-evaluation/7-recover-fs/run.sh
./artifact-evaluation/6-auto-scale/run.sh
./artifact-evaluation/8-process-migration/run.sh
./artifact-evaluation/9-resource-util/run.sh
```

Host Linux baselines included in one-click extras (`run_all.py all` or named
experiments): `basic` runs MSI then MLC (`ALLOW_MLC_SKIP=1` by default if
`mlc` is absent); `sched-notify` runs ChCore then `run_linux.sh`.

Application-level Linux Ideal / Distributed ports for paper auto-scale curves
live under `test-on-linux/` (git submodules; `git submodule update --init
test-on-linux`). See `test-on-linux/README.md`.

Each subdirectory has a README describing its workload, configuration matrix,
environment overrides, raw logs, and figure-regeneration command. Generated
files are written under `out/<timestamp>/` for the sweep experiments and
directly in the evaluation directory for the self-contained ones. The
filesystem-recovery evaluation keeps only its latest raw logs in its fixed
`logs/` directory.
