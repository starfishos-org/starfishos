# StarfishOS Artifact Evaluation

Quick jump: [Artifact Available](#artifact-available) |
[Artifact Functional](#artifact-functional) |
[Results Reproduced](#results-reproduced) |
[Known limitations](#known-limitations)

This artifact accompanies *StarfishOS: Revisiting Single System Image on CXL
with State-Partitioned Microkernel*. StarfishOS is an experimental OS-level
single-system image built on the ChCore microkernel. It uses shared CXL memory
for cross-machine coordination while keeping hot private state in local DRAM.

This guide is the reviewer entry point. The repository [README](README.md)
describes the system and normal development workflow;
[artifact-evaluation/README.md](artifact-evaluation/README.md) contains the
complete hardware, runtime, privilege, and runner reference; each numbered
experiment has its own README with inputs, outputs, and re-plot instructions.

The artifact supports the following paper claims:

1. A state-partitioned microkernel keeps only cross-machine coordination state
   in CXL while retaining local execution state in private DRAM.
2. Cross-machine IPC, scheduling, notification, allocation, and collaborative
   system services make a CXL pod usable as one OS-level system image.
3. Existing shared-memory applications can run across machines and use
   otherwise fragmented pod resources without application redesign.
4. Recoverable shared coordination state lets surviving machines and services
   continue after a machine failure; application recovery remains
   application-owned.

## Artifact Available

Reviewers should confirm:

1. **Repository:** the artifact is public at
   <https://github.com/starfishos-org/starfishos>. Record the evaluated revision
   with `git rev-parse HEAD`; use the revision supplied with the evaluation
   snapshot rather than a later development commit.
2. **License:** [LICENSE](LICENSE) contains the Mulan PSL v1 license.
3. **Paper snapshot:** [docs/starfish.pdf](docs/starfish.pdf) is the submitted
   manuscript. Figure numbers in that PDF are the submitted numbers used by
   this guide. The experiment-to-result table below also lists the two
   evaluation additions that are not present in that snapshot.
4. **Documentation:** the architecture guide starts at
   [docs/README.md](docs/README.md), and the evaluation guide starts at
   [artifact-evaluation/README.md](artifact-evaluation/README.md).

### Completeness and paper mapping

| Paper mechanism or result | Primary implementation | Evaluation |
| --- | --- | --- |
| Pod boot, CXL sharing, and doorbells | `build/simulate.sh`, `dsm-scripts/config_memdev.sh`, `kernel/drivers/pci/ivshmem.c`, `kernel/irq/ipi.c` | `0-basic` |
| Cross-machine IPC, scheduling, and notification | `kernel/ipc/`, `kernel/sched/`, `kernel/dsm/dsm_objects/` | `1-ipc-cdf`, `2-sched-notify-latency`, `9-queue-saturation` |
| DRAM/CXL allocation and state placement | `kernel/mm/`, `kernel/dsm_config.cmake` | `3-memory-allocator`, `4-state-partition` |
| Collaborative services and filesystem recovery | `user/system-servers/` | `7-recover-fs` |
| Application migration and auto-scaling | `kernel/dsm/dsm_migrate.c`, `user/demos/` | `5-auto-scale`, `6-resource-util` |
| Cross-warehouse TPC-C sensitivity | `user/demos/dbx1000/`, `kernel/dsm/` | `8-dbx1000-cross-warehouse` |

The more detailed mechanism-to-source map is in
[docs/05-implementation-map.md](docs/05-implementation-map.md).

## Artifact Functional

The reference setup is x86_64 Ubuntu 22.04/Linux 5.15 with QEMU 6.2/KVM,
96 physical CPUs, 256 GiB DRAM, and the six-node NUMA topology described in
[artifact-evaluation/README.md](artifact-evaluation/README.md). A full prepare
uses about 216 GiB of `/dev/shm` backing memory. Do not use Linux 6.1-6.4; see
the evaluation README for the PCID/`INVLPG` limitation and for reduced-size
configuration notes.

### From a fresh clone

Run these commands once on a clean host:

```bash
git clone https://github.com/starfishos-org/starfishos.git starfishos
cd starfishos
git submodule update --init --recursive

bash artifact-evaluation/install-host-deps.sh
# Log out and back in so docker and kvm group membership take effect.
docker pull promisivia/treesls_chcore_builder:v2.3

./artifact-evaluation/run-all.sh --list
```

`install-host-deps.sh` installs host packages, builds QEMU 6.2 and
`ivshmem-server`, and changes Docker/KVM group membership. Read the privilege
and restricted-Tigon sections of the evaluation README before running it on a
shared machine.

### End-to-end smoke test

The exp8 smoke profile exercises dependency preparation, build, a two-machine
QEMU/KVM boot, cross-machine DBx1000 execution, log validation, CSV parsing,
and plotting with one short point:

```bash
DBX_SMOKE=1 ./artifact-evaluation/run-all.sh \
  --run-subset-of-tests 8
```

The first run is dominated by host preparation and the initial build. A
successful run exits with status 0 and creates:

```text
artifact-evaluation/8-dbx1000-cross-warehouse/out/<timestamp>/
  logs/
  csv/cross_warehouse.csv
  csv/cross_warehouse_samples.csv
  figures/dbx1000-cross-warehouse.png
  config/run_config.json
```

The runner accepts a point only after the guest prints its benchmark-complete
markers, returns to a shell prompt, and all fatal-signature, repetition, scope,
and binding checks pass.

## Results Reproduced

Do not run the evaluation from inside an existing tmux session. Each experiment
starts its own tmux sessions for QEMU.

### Fast reviewer run

The default command runs all figure-producing experiments under the fast
profile:

```bash
./artifact-evaluation/run-all.sh
```

The fast profile reduces selected sweep axes and repetitions. It is useful for
an end-to-end reviewer run, but it is **not** the complete paper reproduction:
it omits Table 4, uses one DBx1000 repetition, and thins several axes. The exact
overrides are listed in the evaluation README.

### Complete paper sweeps

Use `--full` for the complete sweeps, repetitions, error bars, and Table 4:

```bash
./artifact-evaluation/run-all.sh --full
```

The default/full aggregate runner covers Figures 11-16 and the two camera-ready
experiments. Table 3 and the Section 8.2 text measurements are intentionally
separate; run them with:

```bash
./artifact-evaluation/run-all.sh --full --run-subset-of-tests 0,2
```

On the paper testbed, measured full scopes range from about 0.15 h for queue
saturation to 7.9 h for auto-scaling with Tigon and the footprint pass. The
budgets shown by `--list` and in the evaluation README are hang timeouts, not
runtime estimates. Build caches, selected axes, and host performance affect the
aggregate runtime; reviewers should run selected experiments first and use
their observed durations when planning the remaining sweep.

### Experiment-to-result map

| # | Result | Main output |
| ---: | --- | --- |
| 0 | Paper Table 3 setup measurements | MLC logs and `msi_summary.csv` |
| 1 | Figure 11 IPC latency and breakdown | `ipc_cdf.png`, `ipc_read_breakdown.png` |
| 2 | Section 8.2 scheduling/notification latency | `sched_notify_latency.png` |
| 3 | Figure 12 allocator throughput | `allocator-all.png` |
| 4 | Figure 13 state-partition ablation | `state_partition.png` |
| 5 | Figure 14 application auto-scaling and Table 4 | `auto-scale-matrix.png`, `db1000.png`, `gemini-chcore.png` |
| 6 | Figure 15 resource utilization | `real.png` |
| 7 | Figure 16 LevelDB recovery | `recovery-performance-single.png` |
| 8 | Camera-ready cross-warehouse sensitivity | `dbx1000-cross-warehouse.png` |
| 9 | Camera-ready service-queue saturation | `queue_saturation.png` |

Run one experiment with:

```bash
./artifact-evaluation/run-all.sh --full --run-subset-of-tests <N>
```

Each run creates a new timestamped directory under
`artifact-evaluation/<experiment>/out/` containing raw logs, parsed CSVs,
figures, the git revision, the effective placement, experiment knobs, and
verbatim build-configuration snapshots. Output directories are gitignored and
are not reused across runs.

Re-plot the latest completed output without restarting QEMU:

```bash
./artifact-evaluation/run-all.sh --plot-only \
  --run-subset-of-tests <N>
```

The experiment-to-result table above is the public figure mapping for this
artifact. Per-experiment README files document the expected CSV schema, figure
names, scope controls, and experiment-specific validation.

## Known limitations

- The artifact targets the paper's specific x86_64, QEMU 6.2/KVM, large-memory,
  multi-socket NUMA environment. Reduced configurations are useful for
  debugging but may not reproduce paper-scale results.
- The full auto-scale experiment can build and start an eight-VM Tigon
  environment. Use the root-owned restricted helper described in the evaluation
  README on shared hosts; do not grant passwordless sudo to scripts in a
  reviewer-writable checkout.
- The `dbx1000/All_DRAM/8` Private baseline temporarily expands one local-DRAM
  backing file from 16 GiB to 32 GiB and restores it afterward. Allow about
  16 GiB of additional temporary host memory for that point.
- Exp8 enables host NUMA binding by default and interleaves the CXL backing
  region across the configured memory-only nodes. Results collected with
  different placement policies are not directly comparable.
- Stop an interrupted run or clear stale QEMU/tmux state with
  `./artifact-evaluation/stop.sh` before retrying.
