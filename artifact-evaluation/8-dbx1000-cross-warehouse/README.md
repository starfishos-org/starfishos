# DBx1000 cross-warehouse transaction sweep

This reviewer-requested experiment varies the probability that a TPC-C
transaction crosses a warehouse boundary. It reports throughput for an
eight-machine StarfishOS cluster and a matched one-machine weak-scale baseline,
plus the cluster's shared-CXL and summed local-DRAM footprint.

[docs/10-exp8-cross-warehouse-ratio-mechanism.md](../../docs/10-exp8-cross-warehouse-ratio-mechanism.md)
explains why the curve has the shape it has — which mechanisms were tested and
eliminated, and what may and may not be concluded from the figure. The changelog
below records what each run measured; that note records why.

## Ratio semantics

`PERC_CROSS_WAREHOUSE_TXN` is sampled once per transaction:

- a selected Payment uses a customer warehouse different from its home;
- a selected NewOrder has exactly one remote order line;
- an unselected transaction remains within its home warehouse.

Both local and remote NewOrder lines retain the paper workload's full item key
range. The new mode is explicitly enabled only by this runner; DBx1000 defaults
to its legacy `PERC_REMOTE_PAYMENT` and per-line
`PERC_REMOTE_NEW_ORDER` behavior for all other experiments.

## Formal configuration

The defaults are:

- ratios 15%, 50%, and 80%;
- eight machines, 64 warehouses, and eight workers per machine;
- a matched one-machine baseline with eight warehouses and eight workers,
  **CXL-backed** (`MIXED_DEFAULT_CXL` + `DEFAULT_CXL`) while the cluster starts
  from DRAM;
- the paper settings `WARMUP=7040000`, `MAX_TXN_PER_PART=10000`,
  `LOAD_UNUSED_TABLES=false`, and `ITEM_I_DATA_LEN=1000`;
- three independent boots per ratio and machine count;
- user pages starting in local DRAM, with shared kernel metadata in CXL.

The figure shows mean throughput and sample standard deviation. Scaleup is the
cluster throughput mean divided by the baseline mean. The footprint panel
stacks shared CXL and summed machine-local DRAM.

### Why the two sides use different placements

The cluster is configured DRAM-first, but cross-machine faults migrate its pages
into CXL and nothing migrates them back, so by steady state it serves about 99%
of its accesses from CXL. A one-machine baseline has no remote peer, so under the
same configuration it would stay 100% DRAM-resident — and the ratio of the two
would then measure a memory tier as much as a machine count. The baseline is
therefore pinned to CXL (`DBX_BASELINE_MALLOC_MODE` / `DBX_BASELINE_USER_MALLOC_MODE`,
defaulting to `MIXED_DEFAULT_CXL` + `DEFAULT_CXL`) so that scaleup isolates
scale-out. Set `DBX_BASELINE_USER_MALLOC_MODE=DEFAULT_DRAM` to recover the older,
DRAM-backed baseline; expect a markedly higher scaleup that is not comparable to
the numbers in the changelog.

Guests are pinned to host NUMA nodes by default (`CHCORE_QEMU_NUMA_BIND=1`, two
machines per node). Without it the host scheduler scatters vCPU threads across
sockets and workers split into fast and slow modes — see the 07-28 changelog
entry.

## Prepare and run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
```

Both formal and smoke runs use `DRAM_SIZE=16G`, whose ivshmem BAR requires each
`/dev/shm/numa*-$USER` file it touches to be exactly 16 GiB — the default size,
so no preparation is needed. The experiment checks the exact sizes but never
resizes them.

```bash
DBX_SMOKE=1 ./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
```

Smoke mode runs one 15% point, two machines, two warehouses/workers, and one
short repetition. It validates build, boot, log parsing, and plotting.

## Validation and outputs

For every repetition the runner waits for `PASS! SimTime`, then for DBx1000's
exact `done` line and the following shell prompt. It checks fatal signatures
and tmux liveness on every guest before accepting the point. The plotter also
checks the complete repetition matrix and verifies the effective transaction
mode, ratio, thread/warehouse count, warmup, transaction count, and table-load
setting printed by DBx1000. It also verifies the binding banner's worker count
and exact expanded CPU sequence for every machine count.

DBx1000 is intentionally silent during long parts of TPC-C initialization, so
the log-stall detector defaults to disabled (`DBX_LOG_STALL_S=0`). The runner
still enforces `TIMEOUT` and continuously checks every guest for fatal markers
and dead tmux panes. Set a positive stall limit only for targeted diagnostics.

Each run writes under `out/<timestamp>/`:

- `logs/`: per-machine logs named by machine count, ratio, and repetition;
- `csv/cross_warehouse.csv`: aggregate means and standard deviations;
- `csv/cross_warehouse_samples.csv`: individual repetitions;
- `figures/dbx1000-cross-warehouse.png`: throughput and footprint panels.

Re-plot the latest formal output with:

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 8
```

The main overrides are `RATIOS`, `NUM_MACHINES`, `DBX_NUM_WH`,
`DBX_THREADS_PER_MACHINE`, `DBX_WARMUP`, `DBX_MAX_TXN`, `DBX_REPETITIONS`,
`DBX_DRAM_SIZE`, `DBX_BACKING_MIN_BYTES`, `DBX_GUEST_CPUS`, `TIMEOUT`,
`DBX_LOG_STALL_S`/`DBX_EXIT_TIMEOUT`, and the placement pair
`DBX_MALLOC_MODE`/`DBX_USER_MALLOC_MODE`. For an overridden/smoke output, pass
the matching scope arguments directly to `plot.py` when re-plotting.

`DBX_READAHEAD` (default 32) sets the kernel's Case 2.3 page-migration
read-ahead depth in pages by defining `PGFAULT_READAHEAD_MAX` for the kernel
build; 1 disables read-ahead. It never changes `POLLING_TLB_BATCH_MAX`, which
sizes `entries[]` in the kernel/polling shared-memory ABI struct and must stay
32 on both sides. See the 07-28 read-ahead entry below.

## Changelog

**2026-07-30 (latest) — the whole curve in one configuration, and a plotting
precision fix.** Every earlier ratio table on this page was assembled from runs
that differed in `DBX_MALLOC_MODE`, which is worth about 10.8% on the cluster
arm. `out/20260730_151635` measures all five ratios in a single run
(`MIXED_DEFAULT_DRAM` cluster, `MIXED_DEFAULT_CXL` baseline, read-ahead 32,
three boots per point):

| ratio | cluster (Mops/s) | baseline (Mops/s) | scaleup | step |
|-------|------------------|-------------------|---------|------|
| 0%    | 1.8268 +- 0.0078 | 0.1789 +- 0.0016  | 10.212  | —       |
| 15%   | 0.6671 +- 0.0028 | 0.1771 +- 0.0017  | 3.767   | -63.5%  |
| 50%   | 0.6401 +- 0.0087 | 0.1777 +- 0.0005  | 3.603   | -4.05%  |
| 80%   | 0.6282 +- 0.0019 | 0.1768 +- 0.0018  | 3.553   | -1.86%  |
| 100%  | 0.6260 +- 0.0012 | 0.1770 +- 0.0005  | 3.537   | -0.35%  |

The cluster points at 15/50/80% reproduce the two runs below to within 0.25%,
and no boot shows bimodality (worker max/min 1.50-1.59). Two earlier conclusions
do not survive the single-configuration measurement:

- **The "80% -> 100% costs 7.86%" steepening was an artifact of mixing
  configurations** and is retracted. It costs 0.35%. The per-step cost decays
  monotonically (-4.05, -1.86, -0.35), so nothing on the curve is unexplained.
- **The one-machine baseline does not decline with the ratio.** It reads
  0.1789/0.1771/0.1777/0.1768/0.1770 — a 1.2% range with no trend, against the
  -2.17% recorded in the 07-28 entry. Three independent measurements of the
  ratio-15% baseline (0.1801, 0.1771, 0.1793) span 1.7%, so the earlier decline
  was within cross-run spread. Any decomposition that credits a channel to "the
  workload itself getting harder" should drop it.

**`plot.py` now recomputes throughput from the per-thread `txn_cnt` and
`run_time` instead of DBx1000's `%.2f` aggregate line.** The old path quantized
the one-machine baseline to 0.18 at every ratio with a zero standard deviation,
and rounding made the plotted scaleup non-monotone (3.556 at 50% against 3.566 at
80%). The figures and CSV were degraded even though the tables in this changelog
were already recomputed by hand. The fallback to the aggregate line is kept for
logs without per-thread rows.

A read-ahead-8 sweep (`out/20260730_173608`, two boots) shows the depth sweep in
the entry below is configuration-scoped:

| ratio | ra=32 (Mops/s) | ra=32 CXL | ra=8 (Mops/s) | ra=8 CXL |
|-------|----------------|-----------|---------------|----------|
| 15%   | 0.6671         | 78.0%     | 0.6747        | 66.4%    |
| 50%   | 0.6401         | 81.0%     | 0.6457        | 75.2%    |
| 80%   | 0.6282         | 81.5%     | 0.6237        | 77.4%    |

Under `MIXED_DEFAULT_CXL`, depth 8 cost 1.41x; under `MIXED_DEFAULT_DRAM` it
costs nothing. Depth 32 stays the default, but "read-ahead is a large net win"
is a statement about CXL-placed kernel structures, not a universal one. The pair
is also the cleanest evidence that residency is not what sets throughput: depth 8
is 11.6 pp less resident with a 3.2x steeper residency gradient, and the
throughput curves still lie within ~1% of each other.

Residency here is CXL pages over the **current** footprint (4.285M pages). The
older entries below divide by the 3.561M pages present at table load, which the
process outgrows by 0.73M pages during warmup; that convention reads about 16 pp
higher. See
[docs/10-exp8-cross-warehouse-ratio-mechanism.md](../../docs/10-exp8-cross-warehouse-ratio-mechanism.md)
for the mechanism behind all of this.

**2026-07-28 — read-ahead is what fills CXL, but not what flattens
the ratio curve.** The Case 2.3 migration path reads ahead up to
`POLLING_TLB_BATCH_MAX` virtually contiguous pages owned by the same remote
machine. Since nothing migrates back, the suspicion was that read-ahead
converts so much of the database so early that throughput stops responding to
the cross-warehouse ratio. `DBX_READAHEAD` was added to test this. Residency
below is CXL pages over the 3.561M-page (13.6 GiB) loaded database, the same
convention as the rows further down; two boots per point unless noted.

Depth sweep at ratio 15% (`out/20260728_1{71559,73509,83845,75859,65454}`):

| read-ahead | DB in CXL | cluster (Mops/s) | pages converted per cross-wh txn |
|------------|-----------|------------------|----------------------------------|
| 1 (off)    | 47.0%     | 0.0628 +- 0.0004 | 1.43 |
| 4          | 69.4%     | 0.2165 +- 0.0220 | 2.19 |
| 8          | 79.9%     | 0.4224 +- 0.0027 | 2.54 |
| 32         | 94.0%     | 0.5956 +- 0.0026 | 3.02 |

Read-ahead is therefore the direct cause of near-total CXL conversion: turning
it off halves residency at ratio 15, monotonically and very repeatably
(residency sd <= 0.2 pp over three ra=1 boots). But the effect is far smaller
than the 32x cap suggests — the run stops at the first page not still owned by
the source machine, so the average batch is about 2.1 pages, not 32.

The causal chain nevertheless breaks at the next link. Full ratio sweep with
read-ahead off (`out/20260728_1{85737,95917}`):

| ratio | DB in CXL, ra=1 | Mops/s, ra=1 | DB in CXL, ra=32 | Mops/s, ra=32 |
|-------|-----------------|--------------|------------------|---------------|
| 0%    | 3.8%            | 1.804        | 4.6%             | 1.786 |
| 15%   | 47.0%           | 0.0628       | 94.1%            | 0.6678 |
| 50%   | 65.1%           | 0.0622       | 97.5%            | 0.6385 |
| 80%   | 71.5%           | 0.0653       | 98.2%            | 0.6298 |

With read-ahead off, residency finally has a real gradient (47 -> 65 -> 72%),
yet throughput across 15-80% is flat to within measurement noise and if
anything rises. The plateau is not caused by residency saturation; what
saturates is the cross-machine migration service itself. Read-ahead sets the
plateau's height (9.5x at ratio 15) by amortizing the two all-CPU TLB
shootdowns per request; it does not create the plateau. Ratio 0% is unchanged,
as expected when no page ever migrates.

Read-ahead stays at 32. Two cautions for anyone reading the ratio rows: the
0.5956-against-0.6678 gap at ratio 15 is the `DBX_MALLOC_MODE` difference, not
noise (see the 07-30 entry, where the 15% -> 80% gradient is confirmed as real at
-5.83%); and the read-ahead comment's "would have moved anyway" argument is
explicitly coupled to migration being one-way, so adding a demotion path requires
revisiting this loop rather than treating the two as independent changes.

**2026-07-28 (later) — CXL-backed baseline, and NUMA pinning on by default.**
`CHCORE_QEMU_NUMA_BIND` now defaults to 1, and the one-machine baseline runs
`MIXED_DEFAULT_CXL` + `DEFAULT_CXL` so that both sides serve from the same memory
tier (see "Why the two sides use different placements" above). Run
`out/20260728_125606`:

| ratio | cluster (Mops/s) | baseline (Mops/s) | scaleup |
|-------|------------------|-------------------|---------|
| 15%   | 0.6678 +- 0.0020 | 0.1801 +- 0.0004  | 3.709 +- 0.014 |
| 50%   | 0.6385 +- 0.0043 | 0.1770 +- 0.0011  | 3.608 +- 0.033 |
| 80%   | 0.6298 +- 0.0020 | 0.1762 +- 0.0006  | 3.574 +- 0.017 |

The cluster side is unchanged from the previous run to within 0.1% at all three
ratios, which is a useful internal check: the only thing that moved is the
baseline. Scaleup now falls monotonically with the cross-warehouse ratio, and
the 15%-to-50% step is about 3x its combined uncertainty.

**A single machine loses 2.94x by running from CXL instead of local DRAM**
(0.5292 vs 0.1801 Mops/s at 15%, identical in every other respect). That figure
is worth reporting in its own right: it is the price the cluster pays for the
one-way DRAM-to-CXL migration described in the 07-27 entry.

Both baselines are kept and are directly comparable, since the cluster arm
matches to 0.1%: `out/20260728_105941` (DRAM baseline, scaleup 1.26/1.24/1.24)
and `out/20260728_125606` (CXL baseline, 3.71/3.61/3.57). They answer different
questions — the DRAM baseline asks what eight machines buy over the best single
machine, the CXL baseline asks what they buy at a fixed memory tier — and the
choice moves the headline number by 3x, so state which one a reported figure
uses. A reader may reasonably object that the CXL baseline is handicapped: a real
single-machine deployment would use its local DRAM, and it is the cluster that
ends up on CXL by consequence rather than by choice.

**2026-07-28 — host NUMA fixed; these are the numbers to quote.** The worker
split described in the 07-27 entry turned out to be host NUMA, not anything in
the guest: the shared CXL region sat on host node 4, 278 ns from node 0 but
463-598 ns from nodes 1-3, while QEMU's vCPU threads were left to float over all
96 cores. A worker was fast exactly when CFS parked its thread on node 0. See
[docs/09-host-numa-and-exp8-bimodality.md](../../docs/09-host-numa-and-exp8-bimodality.md).
Interleaving the region over nodes 4 and 5 (now the `config_memdev.sh` default)
plus `CHCORE_QEMU_NUMA_BIND=1` removes it. Run `out/20260728_105941`:

| ratio | cluster (Mops/s) | baseline (Mops/s) | scaleup |
|-------|------------------|-------------------|---------|
| 15%   | 0.6672 +- 0.0009 | 0.5292 +- 0.0037  | 1.261 +- 0.009 |
| 50%   | 0.6391 +- 0.0058 | 0.5154 +- 0.0020  | 1.240 +- 0.012 |
| 80%   | 0.6292 +- 0.0022 | 0.5085 +- 0.0028  | 1.237 +- 0.008 |

The bimodal split is gone (max/min 1.57-1.60 against 3.9-4.8 before, no worker
above 2x the median) and repeatability is now 0.13-0.91% instead of about +-11%.
Both curves fall monotonically with the cross-warehouse ratio — cluster -5.7%,
baseline -3.9% — so scaleup edges down from 1.26 to 1.24 and is then flat within
error between 50% and 80%.

Note the aggregate `thp=` line DBx1000 prints is `%.2f`, so a standard deviation
computed from it reads 0.0000 whenever three boots agree to about 1.5%. The
figures above are recomputed from `txn_cnt` and `run_time`, which carry more
digits.

**Scaleup dropped from 1.78 to 1.26 because the baseline gained 43%, not because
the cluster lost anything.** The single-machine baseline is 100% local DRAM
(a residency probe reads 0.00% CXL on it), so pinning its vCPUs to the node
holding its memory is a pure win; the cluster serves ~99% of its accesses from
CXL, where interleaving flattens latency rather than reducing it. Earlier
scaleup figures were inflated by an under-performing baseline. Note also that
pinning gives the two sides different CPU headroom — the cluster runs 96 vCPU
threads on 96 cores, the baseline 12 on the 24 of one node — so the comparison
now assumes the baseline should run in its best configuration.

Numbers here use `CHCORE_QEMU_NUMA_BIND=1`, which is **not** the default; a run
without it lands somewhere between this and the 07-27 row.

**2026-07-27 — fixed-duration measurement window.** DBx1000's stock rule ends
the run as soon as the first worker commits `MAX_TXN_PER_PART` transactions
(`system/thread.cpp`), so the measured window is `MAX_TXN_PER_PART / rate of the
fastest worker` — a random variable. On the eight-machine cluster roughly 30% of
workers run 3-4x faster than the rest (which ones is random per boot), so the
window length, and with it the reported aggregate throughput, swung between 0.46
and 0.61 Mops/s at a fixed cross-warehouse ratio. `MEASURE_DURATION_SEC` now
makes every worker measure the same interval and stop on its own clock; the
cluster-wide `sim_done` flag is no longer polled inside the hot loop. Because a
worker can now outlive its pre-generated query stream, `Query_thd` wraps its
query array instead of running off the end. Set `DBX_MEASURE_SEC=0` to restore
the stock rule.

Effect of the fix, measured over the full sweep (`out/20260727_171936`, 5s
window, three boots per point) against the last run of the stock rule
(`out/20260726_190212`):

| ratio | cluster (old → new) | baseline (old → new) | scaleup (old → new) |
|-------|---------------------|----------------------|---------------------|
| 15%   | 0.593 → 0.660       | 0.353 → 0.370        | 1.679 → 1.784       |
| 50%   | 0.517 → 0.587       | 0.327 → 0.340        | 1.582 → 1.725       |
| 80%   | 0.533 → 0.543       | 0.293 → 0.310        | 1.818 → 1.753       |

Every number rises because the stock window (0.16-0.6s) stopped before steady
state; the single-machine baseline was under-measured by up to 7.6%. Both curves
now decline monotonically with the cross-warehouse ratio, and repeatability
improves sharply — the 50% cluster point went from 11.7% to 2.0% coefficient of
variation. The footprint panel is unchanged (within 50 MiB), so the footprint
half of Reviewer B Q3 was never affected by this bug.

**The reported standard deviation still understates the true uncertainty.**
Aggregate throughput tracks the number of "fast" workers almost linearly
(roughly +0.018 Mops/s each), and that count is drawn afresh on every boot: 13 to
21 out of 64 across every run recorded here. Three boots sample it too coarsely —
an identical ratio-15% configuration measured 0.58 Mops/s with 13 fast workers
and 0.66 with 17-18. Treat the cluster points as +-11%, which makes the residual
1.784/1.725/1.753 spread in scaleup noise rather than a trend.

The 30/70 worker split itself is still open, and it is now the dominant source
of measurement uncertainty rather than a curiosity. It is not warmup skew (warmup
is a barrier-bracketed phase and is identical per worker) and not CPU
oversubscription (halving workers per machine leaves the split intact). At a 10x
longer window it gets sharper, not blurrier — 51 workers packed into
23,413-27,372 transactions, 13 at 93,811-128,798, nothing in between — so it is
structural, not a startup transient. The fast fraction also falls as the
cross-warehouse ratio rises (18/17/18 workers at 15%, 15/14/14 at 80%), which
fits fast workers being the ones whose working set stayed local. Current lead:
the DSM page placement / remote fault path.

**2026-07-26 — placement.** Formal runs pin `MIXED_DEFAULT_DRAM` +
`DEFAULT_DRAM`, matching Figure 13's K-mix/U-mix. Figure 14's auto-scale DBx1000
point uses `MIXED_DEFAULT_CXL` + `DEFAULT_CXL` (`5-auto-scale`); see
[docs/06-paper-figure-map.md](../../docs/06-paper-figure-map.md). `run.sh`'s own
historical default (`MIXED_DEFAULT_CXL` + `DEFAULT_DRAM`) matched no paper
configuration.

**2026-07-25 (resolved).** `DBX_USER_MALLOC_MODE=DEFAULT_DRAM` used to hang
DBx1000 during TPC-C table load — the guest printed the `TPCC init:` banner and
never reached `PASS! SimTime`. Fixed by `fb59517e` (remote page migration and
reclamation), verified 07-25/26.

`out/formal_20260720_1430` predates placement pinning in `run.sh`, so it
inherited `kernel/dsm_config.cmake` and its footprint is 100% CXL / 0 DRAM. That
is why `plot.py`'s "cluster point must contain both CXL and DRAM" guard rejects
it; it is not a parsing bug.
