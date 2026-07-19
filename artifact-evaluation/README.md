# Artifact evaluation
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

If a working `docker` is already present (e.g. an existing `docker-ce`
install), the script skips installing the `docker.io` package instead of
letting apt fail on the `docker.io`/`docker-ce` package conflict.

After installing packages the script verifies that `numpy`, `pandas`, and
`matplotlib` actually import. If a user-site NumPy 2.x (`~/.local`) is mixed
with the distro's NumPy 1.x C extensions — imports then fail with
`_ARRAY_API not found` — it repairs the stack by installing consistent
versions (plus `numexpr`/`bottleneck`) into the user site via `pip --user`.

The installer waits up to 60 seconds for apt/dpkg locks. Use
`APT_LOCK_TIMEOUT=<seconds>` to wait longer. On a pre-provisioned host,
`SKIP_APT=1` skips package installation but still verifies/installs QEMU and
`ivshmem-server`; it must only be used when all listed packages already exist.

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

## Getting Start

Do these steps once on a clean host before the one-click runner. Skipping
submodules or host deps is the most common reason a first run fails.

```bash
git clone https://github.com/starfishos-org/starfishos.git starfishos
cd starfishos

# Initialize the ready-AE dependencies independently. 
git submodule update --init --recursive

# Host tools: QEMU 6.2, ivshmem-server, Docker, numactl, tmux, matplotlib/numpy/pandas.
bash artifact-evaluation/install-host-deps.sh
# Log out and back in so docker + kvm group membership apply.

# One-click entry point
./artifact-evaluation/run-all.sh --clean # clean old output directories
./artifact-evaluation/run-all.sh
```

**Note:** Do not run `./artifact-evaluation/run-all.sh` inside tmux; each experiment launches its own tmux session for QEMU, and nesting leads to session conflicts.

### Restricted Tigon administration without general sudo

On a shared host, do not grant the evaluation account passwordless access to
the Tigon scripts in this checkout. The checkout is writable by that account,
so such a sudoers rule is equivalent to unrestricted root access. Instead, an
administrator can install the root-owned `starfishos-tigon` helper and its
root-owned runtime snapshot. The helper accepts only four exact operations:

```bash
sudo -n /usr/local/libexec/starfishos-tigon start
sudo -n /usr/local/libexec/starfishos-tigon reset
sudo -n /usr/local/libexec/starfishos-tigon stop
sudo -n /usr/local/libexec/starfishos-tigon status
```

The helper must be owned by `root:root`, must not be group/other writable, and
must never execute code from this writable checkout. Grant only the exact
helper commands in sudoers and validate the rule with `visudo -cf`. The AE
account does not need to belong to the `sudo` group.

`run-all.sh` detects this restricted configuration by checking that the fixed
helper exists and that all four exact commands are authorized by sudoers. The
check is policy-only and does not start a VM. The wrapper then exports the
restricted-mode marker and `TIGON_SETUP=0`; the caller does not need to set the
variable or invoke `start`/`stop` manually:

```bash
./artifact-evaluation/run-all.sh --clean
./artifact-evaluation/run-all.sh
```

Arguments and experiment selectors remain owned by `run_all.py`. Invalid
arguments, `--clean`, `--list`, `--plot-only`, `--dry-run`, and subsets without
the auto-scale experiment never start Tigon. During auto-scale, the Linux and
TCP baselines run first; only then does its script start the prepared eight-VM
environment, run the Tigon baseline, and immediately stop it. Failure and
Ctrl-C use the auto-scale exit cleanup path. If the environment was already
running, the script reuses it and leaves it running; it only stops an
environment that it started itself.

In restricted mode, `TIGON_SETUP=0` is required internally: setup mode builds
an image and changes host VM, network, mount, and CPU state. The root-owned
helper owns those privileged operations, while the benchmark runner can only
request an exact CXL-backing reset through the helper. Outside restricted
mode, the original setup and direct `fallocate` reset paths remain available.
`--clean` removes experiment outputs only and does not start or stop VMs.

Use the helper directly to inspect the environment or to recover a pre-existing
or degraded instance before retrying:

```bash
sudo -n /usr/local/libexec/starfishos-tigon status
sudo -n /usr/local/libexec/starfishos-tigon stop
```

After finishing the one-click runner, each experiment creates a timestamped
output directory:

```
artifact-evaluation/<experiment>/out/<timestamp>/
  logs/      runtime QEMU and benchmark logs
  csv/       parsed tables and intermediate data
  figures/   plots (png only)
```

These directories are gitignored. Re-running creates a new `out/<timestamp>/`
directory; prior runs are preserved until removed with `./artifact-evaluation/run-all.sh --clean`.

### CLI options

| Option | Effect |
| --- | --- |
| *(default)* | Run the ready set: ipc-cdf, queue-saturation, sched-notify, memory-allocator, state-partition, auto-scale, resource-util, recover-fs, dbx1000-cross-warehouse |
| `--run-subset-of-tests N[,N...]` | Run only numbered experiments (comma-separated; spaces trimmed). See table below. |
| `--clean` | Remove `artifact-evaluation/*/out/` and legacy flat `logs/`, `csv/`, `figures/` under each experiment. Alone: clean and exit. With a run or `--plot-only`: clean first, then continue. |
| `--plot-only` | Re-plot from the latest `out/<timestamp>/` without re-running QEMU |
| `--no-prepare` | Skip `prepare.sh` |
| `--no-build` | Skip first-time OS build check |
| `--budget SECS` | Override timeout for all selected experiments |
| `--dry-run` | Print actions without running prepare, build, experiments, or clean |
| `--list` | List experiments with paper numbers and exit |

Examples:

```bash
python3 artifact-evaluation/run_all.py --list
python3 artifact-evaluation/run_all.py --clean
python3 artifact-evaluation/run_all.py --run-subset-of-tests 1,4,7
python3 artifact-evaluation/run_all.py --no-prepare --no-build --run-subset-of-tests 1
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 3
python3 artifact-evaluation/run_all.py --dry-run --clean --run-subset-of-tests 1,4
```

### Stopping runs and troubleshooting

Each experiment launches QEMU (and sometimes `chbuild` via Docker) in its own
tmux session. If you want to **stop an in-progress `run_all.py`**, or you hit a
**Docker container name conflict** (for example
`The container name "/wfn-chbuild" is already in use`), stop all tmux sessions:

```bash
./artifact-evaluation/stop.sh
```

Then re-run `./artifact-evaluation/run-all.sh` to continue.

### Experiments

Each numbered experiment writes paper figures as `.png` files under
`out/<timestamp>/figures/`.

| # | Directory | Output Figure(s) | Paper | Description |
| --- | --- | --- | --- | --- |
| 0 | 0-basic | — | Table 3 (setup) | basic (CXL latency/bandwidth/MSI) |
| 1 | 1-ipc-cdf | `ipc_cdf`, `ipc_read_breakdown` | Figure 11 | ipc-cdf |
| 2 | 2-sched-notify-latency | `sched_notify_latency` | Section 8.2 (text) | sched-notify |
| 3 | 3-memory-allocator | `allocator-all` | Figure 12 | memory-allocator |
| 4 | 4-state-partition | `state_partition` | Figure 13 (camera-ready: 4- and 8-machine panels) | state-partition |
| 5 | 5-auto-scale | `auto-scale-matrix`, `db1000`, `gemini-chcore`, `auto-scale-legend` | Figure 14 | auto-scale |
| 6 | 6-resource-util | `real` | Figure 15 | resource-util |
| 7 | 7-recover-fs | `recovery-performance-single` | Figure 16 | recover-fs |
| 8 | 8-dbx1000-cross-warehouse | `dbx1000-cross-warehouse` | Camera-ready revision (Reviewer B Q3) | TPC-C cross-warehouse ratio sweep |
| 9 | 9-queue-saturation | `queue_saturation` | Camera-ready revision (Reviewer B, Fig. 11b) | per-service-queue tail latency + saturation throughput |

The persistent CXLFS backing file is tied to the checkout's built ramdisk.
Before every AE boot it is recreated when the repository changes or
`user/build/ramdisk.cpio` is rebuilt. This prevents files inherited from
another clone/build (especially `/libc.so`) from failing CXLFS verification,
while retaining the filesystem across boots within one recovery experiment.

Application-level Linux Ideal / Distributed ports for paper auto-scale curves
live under `test-on-linux/`.
