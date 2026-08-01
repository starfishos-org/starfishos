# How the cross-warehouse ratio actually affects DBx1000 throughput

Companion to `artifact-evaluation/8-dbx1000-cross-warehouse/`. The runner's
README records *what* each run measured and the configuration changes that got
it there; this note records *why* the curve has the shape it has, which
mechanisms were tested and eliminated, and what a reader can and cannot conclude
from the figure.

Read [09 — host NUMA and exp8 bimodality](09-host-numa-and-exp8-bimodality.md)
first if the run-to-run spread is what you are chasing. That problem is closed;
this one is about the shape of the mean.

## 1. What the knob does

`PERC_CROSS_WAREHOUSE_TXN` is sampled once per transaction
(`user/demos/dbx1000/config.h`, enabled by `USE_TRANSACTION_CROSS_WAREHOUSE_RATIO`):

- a selected Payment picks a customer warehouse other than its home;
- a selected NewOrder gets exactly one remote order line;
- everything else stays inside the home warehouse.

Because the cluster maps warehouse *w* to machine *w mod 8*, "crosses a
warehouse" is nearly the same event as "crosses a machine". So the knob is a
re-addressing knob and nothing else. Two properties matter and are easy to get
wrong:

**Rows touched per transaction is invariant in the ratio** (~13.0 across the
whole range). Raising the ratio does not make transactions bigger, it only
changes where their rows live.

**TPC-C semantics cap remoteness far below the ratio.** A NewOrder touches ~23
rows and at most one of them is remote; Payment touches a handful. Sweeping the
ratio from 0 to 100 moves *cross-machine row accesses* only from 0.00% to 6.84%.
The knob's name overstates its reach by more than an order of magnitude, and any
figure that plots throughput against the ratio is really plotting it against
that 0–6.8% band with a very stretched x-axis.

An earlier hypothesis — that transaction-level selection damps access-level
remoteness as the ratio rises, which would explain the flat region — is
**refuted**: cross-machine row accesses rise 5.37x against a transaction-level
5.33x, i.e. in lockstep. What the semantics give is a hard *cap*, not a damping.

## 2. The measured shape: a cliff, then a plateau

All five ratios measured in one run under one configuration
(`out/20260730_151635`, `MIXED_DEFAULT_DRAM` cluster against a `MIXED_DEFAULT_CXL`
baseline, read-ahead 32, three boots each, 5 s window):

| ratio | cluster (Mops/s) | baseline (Mops/s) | scaleup | CXL residency | step |
|---|---|---|---|---|---|
| 0% | 1.8268 ± 0.0078 | 0.1789 ± 0.0016 | 10.21 | 3.8% | — |
| 15% | 0.6671 ± 0.0028 | 0.1771 ± 0.0017 | 3.77 | 78.0% | **−63.5%** |
| 50% | 0.6401 ± 0.0087 | 0.1777 ± 0.0005 | 3.60 | 81.0% | −4.05% |
| 80% | 0.6282 ± 0.0019 | 0.1768 ± 0.0018 | 3.55 | 81.5% | −1.86% |
| 100% | 0.6260 ± 0.0012 | 0.1770 ± 0.0005 | 3.54 | 81.8% | −0.35% |

The curve is not a slope. Essentially the entire decline is spent between ratio
0 and ratio 15, and **the per-step cost then decays monotonically toward zero**:
−4.05%, −1.86%, −0.35%. The whole 15→100 range costs 6.2%, less than a tenth of
the first step.

Cluster repeatability is 0.19–1.35% CV, with no bimodality anywhere (worker
max/min 1.50–1.59, zero workers above 2x the median). The r15/r50/r80 cluster
points reproduce two earlier independent sweeps to 0.25%.

**The one-machine baseline is flat** — 0.1789 / 0.1771 / 0.1777 / 0.1768 /
0.1770, a 1.2% range with no monotone trend and per-point CV up to 1.0%. It is
pinned to CXL and measured at 100% CXL residency at every ratio, so it isolates
the workload from any tier effect. See §5 for why this matters.

Two cautions before quoting any of this:

- **Ratio 0 is not a TPC-C configuration.** Standard TPC-C already contains
  roughly 15% remote Payment traffic. Ratio 0 is a hypothetical lower bound
  useful for attribution, not a performance data point.
- **Ratio 100 is outside the TPC-C spec.** It is measured and well-behaved, but
  it describes a workload the benchmark does not define.

## 3. The cliff is a memory tier, not coherence

The 0→15 drop is the database moving from DRAM to CXL, not the cost of talking
to another machine.

Three independent measurements agree:

| measurement | factor |
|---|---|
| one machine, `DEFAULT_DRAM` vs `DEFAULT_CXL`, identical in all else | **2.94x** |
| eight machines, ratio 0 → 15 | **2.67x** |
| latency factor *k* back-solved from residency + throughput (below) | **3.05** |

Model the run time as a two-tier average, with *f* the fraction of accesses
served from CXL and *k* the CXL/DRAM access-cost ratio:

```
t  ∝  (1 - f) + f·k
```

Feeding in the measured access-weighted residency (~3.8% at ratio 0, ~99% at
ratio 15) and the single-configuration throughput ratio 2.738 yields
*k* = **2.96**, against an independently measured **2.94** — agreement to 0.8%
across two entirely different experiments. That is strong evidence that the
cliff is a tier flip and nothing else.

### Residency has three different denominators; they differ by 16 pp

This trips people up, so state the convention whenever quoting a residency. At
ratio 15%, the same three boots give:

| convention | value | what it answers |
|---|---|---|
| CXL / pages present at load (3.558M) | **94.0%** | *older README convention* |
| CXL / current footprint (4.285M) | **78.0%** | how much of the process is in CXL |
| pages that left DRAM / pages that started there | **72.7%** | how much actually migrated |

The process allocates about 0.73M pages (2.8 GiB) after the table load, so the
loaded-database denominator is stale by the time the measurement window opens
and inflates the figure. The 94% numbers in the runner's changelog use it.

Note also that **page-weighted residency under-predicts the slowdown** — plugging
78.0% into the model gives *k* = 3.57, well off the measured 2.94. Accesses are
not spread evenly over pages: the pages that migrate are exactly the hot,
cross-machine-shared ones, so the access-weighted fraction (~99%) runs far ahead
of the page-weighted one (78%). Use access weighting in the model; use page
weighting only to describe footprint.

### Why the flip is so complete, so early

The mechanism is the **one-way DRAM→CXL ratchet** in Case 2.3 of
`kernel/mm/pgfault_handler.c`: a page touched from a remote machine is migrated
into the shared CXL region and **nothing ever migrates it back**. Combined with
warmup's ~934k cross-machine accesses against a 13.6 GiB database, any nonzero
ratio drives the database to near-total CXL residency before the measurement
window opens.

This is the structurally important point for anyone designing a follow-up
experiment:

> Without a demotion path, **every nonzero ratio converges to the same fixed
> point** (fully converted). The ratio controls only the *rate* of conversion,
> not the equilibrium. That is why the curve is flat: past the cliff, every
> configuration is measuring the same memory system.

With a demotion path the equilibrium would become ratio-dependent
(`f = ratio / (ratio + a)` for some demotion rate *a*), which is the only known
way to turn this into a genuine steady-state ratio effect.

## 4. Read-ahead fills CXL, but it is not what flattens the curve

Case 2.3 reads ahead up to 32 virtually contiguous pages owned by the same
remote machine. The natural suspicion is that read-ahead over-converts the
database and thereby erases the ratio's effect. `DBX_READAHEAD` was added
(defining `PGFAULT_READAHEAD_MAX`, deliberately **separate** from the ABI
constant `POLLING_TLB_BATCH_MAX`, which must stay 32 on both the kernel and
polling-server sides) to test exactly this.

Depth sweep at ratio 15% (residency in loaded-database units — the older
convention of the previous section, so these are ~16 pp higher than the
footprint-based figures in §2):

| read-ahead | DB in CXL | Mops/s |
|---|---|---|
| 1 (off) | **47.0%** | 0.0628 |
| 4 | 69.4% | 0.2165 |
| 8 | 79.9% | 0.4224 |
| 32 | **94.0%** | 0.5956 |

So read-ahead really is the direct cause of near-total conversion — and the
effect is very repeatable (residency sd ≤ 0.2 pp). But the causal chain breaks
at the next link. With read-ahead **off**, residency finally acquires a real
gradient across the ratio (47% → 65% → 72% at 15/50/80) and **throughput stays
flat anyway** (0.0628 / 0.0622 / 0.0653, if anything rising).

**Conclusion: residency saturation is not what flattens the curve.** What
saturates is the cross-machine migration service itself. Read-ahead sets the
plateau's *height* — 9.5x, by amortizing the two all-CPU TLB shootdowns each
request would otherwise pay — not its *existence*.

Note also that the average realized batch is only ~2.1 pages: the run stops at
the first page not still owned by the source machine, so the `owner != mid` test
binds long before the 32-page cap does.

### The depth sweep above is configuration-scoped; do not generalize it

Every run in that table used `MALLOC_MODE=MIXED_DEFAULT_CXL`. Repeating depth 8
under the authoritative `MIXED_DEFAULT_DRAM` (`out/20260730_173608`, two boots)
gives a completely different picture:

| ratio | ra=32 (Mops/s) | ra=32 residency | ra=8 (Mops/s) | ra=8 residency |
|---|---|---|---|---|
| 15% | 0.6671 | 78.0% | **0.6747** | **66.4%** |
| 50% | 0.6401 | 81.0% | 0.6457 | 75.2% |
| 80% | 0.6282 | 81.5% | 0.6237 | 77.4% |

Under `MIXED_DEFAULT_CXL`, depth 8 cost 1.41x against depth 32 (0.4224 vs
0.5956). Under `MIXED_DEFAULT_DRAM` it costs **nothing** — the two curves lie
within about 1% of each other at every ratio. The −10.8% placement effect
explains the ra=32 gap between the two configurations but is nowhere near enough
to explain the ra=8 gap, so this is a genuine interaction: when the kernel's own
page tables and thread contexts also live in CXL, servicing a fault is expensive
enough that amortizing it matters; when they live in DRAM, it is not.

**So "read-ahead is a large net win" holds for `MIXED_DEFAULT_CXL` and does not
transfer to the configuration the formal sweep actually uses.** Depth 32 remains
the default, but on these numbers the justification is the CXL-placement case,
not a universal one.

This pair is also the strongest available evidence for the claim at the top of
this section. Depth 8 leaves the database **11.6 pp less CXL-resident** at ratio
15 and gives it a **3.2x steeper residency gradient** across 15→80% (+11.0 pp
against +3.5 pp) — yet the throughput curves nearly coincide. The pages
read-ahead drags in are cold; the hot ones migrate under either depth, so the
access-weighted residency that drives §3's model is essentially the same.

## 5. Decomposition of the residual 15→100% slope

The plateau is not perfectly flat: 15→100% costs **6.16%**. Two channels
account for it.

1. **Tier creep (about half).** Page-weighted residency still rises from 78.03%
   to 81.78%. Propagating those 3.75 pp through the two-tier model at *k* = 2.94
   predicts **−2.81%**.
2. **True remoteness (the rest).** The residual **−3.35%** falls over a rise in
   cross-machine row access from 1.02% to 6.84%, i.e. about **0.58% per
   percentage point of remote row access**. Restricting the same calculation to
   15→80% gives 0.73%/pp, so the coefficient is stable to roughly ±25% across
   the range.

**There is no third, workload-intrinsic channel.** An earlier decomposition
credited 2.17% to TPC-C itself getting harder as the ratio rises, measured on the
one-machine control. That does **not** reproduce: in `out/20260730_151635` the
same control moves −0.17% over 15→80% with a per-point CV near 1%, and is
non-monotone. Two independent measurements of the same quantity (−2.17% and
−0.17%) disagree well outside either one's error bars, so the channel cannot
carry weight. Dropping it *strengthens* the cross-machine attribution — the whole
residual now belongs to channels 1 and 2 — at the cost of reducing the model from
three channels to two.

Ruled out explicitly:

- **Bandwidth saturation** — a pure bandwidth limit would be exactly flat; it
  is not.
- **NO_WAIT lock contention** — abort rate peaks at 0.033%.

### Retracted: the "80→100% steepening"

An earlier note recorded that 80→100% costs 7.86%, roughly 5x the 15→80% slope,
and flagged it as a real but unexplained rise in the cost per remote access.
**It was an artifact of combining two runs with different `DBX_MALLOC_MODE`.**
The 0.5803 ratio-100 point came from a `MIXED_DEFAULT_CXL` run and the 0.6298
ratio-80 point from a `MIXED_DEFAULT_DRAM` run; the ~10% placement effect
(§8) is the entire difference. Measured in one configuration, 80→100% costs
**0.35%**, and the per-step cost decays monotonically across the whole plateau.

Nothing on this curve is now unexplained.

## 7. How to configure a run whose throughput declines visibly

If the goal is a figure where eight-machine throughput falls substantially as
the ratio rises, the options in decreasing order of defensibility:

1. **Include ratio 0 and label it honestly.** This gives the full 2.74x drop —
   but the figure it produces (`out/20260730_151635`) is one tall bar and four
   nearly equal ones, not a downward slope. Ratio 0 must be marked as a
   hypothetical lower bound rather than a TPC-C point, and the drop must be
   explained as §3's tier flip rather than as a scale-out cost. A reader will
   otherwise ask whether the comparison is really DRAM against CXL — and they
   would be right.
2. ~~**Lower `DBX_READAHEAD`.**~~ **Tested and it does not work.** Depth 8 was
   the obvious candidate: it leaves far more headroom in residency, which then
   tracks the ratio much more steeply. It buys almost nothing. Across 15→80% the
   decline goes from −5.83% (ra=32) to −7.56% (ra=8) — 1.3x steeper from a 3.2x
   steeper residency gradient, and still under 8% in total. §4 explains why: the
   extra pages are cold. Not worth changing a kernel parameter for.
3. **Add a demotion path** (§3). The only change that would make the plateau
   genuinely ratio-dependent rather than cosmetically so. The batch-demotion
   work in `codex/cxl-batch-demotion` does **not** do this: it is a
   watermark-triggered (90%/85%) FIFO reclaim aimed at capacity exhaustion,
   while exp8 runs at 19.9% occupancy (12.8 GiB of 64 GiB) and would never
   trigger it. Solving capacity is not solving locality.

What will **not** work: shortening warmup. `DBX_WARMUP=0` removes the
pre-conversion but not the ratchet, so conversion simply happens inside the
measurement window instead of before it — the run measures the transient rather
than the steady state, and the endpoint is unchanged.

**The honest summary is that no available configuration produces a figure of
throughput declining substantially with the ratio, and the reason is the
result.** Between 15% and 100% the system loses 6.2%; that number is small
because, once the working set is in the shared pool, crossing a machine boundary
costs almost nothing. Presenting that as a disappointment inverts it. The figure
worth showing is the one that makes the *mechanism* visible — the footprint
panel, where local DRAM collapses from 16 GiB at ratio 0 to 3.6 GiB at ratio 15
and then creeps to 2.9 GiB, is a far better illustration of what the ratio does
than the throughput panel is.

## 8. Measurement pitfalls specific to this experiment

- **Do not combine points from runs with different `DBX_MALLOC_MODE`.**
  `MIXED_DEFAULT_CXL` vs `MIXED_DEFAULT_DRAM` is worth ~10.8% on the cluster
  arm — larger than the entire 15→80 slope. This is a real configuration effect
  (kernel-structure placement alone is worth ~10%), not noise, and it has
  already caused one false "irreproducibility" conclusion.
- **Do not use the aggregate `thp=` line.** DBx1000 prints it with `%.2f`, which
  quantizes the one-machine baseline (~0.18) to two significant digits, reports a
  zero standard deviation whenever three boots agree, and made the plotted
  scaleup non-monotone (3.556 at ratio 50 against 3.566 at ratio 80) purely by
  rounding. `plot.py` now recomputes from the per-thread `txn_cnt` and
  `run_time`, which restores the monotone 3.767 / 3.603 / 3.553 / 3.537. Any
  analysis reading these logs directly must do the same.
- **`PGFAULT_STATS_DEBUG` structurally undercounts migrations.** After a
  migration the owner's PTE is rewritten in place, so that page never faults
  again and the counter never sees it. Use a residency probe, not a fault
  counter.
- **`REMOTE_ITEM_MAX` (`user/demos/dbx1000/config.h`) is dead code** — defined,
  referenced nowhere, and the comment above it describes behavior that does not
  exist. The live `#if 1` path in `tpcc_query.cpp` draws local and remote order
  lines from the same full `NURand` range.
- **Halving `DBX_THREADS_PER_MACHINE` does not reduce host CPU pressure.** QEMU
  still runs 12 vCPU threads per machine, 96 total on a 96-core host, regardless
  of how many of them the guest workload uses.
