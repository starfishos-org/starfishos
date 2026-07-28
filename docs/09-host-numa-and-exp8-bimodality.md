# Host NUMA placement and the exp8 bimodal worker split

Status: **confirmed by measurement** (2026-07-27). This supersedes the earlier
"host NUMA is completely unmanaged" hypothesis, which was half right, and the
DRAM->CXL page-ratchet hypothesis, which was measured and refuted.

## The signature

`artifact-evaluation/8-dbx1000-cross-warehouse` at 8 machines, ratio 15%, splits
its 64 workers into two clean modes with nothing in between: 13-21 workers
commit ~1.1-1.3x10^5 transactions in the 5 s measurement window while the rest
commit ~2.5-2.8x10^4. Every worker measures the same window (`run_time` is
4.99 s for all of them), so this is a genuine 3.3-4.8x throughput difference,
not a measurement artifact.

*Which* workers are fast is redrawn on every boot; *how many* is stable within a
run but drifts across runs. Because aggregate throughput tracks the fast-worker
count, that drift is what gave the headline number its run-to-run uncertainty.

## Root cause

Two facts combine.

**1. Guest memory placement is deliberate, and asymmetric.**
`dsm-scripts/config_memdev.sh` binds the backing files explicitly at creation
time (`dd` pre-populates them, so the pages are already placed before QEMU
starts):

- each 16 G per-machine DRAM file `numaX.Y-$USER`: `numactl --membind=$((i/2))`,
  i.e. onto CPU-bearing nodes 0-3;
- the 64 G shared CXL region `ivshmem-$USER`: `numactl --membind=4` — a
  **CPU-less** node, which is the point: it emulates CXL with genuinely far
  memory. `artifact-evaluation/prepare.sh` warns when that node is absent.

  (This was the default at the time of the investigation; it is now
  `--interleave=4,5`, for the reasons below.)

Only the 16 G hostfs image is left to first touch.

On this host (`spr4numa`, 4x Xeon Gold 6418H, 6 NUMA nodes) node 4 is attached
to socket 0, so it is *near node 0 and far from everything else*. Measured with
a pointer-chase probe, 512 MB, random permutation:

| CPU node | -> node 4 (CXL region) | -> node 0 (DRAM) |
|---------:|-----------------------:|-----------------:|
| 0        | **278 ns**             | 130 ns           |
| 1        | 598 ns                 | 307 ns           |
| 2        | 464 ns                 | 239 ns           |
| 3        | 463 ns                 | 230 ns           |

**2. vCPU threads are not bound.** `scripts/qemu/qemu_wrapper.sh` had its
`numactl` line commented out, so all 96 vCPU threads float over all 96 host
cores under CFS. Sampling `/proc/<pid>/task/<tid>/stat` field 39 confirms
threads from one machine scattered across several nodes.

Because `DSM_PAGE_MODE`, `DSM_STACK_MODE` and `DSM_THREADCTX_MODE` are all `CXL`
(and the one-way DRAM->CXL ratchet migrates the rest), ~98.7% of DBx1000's hot
data lives in that node-4 region. So **a worker is fast exactly when CFS happens
to park its vCPU thread on host node 0**, and slow otherwise. 64 workers over 4
CPU-bearing nodes gives ~16 on node 0 — the observed 13-21 band.

This is consistent with the refuted residency result: both modes really are
~99% CXL-backed. The old probe answered *which tier*, not *which host node*.

## Evidence

Per-worker correlation of vCPU placement against the fast/slow label
(`RATIOS=15`, `DBX_REPETITIONS=2`, unbound baseline):

| modal host node | rep1 fast/slow | rep1 mean txn | rep2 fast/slow | rep2 mean txn |
|----------------:|---------------:|--------------:|---------------:|--------------:|
| **0**           | **14 / 1**     | **106829**    | **19 / 1**     | **104990**    |
| 1               | 0 / 12         | 27338         | 0 / 13         | 27383         |
| 2               | 0 / 13         | 30094         | 0 / 15         | 28329         |
| 3               | 2 / 14         | 40990         | 0 / 16         | 28431         |

All 19 fast workers in rep2 are on node 0. The correlation is sharpest over the
narrowest sampling window (93-95% at 3-15 s, decaying to 79% at 120 s), which is
what an instantaneous-placement effect should look like. The per-node means also
follow the latency table ordinally: node 1 is the slowest path to node 4 and has
the lowest throughput, nodes 2 and 3 are equidistant and nearly identical.

The 1-machine control shows no bimodality at all (8 workers, 177k-314k,
max/min 1.77) — consistent with it reading 0.00% CXL.

## Intervention

The fix has two independent halves, and both are needed:

- **CPU side** (`CHCORE_QEMU_NUMA_BIND`) buys *determinism* — it stops CFS from
  re-drawing which worker gets the good node on every boot.
- **Memory side** (`CXL_MEM_POLICY=interleave`) buys *flatness* — it removes the
  privilege that makes one node good in the first place.

Binding alone leaves a 4.8x split that is merely predictable. Interleaving alone
would flatten the tiers but leave which machine lands in which tier up to CFS.

### CPU side: `scripts/qemu/qemu_wrapper.sh`

```bash
CHCORE_QEMU_NUMA_BIND=1 ./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
```

The wrapper already receives `vm_id` as `$1`, so no extra plumbing is needed to
decide the node. Points that matter:

- **CPU-less nodes must be filtered out.** The candidate list is built with
  `awk '/^node [0-9]+ cpus:/ && NF > 3'` — in `numactl --hardware` output a
  CPU-less node prints `node 4 cpus:` with only three fields. This is also why
  the old commented-out line could not simply be un-commented: it read
  `numactl -N $vm_id --preferred=$vm_id`, and with `vm_id` running 0-7 the
  values 4-7 name nodes that have no CPUs at all. That is most likely why it was
  disabled in the first place.
- **`--cpunodebind`, not `-N`/`--preferred`.** `--preferred` alone does not
  restrict CPUs, which is the whole point here.
- **`--preferred` for memory, not `--membind`**, keeping the intent of the
  original comment: stay local while the node has room, fall back rather than
  invite the OOM killer. The 64 G that actually matters is already placed by
  `dd` (below), so this only affects QEMU's own allocations.
- **`ceil(machines / nodes)`**, not a hard-coded `/2`, so 2/4/6-machine runs also
  work. For 8 machines on 4 nodes it yields machine i -> node i/2, reproducing
  the 2-machines-per-node layout the `numaX.Y` file naming already implied.

`CHCORE_QEMU_NUMA_NODES="1,2,3"` restricts the candidate list (see the dead end
below before using it).

**Environment forwarding.** `dsm-scripts/simulate_ncluster.sh` passes both
variables as an explicit prefix on `RUN_CMD`, because tmux windows do not
reliably inherit the launching shell's environment — the tmux server may be a
pre-existing process. The existing code already prefixed `MACHINE_NUM` and
`CPU_NUM` for the same reason. The chain is: your shell -> `run.sh` ->
`simulate_ncluster.sh` -> (explicit prefix) -> `simulate.sh` -> (ordinary exec
inheritance) -> `qemu_wrapper.sh`.

`scripts/qemu/emulate.tpl.sh` also gained `debug-threads=on` so QEMU names its
vCPU threads `CPU <n>/KVM`; without it every thread inherits the process name and
the guest-CPU mapping has to be guessed from thread creation order. Note this is
the *template* — `build/simulate.sh` is generated from it by
`scripts/build/cmake/Modules/KernelTools.cmake`, so editing the build output
directly is lost on the next build.

### Memory side: `dsm-scripts/config_memdev.sh`

**Why changing the `dd` line is sufficient — and why the file must be
recreated.** tmpfs pages are allocated on *write*, under whatever NUMA policy is
in force at that moment, and they stay there. The 64 G region is filled once by
`dd if=/dev/zero`, so its placement is decided in that single pass. QEMU later
only `mmap`s the already-populated file, and **mmap does not migrate pages** — no
`host-nodes=`/`policy=` on the `memory-backend-file` object could move them.
That is why `cxl-new` does `rm` before `dd`: changing the policy has no effect on
pages that already exist.

So the change is on that one line. **The default is now `--interleave=4,5`**
(it was `--membind=4`):

```bash
cxlMemPolicy="${CXL_MEM_POLICY:-interleave}"
cxlMemNodes="${CXL_MEM_NODES:-4,5}"
...
numactl --$cxlMemPolicy=$nodes dd if=/dev/zero of=$devName bs=1G count=$size
```

`CXL_MEM_POLICY=membind CXL_MEM_NODES=4` restores the old behaviour.

**Topology fallback.** Hard-coding node numbers in a default would break any host
without them, so `resolve_cxl_mem_nodes()` drops nodes with no
`/sys/devices/system/node/node$n`, and `new_cxl` degrades in three steps:

| requested nodes present | behaviour |
|---|---|
| all | `numactl --interleave=4,5` |
| some | warn, `--interleave=` over the survivors |
| none, but node 4 exists | warn, `--membind=4` (legacy behaviour) |
| none at all | warn loudly, plain `dd` with the default policy |

The last case is the one worth reading the warning for: the shared region lands
on ordinary DRAM and the guests stop seeing CXL-like latency at all, so the
experiment silently stops measuring what it claims to. It still runs.

### Verifying both halves actually took effect

Do not trust the scripts — check the running system. For the CPU side, read the
real affinity masks:

```bash
for p in $(pgrep -x qemu-6.2-system); do
    echo "$(tr '\0' ' ' < /proc/$p/cmdline | grep -oE 'chcore-[0-9]+') $(taskset -cp $p)"
done
```

which should show 2 machines per node (`chcore-0/1` on 0-23, `chcore-2/3` on
24-47, and so on). For the memory side, `numastat -m` and read the `Shmem` row:

```
              node0   node1   node2   node3   node4   node5
membind=4     33410   32777   32773   32769   65536   16376
interleave    32774   34685   34681   34041   32768   49144
```

Node 4 halving from 65536 to 32768 with the other 32768 appearing on node 5
(49144 = 32768 + the 16376 hostfs image) is the interleave landing exactly
50/50.

### Result: binding machine i -> node i/2

The split does **not** disappear, and that is the expected outcome — this layout
preserves the asymmetry, since node 0 is still the only node near the CXL region.
What changes is that it stops being random:

| | fast workers | fast mean | slow mean | ratio |
|---|---|---|---|---|
| rep1 | `tid 0-15` (machines 0,1) | 127.5k | 26.8k | 4.76 |
| rep2 | `tid 0-15` (machines 0,1) | 127.3k | 26.5k | 4.80 |

Exactly the 16 workers of the two machines pinned to node 0, in both reps, with
fast means 0.2% apart. Placement correlation for rep1 is 16/16/16/16 per node,
100% of node-0 workers fast and 0% elsewhere. Aggregate cluster throughput
spread fell from 1.35% to 0.50% over two reps, and the mechanism that drove the
larger run-to-run drift — a fast-worker count wandering between 13 and 21 — is
pinned at exactly 16.

This is a confirmation of the mechanism, not a refutation: the intervention
predicted *which specific workers* would be fast, and they were.

### Dead end: excluding node 0 (`CHCORE_QEMU_NUMA_NODES="1,2,3"`)

The obvious way to equalise the machines is to keep them all off node 0, so that
every worker is 463-598 ns from the CXL region. It does remove the bimodality —
and is useless, because it removes everything else too. Throughput collapsed
from ~27k-127k to **804-1418 transactions per worker**, uniformly across all
eight machines, `run_time` still a correct 5.03 s:

```
machine 0 (node 1): 1172   machine 4 (node 2): 1211
machine 1 (node 1):  962   machine 5 (node 2):  804
machine 2 (node 1):  951   machine 6 (node 3):  871
machine 3 (node 2): 1418   machine 7 (node 3): 1114
```

8 machines x 12 vCPUs needs all 96 cores. Dropping to three nodes puts 36 vCPU
threads on node 1's 24 cores, and in a spin/poll-heavy microkernel that
lock-holder preemption stalls the whole distributed workload — machines 6 and 7
are not even oversubscribed and are just as slow, because cross-warehouse
transactions run at the speed of the slowest participant. The second repetition
never finished its warmup within 50 minutes. **Do not use this configuration.**

### The actual fix: make the CXL region symmetric

The asymmetry lives in the *memory* placement, not the CPU placement, so remove
it there. There are two CXL nodes and they are mirror images: node 4 hangs off
socket 0, node 5 off socket 1. Each CPU node is near one of them, but not the
same one — measured:

| CPU node | -> node 4 | -> node 5 |
|---------:|----------:|----------:|
| 0        | **278 ns**| 638 ns    |
| 1        | 598 ns    | **396 ns**|
| 2        | 464 ns    | 651 ns    |
| 3        | 463 ns    | 661 ns    |

Interleaving 50/50 across both therefore gives every CPU node half near pages and
half far pages, which evens the distances out:

| CPU node | node 4 only | interleaved 4,5 |
|---------:|------------:|----------------:|
| 0        | 278 ns      | ~458 ns         |
| 1        | 598 ns      | ~497 ns         |
| 2        | 464 ns      | ~558 ns         |
| 3        | 463 ns      | ~562 ns         |

A 1.23x spread instead of 2.15x, keeping all 96 cores in play. **The interleaved
column is an arithmetic mean of the two measured columns, not a direct
measurement** — the probe run for it hit the command timeout and was not
repeated. The throughput results below are measured, and their ordering matches
this estimate (nodes 0 and 1 in one tier, nodes 2 and 3 in a lower one).

```bash
CXL_MEM_POLICY=interleave CXL_MEM_NODES=4,5 ./dsm-scripts/config_memdev.sh cxl-new
```

Note this changes what the experiment emulates: the guests no longer see one
uniform CXL device but a mean over a near and a far one. It buys reproducibility
at the cost of a slower best case.

### Result: interleaved CXL region + `CHCORE_QEMU_NUMA_BIND=1`

**The bimodal split is gone.** With the region interleaved over nodes 4 and 5 and
machines bound i -> node i/2 (so all 96 cores stay in play), rep1 gives:

```
rep1: n=64 min=41291 p25=42541 median=59082 p75=61997 max=65826  max/min=1.59  above 2x median: 0
rep2: n=64 min=41515            median=58591            max=65895  max/min=1.59  above 2x median: 0
```

Not one worker exceeds twice the median — the two clean modes with nothing in
between have become a 1.59x spread, identically in both reps. What is left is a
deterministic per-machine tier that follows the interleaved latency table:

| machines | host node | rep1 mean | rep2 mean |
|---|---|---|---|
| 0, 1 | 0 | 60621, 59874 | 60135, 59818 |
| 2, 3 | 1 | 63202, 63546 | 63518, 64167 |
| 4, 5 | 2 | 42489, 42532 | 42429, 42162 |
| 6, 7 | 3 | 42401, 42542 | 42081, 42130 |

Nodes 0 and 1 each sit next to one of the two CXL devices and land at ~60-64k;
nodes 2 and 3 are far from both and land at ~42k. Machines on the same node
agree to within 1%.

Crucially **aggregate throughput is preserved** while reproducibility improves:

| configuration | rep1 | rep2 | spread | bimodal? |
|---|---|---|---|---|
| unbound (baseline) | 3380615 | 3335598 | 1.35% | yes, 3.7-4.8x, 13-21 fast |
| `NUMA_BIND=1` | 3326154 | 3309457 | 0.50% | yes, 4.8x, exactly 16 fast |
| interleave + `NUMA_BIND=1` | 3337660 | 3331528 | **0.18%** | **no** |

The split is removed without paying for it.

## Recommendation

For reproducible exp8 numbers on this host, use both:

```bash
CXL_MEM_POLICY=interleave CXL_MEM_NODES=4,5 ./dsm-scripts/config_memdev.sh cxl-new
CHCORE_QEMU_NUMA_BIND=1 ./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
```

Binding alone pins the fast-worker count at exactly 16 instead of a drifting
13-21, which already removes the run-to-run uncertainty in the headline number.
Interleaving additionally removes the 4.8x intra-cluster split itself.

Interleaving is now the **default** for `config_memdev.sh`, so the first command
is only needed to rebuild an existing region (or to override the nodes). CPU
binding is still **opt-in** — `CHCORE_QEMU_NUMA_BIND` defaults to 0 — because it
constrains the scheduler on hosts whose core counts may not match this one, so
it has to be requested explicitly.

Any published number should state which placement was used, since the two
configurations are not comparable at the per-worker level.

Caveats worth carrying forward:

- Each configuration was measured with **2 repetitions only**. The two reps agree
  closely (identical fast-worker sets under binding; `max/min` 1.59 in both
  interleaved reps), but the aggregate-throughput spreads in the table above rest
  on two points each and should not be quoted as variance estimates.
- Everything here is specific to this host's topology (`spr4numa`: 4 CPU nodes
  plus 2 CPU-less CXL nodes, one per socket pair). On a host with a single CXL
  node, or with the CXL node equidistant from all sockets, the interleave fix has
  nothing to interleave across and the analysis has to be redone.
- **Interleaving is now the repository default** (2026-07-28), and the host has
  been switched over: `/dev/shm/ivshmem-$USER` is `--interleave=4,5`, verified as
  32768 MB on node 4 and 32768 MB on node 5. This affects *every* experiment that
  touches the CXL region, not just exp8, so results taken before and after this
  date are not directly comparable. Revert a run with
  `CXL_MEM_POLICY=membind CXL_MEM_NODES=4 ./dsm-scripts/config_memdev.sh cxl-new`.

## Tooling

- `dsm-scripts/analysis/sample_qemu_numa.py` — samples each QEMU vCPU thread's
  last-run host CPU and derives its NUMA node.
- `dsm-scripts/analysis/correlate_numa_placement.py` — correlates that against
  the `[tid=N] txn_cnt=` lines in a DBx1000 log.

Worker *i* on machine *m* runs on guest CPU `m * GUEST_CPUS + i`, which is what
DBx1000's `bind 64 cpu:` banner lists.
