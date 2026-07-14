# Artifact evaluation

## From a fresh clone (required order)

Do these steps once on a clean host before the one-click runner. Skipping
submodules or host deps is the most common reason a first run fails.

```bash
git clone <repo-url> starfishos
cd starfishos

# Required: demo/library gitlinks (rpmalloc, phoenix, dbx1000, GeminiGraph, …)
# from https://github.com/starfishos-org (public HTTPS).
git submodule update --init --recursive

# Host tools: QEMU 6.2, ivshmem-server, Docker, numactl, tmux, matplotlib/numpy/pandas.
bash artifact-evaluation/install-host-deps.sh
# Log out and back in so docker + kvm group membership apply.

docker pull promisivia/treesls_chcore_builder:v2.3

# One-click: prepare (datasets + ivshmem) → first OS build if needed → ready experiments.
python3 artifact-evaluation/run_all.py
# Equivalent:
./artifact-evaluation/run-all.sh
```

Defaults:
- `run_all.py` with no args runs the **ready** set (`ipc-cdf`, `memory-allocator`,
  `state-partition`, `recover-fs`), not the full paper list.
- `prepare.sh` downloads Phoenix datasets under `datasets/` (gitignored). The
  large Gemini graph (`twitter-2010.bin`, ~11 GiB) is **skipped by default**;
  set `SKIP_GRAPH_DATASET=0` when you need Gemini / auto-scale graph runs.

Optional full paper / extras:

```bash
python3 artifact-evaluation/run_all.py paper    # paper order (includes stubs / unvalidated)
python3 artifact-evaluation/run_all.py all      # paper + extras
```

---

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

```bash
bash artifact-evaluation/install-host-deps.sh
```

The script enables Docker, adds the invoking user to the `docker` and `kvm`
groups, installs plotting deps (`matplotlib`, `numpy`, `pandas`), builds QEMU
6.2.0 with KVM, and installs `/usr/local/bin/qemu-6.2-system-x86_64` plus
`/usr/local/qemu-6.2/bin/ivshmem-server`. Log out and back in before using
Docker or `/dev/kvm` without `sudo`.

### Hardware/Software Requirement

Hardware Requirement:
- Backing files under `/dev/shm` (default layout): about **216 GiB** total —
  8×16 GiB NUMA DRAM files + 64 GiB CXL + 16 GiB hostfs + 8 GiB CXLFS.
  Plan for **≥ 216 GiB** free RAM/tmpfs for a full prepare; smaller hosts must
  shrink `dsm-scripts/numa_sizes.conf` / `chcore.ini` (some tests may then fail).
- CXL ivshmem allocation uses `numactl --membind=4` (memory-only node on the
  paper testbed). Hosts without NUMA node 4 must edit `memNumaNode` in
  `dsm-scripts/config_memdev.sh`.
- CPU requirement: ≥ 96 CPUs for paper-scale runs (many AE scripts override
  `CPU_NUM`; smaller machines can still run microbenchmarks with reduced CPUs).

*Note: you can change per-NUMA sizes in `dsm-scripts/numa_sizes.conf` and guest
sizes in `chcore.ini`, but some tests may not work properly at reduced scale.*

Software Requirement:
- Architecture: x86_64, other architectures are not supported yet.
- Operating System: Ubuntu 22.04/Linux 5.15, (*Note: Do not use Linux 6.1–6.4; our OS cannot boot on affected Linux versions because PCID is disabled due to the `INVLPG` issue described in [Phoronix's report](https://www.phoronix.com/news/Intel-Disable-PCID-ADL-RPL).*)
- QEMU: 6.2 with KVM enabled, other QEMU versions are unsupported yet as they do not have the feature of configuring ivshmem size that our artifact requires.
- Required tools: `numactl`, `tmux`, `Python 3` (`matplotlib`, `numpy`, `pandas`), `GNU make`, `curl`, and `Docker`.

Download the docker image from Docker Hub for building the artifact:

```bash
docker pull promisivia/treesls_chcore_builder:v2.3
```

(Optional) If you want to build the image from scratch instead, run this command from the repository root:

```bash
docker build -t promisivia/treesls_chcore_builder:v2.3 .
```

---

## One-click entry point

```bash
python3 artifact-evaluation/run_all.py
# Equivalent:
./artifact-evaluation/run_all.py
./artifact-evaluation/run-all.sh
```

After the fresh-clone steps above, this is the main command. On the first run it:

| Step | Behavior | When it runs |
| --- | --- | --- |
| `prepare.sh` | Submodule check, dataset download, CXL / 8×NUMA / hostfs / CXLFS / ivshmem doorbell | Every time (skips recreating existing backing files) |
| first-time OS build | `scripts/quick-build.sh` | **Only when** `.config` or `build/kernel.img` is missing |
| Ready experiments | Each directory's `run.sh` (build/QEMU/plot); `run_all.py` can plot after each run | Every time (default mode: `ready`) |
| Stubs | Skipped when status is `stub` / `run.sh` missing | When selected |

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
python3 artifact-evaluation/run_all.py paper              # full paper list
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
| `1-ipc-cdf` | IPC CDF + breakdown | ready (default one-click) |
| `3-memory-allocator` | `fig00-allocator-all` | ready (default one-click) |
| `4-state-partition` | `state_partition` | ready (default one-click) |
| `7-recover-fs` | `recovery-performance-single` | ready (default one-click) |
| `5-auto-scale` | `auto-scale-matrix` / `db1000` / `gemini-chcore` | ready¹ (`paper` / named) |
| `6-resource-util` | `real.eps` | ready¹ (`paper` / named) |
| `process-migration` | `process-migration-data-*` | stub (skipped) |

¹ Plotting is validated against paper data; the live QEMU collection path may
need extra demos, `test-on-linux/` baselines, and `SKIP_GRAPH_DATASET=0`.
See each directory's README.

Application-level Linux Ideal / Distributed ports for paper auto-scale curves
live under `test-on-linux/` (git submodules; `git submodule update --init
test-on-linux`). See `test-on-linux/README.md`.

---

## Low-level prepare scripts (usually not run separately)

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/prepare.sh recreate
SKIP_GRAPH_DATASET=0 ./artifact-evaluation/prepare.sh   # also fetch twitter-2010.bin
./scripts/download_datasets.sh                           # datasets only
```

## Per-experiment entry points

```bash
./artifact-evaluation/1-ipc-cdf/run.sh
./artifact-evaluation/2-sched-notify-latency/run.sh
./artifact-evaluation/3-memory-allocator/run.sh
./artifact-evaluation/4-state-partition/run.sh
./artifact-evaluation/5-auto-scale/run.sh
./artifact-evaluation/6-resource-util/run.sh
./artifact-evaluation/7-recover-fs/run.sh
./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
./artifact-evaluation/0-basic/run_msi.sh
./artifact-evaluation/0-basic/run_mlc.sh
```

Each subdirectory has a README describing its workload, configuration matrix,
environment overrides, raw logs, and figure-regeneration command.
