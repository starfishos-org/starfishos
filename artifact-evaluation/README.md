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
- CXL ivshmem allocation uses `numactl --interleave=4,5` (the two memory-only
  nodes on the paper testbed). Interleaving rather than pinning to one node is
  deliberate: a single CXL node sits much closer to one socket than to the
  others, which splits guest workers into a fast and a slow mode — see
  `docs/09-host-numa-and-exp8-bimodality.md`. Hosts with a different topology
  should set `CXL_MEM_NODES`/`CXL_MEM_POLICY`; nodes that do not exist are
  dropped automatically, so the default degrades rather than fails.
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
  config/    what this run was actually configured with
```

These directories are gitignored. Re-running creates a new `out/<timestamp>/`
directory; prior runs are preserved until removed with `./artifact-evaluation/run-all.sh --clean`.

#### `config/`: what a run was configured with

Experiments rewrite `kernel/dsm_config.cmake`, `.config` and `chcore.ini`
before building and restore the originals when they finish, so the effective
memory placement is otherwise visible neither in the checkout nor in the guest
serial logs. Each run therefore records it:

```
config/placement.txt    effective placement (DSM_MALLOC_MODE,
                        DSM_USER_MALLOC_MODE, DSM_{THREADCTX,PGTABLE,STACK,
                        OBJECT,PAGE}_MODE, ...) plus the guest CPU/DRAM
                        profile; one block per distinct placement, so a sweep
                        that varies placement between points records each
                        variant
config/run_config.json  the same placement in machine-readable form, plus the
                        git revision, host, and the experiment's own knobs
                        (machine/warehouse/thread counts, warmup, ratios,
                        repetitions, ...)
config/build/snapshotN  verbatim copies of dsm_config.cmake, .config,
                        chcore.ini and user/demos/config.cmake, one directory
                        per placement block above (same numbering)
```

Placement blocks are appended automatically whenever a cluster boots, so this
applies to every experiment. Runs produced before this mechanism existed have
no `config/` directory and cannot be attributed to a placement after the fact.

### Runtime and the default fast profile

**The per-experiment budgets are timeouts, not estimates.** They are deliberately
generous so a slow-but-healthy run is never killed; a run that actually hits one
has hung. Do not plan a session around the budget column — plan around the
measured column, which comes from the timestamps of past runs in `out/`:

| Experiment | Paper figure | measured (`--full` scope) | budget (timeout) |
| --- | --- | ---: | ---: |
| 1-ipc-cdf | Figure 11 | *no data* | 3 h |
| 3-memory-allocator | Figure 12 | *no data* | 8 h |
| 4-state-partition | Figure 13 | **0.85 h** (8-machine panel, 24 points) | 4 h |
| 5-auto-scale | Figure 14 | **7.9 h** (incl. Tigon + footprint) | 18 h |
| 6-resource-util | Figure 15 | *never run end to end* | 12 h |
| 7-recover-fs | Figure 16 | *no data* | 3 h |
| 8-dbx1000-cross-warehouse | camera-ready | **1.3 h** (3 ratios × 3 reps) | 6 h |
| 9-queue-saturation | camera-ready | **0.15 h** (6 thread points × 2 queues) | 3 h |

Where measurements exist the budget is 2–15× the real time, and 5-auto-scale is
the only experiment that is genuinely long — most of its 7.9 h is the Tigon
baseline's mkosi image build and eight-VM environment. **A complete `--full` run
of all eight figures is expected to take well under a day**, not the ~77 h the
budgets sum to.

Four experiments have no timing data because they have never been run through
this harness; 6-resource-util additionally carries a `NOT YET VALIDATED AGAINST
A LIVE RUN` caveat in its own `run.sh`. Their budgets are guesses. Treat a first
run of those as unmeasured, and expect to debug rather than to reproduce.

Per-point costs behind the measured numbers, useful for sizing a subset:

- 4-state-partition: **~2.5 min per (benchmark, placement, panel) point**, very
  stable across six historical runs (0.034–0.043 h/point).
- 8-dbx1000-cross-warehouse: **~0.44 h per ratio** at 3 repetitions, linear in
  the ratio count (3 ratios → 1.33 h, 5 ratios → 2.22 h).

Most of the time is QEMU boot and benchmark execution, not compilation: the
experiments already batch builds behind their sweeps. 6-resource-util builds
**once** for all 36 points; 4-state-partition builds once per (placement, panel)
and then runs all six benchmarks on that image; 5-auto-scale hoists the build
out of its machine-count loop for matrix and gemini. The exception is DBx1000,
whose `NUM_MACHINES`/`NUM_WH`/`WARMUP` are compile-time constants that must match
the cluster being booted, so it rebuilds per machine count by necessity.

Under the fast profile the same measured scopes drop to roughly: 8-dbx1000
**~0.7 h** (extrapolated to 1 repetition), 5-auto-scale several hours less once
Tigon is skipped. 4-state-partition is unaffected — it already measures only
the 8-machine panel (0.85 h, measured); the two-panel sweep it used to run took
1.6 h.

The fast profile is on by default and applies two levers: one measurement per
point instead of three where a repeat knob exists, and a thinned sweep axis
that drops points adjacent to ones it keeps. Every value it sets is a
documented scope control of the experiment's own `run.sh`, which validates it
and switches its plotter to `--allow-partial` by itself:

| Experiment | Fast-profile overrides |
| --- | --- |
| 3-memory-allocator | `USER_BENCH_THREADS="1 4 16 64 96"` (from 8 points) |
| 5-auto-scale | `MACHINES="1 2 4 8"` (drops 6), `RUN_FOOTPRINT=0` |
| 8-dbx1000-cross-warehouse | `DBX_REPETITIONS=1` (from 3) |
| 9-queue-saturation | `THREADS="1 2 4 8 10"` (drops 6) |

4-state-partition has no fast-profile override: Figure 13 is the 8-machine
panel only, so its `run.sh` defaults to `MACHINE_COUNTS="8"` under `--full`
too. Pass `MACHINE_COUNTS="4 8"` to also measure the 4-machine panel.

**Known failure — `dbx1000/All_DRAM/8`.** A Private (All_DRAM) guest gets one
16 GiB DRAM device regardless of `DBX_DRAM_SIZE`: kernel-local DRAM comes from
`dsm-scripts/numa_sizes.conf`, not from QEMU's `-m`. The 8-machine panel's 64
warehouses therefore OOM during the TPC-C load and fail the plot step through
`--require-point`. The point aborts in about 208 s rather than burning
`DBX_TIMEOUT`: its `BUG: handle_trans_fault` matches `AE_ERROR_PATTERN`, so
`ae_wait_in_log` stops waiting as soon as it appears. This point is left
failing on purpose rather than hidden with `SKIP_POINTS`, because skipping it
also removes the Private baseline that every other placement in that panel is
normalized against — which would quietly drop DBx1000 from Figure 13 instead of
reporting a problem. The OOM is being fixed. To suppress it in the meantime:

```bash
SKIP_POINTS=dbx1000/All_DRAM/8 ./artifact-evaluation/run-all.sh
```
- **Table 4 (memory footprint) is not produced** — `RUN_FOOTPRINT=0`. This
  costs no figure.
- **9-queue-saturation keeps `REPEATS=3`.** Its `plot.py` drops the lowest and
  highest trial per point and fails below three, so only its thread axis is
  thinned.

The Tigon baseline is **kept** in fast mode — the measured runtimes above show
there is room for it — but it is scheduled as late as possible, twice over:

- `run_baselines.py` always executes `linux` → `matrix-tcp` → `tigon`, in that
  fixed order, regardless of how `BASELINE_STAGES` is spelled. Within
  5-auto-scale the Tigon stage also comes after the StarfishOS sweep and the
  footprint pass.
- 5-auto-scale is the **last** entry in the default run set, so every other
  figure is measured and plotted before any Tigon VM starts.

This matters because the Tigon stage builds an mkosi image and brings up an
eight-VM CXL-pod environment, changing host VM, network, mount and CPU state.
If it fails or wedges the host, the other seven figures are already on disk.
To skip it anyway — for a quick end-to-end check, or on a host where the
eight-VM environment is unavailable — drop the stage explicitly:

```bash
BASELINE_STAGES=linux,matrix-tcp ./artifact-evaluation/run-all.sh
```

Figure 14 then has no Tigon curve, but keeps the Linux Ideal and Distributed
baselines, and no restricted-sudo Tigon helper is needed.

Any override already set in the environment wins over the fast profile, so a
single axis can be widened without leaving it:

```bash
MACHINES="1 2 4 6 8" ./artifact-evaluation/run-all.sh
```

To reproduce the paper's numbers as published — full sweeps, error bars, and
Table 4 — use `--full`.

### CLI options

| Option | Effect |
| --- | --- |
| *(default)* | Run the ready set, in order: ipc-cdf, queue-saturation, memory-allocator, state-partition, resource-util, recover-fs, dbx1000-cross-warehouse, auto-scale (only experiments that produce a paper figure; excludes 0-basic and 2-sched-notify, which are setup/text-only), under the **fast profile** below. auto-scale is last because it is the only one that starts Tigon. |
| `--full` | Disable the fast profile and run the paper's complete sweeps. Equivalent to `AE_FULL=1`. See the measured-runtime table above — the difference is hours, not the days the timeout budgets suggest. |
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
python3 artifact-evaluation/run_all.py --full
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

The `Paper` column uses the **SOSP'26 submission's** figure numbers, i.e. the
ones the reviews refer to. The paper working tree has since drifted by three
(`fig:eval-ipc` builds as Figure 14 locally). See
[docs/06-paper-figure-map.md](../docs/06-paper-figure-map.md) for the full
label ↔ figure ↔ experiment mapping and for which reviewer ask targets which
figure.

| # | Directory | Output Figure(s) | Paper | Description |
| --- | --- | --- | --- | --- |
| 0 | 0-basic | — | Table 3 (setup) | basic (CXL latency/bandwidth/MSI) |
| 1 | 1-ipc-cdf | `ipc_cdf`, `ipc_read_breakdown` | Figure 11 | ipc-cdf |
| 2 | 2-sched-notify-latency | `sched_notify_latency` | Section 8.2 (text) | sched-notify |
| 3 | 3-memory-allocator | `allocator-all` | Figure 12 | memory-allocator |
| 4 | 4-state-partition | `state_partition` | Figure 13 (camera-ready: 8-machine panel) | state-partition |
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
