# DBx1000 cross-warehouse transaction sweep

This experiment varies the probability that a TPC-C transaction crosses a
warehouse boundary. It compares an eight-machine StarfishOS cluster with a
matched one-machine baseline and reports the cluster's shared-CXL and summed
local-DRAM access volume, normalized to the same committed-transaction count.

## Ratio semantics

`PERC_CROSS_WAREHOUSE_TXN` is sampled once per transaction:

- a selected Payment uses a customer warehouse different from its home;
- a selected NewOrder has exactly one remote order line;
- an unselected transaction remains within its home warehouse.

Both local and remote NewOrder lines retain the paper workload's full item-key
range. This transaction-level mode is enabled only by this runner; other
DBx1000 experiments retain their configured workload semantics.

## Formal configuration

The default full scope uses:

- cross-warehouse ratios of 0%, 15%, 50%, 80%, and 100%;
- eight machines, 64 warehouses, and eight workers per machine;
- a one-machine baseline with eight warehouses and eight workers;
- cluster placement `MIXED_DEFAULT_DRAM` + `DEFAULT_DRAM`;
- baseline placement `MIXED_DEFAULT_CXL` + `DEFAULT_CXL`;
- `WARMUP=7040000` for the eight-machine run (880000 for the matched one-machine
  baseline), `MAX_TXN_PER_PART=10000`, a five-second measurement
  window, and three independent boots per ratio;
- access volume normalized to 4 million committed transactions while retaining
  the raw five-second counters in the CSV;
- a 1024 MiB CXL residency cap available to the demotion variant through
  `DBX_CXL_DEMOTE_LIMIT_MB=1024`;
- CXL demotion disabled by default (`DBX_CXL_DEMOTE=OFF`); enable it explicitly
  for the residency-cap variant;
- host NUMA binding enabled (`CHCORE_QEMU_NUMA_BIND=1`); and
- DSM page-migration read-ahead of two pages.

`WARMUP` is not a tuning knob to shorten a run with. It has to be long enough
for the cluster's working set to reach its steady-state tier before the timed
interval opens; below that the eight-machine arm measures page migration in
progress and reads *slower* than one machine. At ratio 15% a healthy RA=2 run
gives roughly 0.59 Mops/s for the cluster arm against 0.17 Mops/s for the
baseline (scaleup ~3.4). A cluster arm at or below the baseline means the run
measured the transient -- check `WARMUP` and the cluster placement before
reading anything into the number.

The CXL-backed one-machine baseline keeps both sides on the same steady-state
memory tier. A DRAM-backed baseline answers a different question and is not
directly comparable to the default scale-out result. Similarly, results
collected with different host NUMA placement policies should not be combined.

The two arms therefore use different placements on purpose: the cluster starts
in local DRAM and reaches CXL only through cross-machine sharing, while the
one-machine baseline is CXL-backed from the start so that it is compared on the
cluster's steady-state tier rather than on a faster one.

### Do not run the cluster arm with `MIXED_DEFAULT_CXL`

`DSM_MALLOC_MODE=MIXED_DEFAULT_CXL` places the kernel's own objects on CXL. It
collapses the eight-machine arm by one to two orders of magnitude and makes it
unstable: repeated runs of the same 15% point have produced 0.0001, 0.0007,
0.06, 0.215, 0.42, and 0.595 Mops/s, against 0.586-0.675 Mops/s for every run
of the same point under `MIXED_DEFAULT_DRAM`. The one-machine baseline arm is
unaffected, so the symptom is a scaleup far below 1 rather than an obvious
failure. This was the default until 2026-08-20 and is the reason an artifact
reviewer measured 0.031 Mops/s and a scaleup of 0.18 at 15%.

The tables under "Recorded one-repetition result" below were collected under
that placement and are kept only as a record of the collapse.

## Prepare and run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
```

Run the matched cap experiment twice, using separate output directories:

```bash
DBX_CXL_DEMOTE=OFF DBX_CXL_DEMOTE_LIMIT_MB=1024 \
  OUT_DIR=artifact-evaluation/8-dbx1000-cross-warehouse/out/demote-off \
  ./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
DBX_CXL_DEMOTE=ON DBX_CXL_DEMOTE_LIMIT_MB=1024 \
  OUT_DIR=artifact-evaluation/8-dbx1000-cross-warehouse/out/demote-on \
  ./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
```

Compare FIFO and CLOCK with admission, cap, placement, and workload held
constant:

```bash
DBX_CXL_DEMOTE=ON DBX_CXL_DEMOTE_LIMIT_MB=1024 \
  DBX_CXL_DEMOTE_POLICY=FIFO \
  OUT_DIR=artifact-evaluation/8-dbx1000-cross-warehouse/out/fifo-cap1g \
  ./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
DBX_CXL_DEMOTE=ON DBX_CXL_DEMOTE_LIMIT_MB=1024 \
  DBX_CXL_DEMOTE_POLICY=CLOCK \
  OUT_DIR=artifact-evaluation/8-dbx1000-cross-warehouse/out/clock-cap1g \
  ./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
```

Run a short dependency, boot, parsing, and plotting check with:

```bash
DBX_SMOKE=1 ./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
```

Smoke mode uses one 15% point, two machines, two warehouses/workers, one short
repetition, and a reduced warmup. It validates the workflow but is not a paper
result.

Both modes use `DRAM_SIZE=16G`. The experiment checks that each selected
`/dev/shm/numa*-$USER` backing file is at least 16 GiB and does not resize it.

## Validation

For every repetition, the runner waits for DBx1000's completion markers and a
returned guest shell prompt. It rejects fatal signatures, dead tmux panes,
missing repetitions, and mismatches in the effective transaction mode, ratio,
thread/warehouse count, warmup, transaction count, table-load setting, or CPU
binding.

DBx1000 is intentionally quiet during parts of TPC-C initialization, so the
log-stall detector defaults to disabled (`DBX_LOG_STALL_S=0`). The runner still
enforces `TIMEOUT`, checks guest liveness, and scans fatal markers. Set a
positive stall limit only for targeted diagnostics.

## Outputs

Each run writes under `out/<timestamp>/`:

| Path | Contents |
| --- | --- |
| `logs/` | Per-machine logs keyed by machine count, ratio, and repetition |
| `csv/cross_warehouse.csv` | Means, sample standard deviations, scaleup, access volume, and resident footprint for both machine counts |
| `csv/cross_warehouse_samples.csv` | Individual repetitions, including all 8-machine and 1-machine measurements |
| `figures/dbx1000-cross-warehouse.png` | Throughput and stacked access-volume panels |
| `config/` | Effective run and build configuration snapshots |

The throughput panel plots the one-machine and cluster means. Scaleup is the
cluster mean divided by the baseline mean. The access-volume panel stacks the
machine-local DRAM and shared-CXL bytes reported by the aggregate `cxlprof
exec: all machines bytes` record, scaled to 4 million committed transactions.
This removes the circular effect where a faster throughput point executes more
transactions and therefore reports more raw bytes in the same five-second
interval. These are worker access bytes, not resident bytes or the final
`VMSPACE MEMORY` footprint. The CSV retains both the raw
`cxl_access_mib`/`dram_access_mib` counters and the explicit
`cxl_access_normalized_mib`/`dram_access_normalized_mib` values, together with
the committed transaction counts and sample standard deviations.

The CSV additionally records post-warmup and post-execution resident-footprint
snapshots. Resident page counts are converted using 4096 bytes per page and use
the explicit `post_warmup_*_resident_mib` and `post_exec_*_resident_mib`
columns; they are never substituted for the access counters.

Exact rates are hardware-dependent. A formal result must contain all five
ratios and all repetitions, a positive total access volume for the cluster,
and the configuration snapshots used to validate the comparison. A zero CXL
access volume is valid for a ratio whose timed interval performs no CXL access.

Do not quote the reported standard deviation as the uncertainty of a single
boot: which worker threads land in the fast mode is redrawn every boot.

## Recorded one-repetition result (`MIXED_DEFAULT_CXL` cluster placement)

These numbers are *not* the expected result for the default configuration; see
the warning above. The following 2026-08-07 runs use `WARMUP=5120`, a five-second measurement
window, a 1024 MiB CXL demotion limit, page-migration read-ahead of 32 pages,
and one repetition per point. The access columns are the aggregate `cxlprof`
byte counters converted to MiB; they are not derived from `VMSPACE MEMORY` and
are not normalized by committed transaction count.

| Ratio | Demote OFF throughput (Mops/s) | OFF CXL access (MiB) | OFF DRAM access (MiB) | Demote ON throughput (Mops/s) | ON CXL access (MiB) | ON DRAM access (MiB) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0% | 0.953854 | 0.000 | 21350.398 | 0.986342 | 0.000 | 22132.641 |
| 5% | 0.002141 | 0.794 | 49.140 | 0.002074 | 0.947 | 49.015 |
| 10% | 0.001003 | 0.684 | 22.986 | 0.000261 | 0.487 | 15.954 |
| 15% | 0.000674 | 0.645 | 15.318 | 0.000146 | 0.244 | 6.849 |
| 50% | 0.000315 | 0.803 | 6.886 | 0.000115 | 0.299 | 3.720 |
| 80% | 0.000288 | 1.089 | 5.631 | 0.000015 | 0.047 | 0.386 |
| 100% | 0.000262 | 1.118 | 4.997 | 0.000012 | 0.048 | 0.335 |

The source outputs are
`out/20260807_warmup5120_cap1g_demote_off/` and
`out/20260807_warmup5120_cap1g_demote_on/`.

### Targeted 100% result at a reduced warmup

The following 2026-08-08 one-repetition run uses a 100% cross-warehouse ratio,
page-migration read-ahead of four pages, demotion disabled, and a 1024 MiB CXL
limit. The eight-machine warmup is 512000 transactions (64000 for the matched
one-machine baseline) -- an eighth of the formal `WARMUP`, so the cluster arm
below is measured mid-migration and is not comparable to a formal run --
followed by the same five-second timed interval.

| Machines | Throughput (Mops/s) | Committed txns | Aborts | CXL access (MiB) | DRAM access (MiB) | Total access (GB) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 0.019512 | 97676 | 105 | 186.975 | 262.566 | 0.471377 |
| 1 | 0.172682 | 862381 | 322 | 3695.625 | 0.000 | 3.875144 |

The eight-machine resident footprint was:

| Snapshot | Shared CXL resident (GB) | Local DRAM resident (GB) | Total resident (GB) |
| --- | ---: | ---: | ---: |
| Post-warmup | 8.241680 | 8.182313 | 16.423993 |
| Post-exec | 8.628425 | 7.798792 | 16.427217 |

The GB column uses decimal GB and, like the MiB columns, comes from the
aggregate `cxlprof exec: all machines bytes` record rather than resident-memory
statistics. The source output is
`out/20260808_warmup512000_readahead4_cap1g_demote_off_r100/`.

## Re-plot only

Re-plot the latest formal output with:

```bash
./artifact-evaluation/run-all.sh --plot-only --run-subset-of-tests 8
```

The plotter reads the effective machine count, workload size, measurement
window, repetitions, and ratios from the selected output's
`config/run_config.json`.

To re-plot a specific output directly, pass its standard output directories;
the manifest is discovered from the log directory automatically:

```bash
python3 artifact-evaluation/8-dbx1000-cross-warehouse/plot.py \
  --log-dir artifact-evaluation/8-dbx1000-cross-warehouse/out/<timestamp>/logs \
  --csv-dir artifact-evaluation/8-dbx1000-cross-warehouse/out/<timestamp>/csv \
  --fig-dir artifact-evaluation/8-dbx1000-cross-warehouse/out/<timestamp>/figures
```

Use `--run-config` when the logs have been moved away from their output
directory. Explicit scope arguments remain available and override the recorded
values for deliberate what-if validation.

## Environment overrides

The main controls are:

- scope: `RATIOS`, `NUM_MACHINES`, `DBX_NUM_WH`,
  `DBX_THREADS_PER_MACHINE`, and `DBX_REPETITIONS`;
- workload: `DBX_WARMUP`, `DBX_MAX_TXN`, and `DBX_MEASURE_SEC`;
- CXL reclaim: `DBX_CXL_DEMOTE` (`OFF` or `ON`) and
  `DBX_CXL_DEMOTE_LIMIT_MB`, plus `DBX_CXL_DEMOTE_POLICY` (`CLOCK` or
  `FIFO`);
- placement: `DBX_MALLOC_MODE`, `DBX_USER_MALLOC_MODE`,
  `DBX_BASELINE_MALLOC_MODE`, `DBX_BASELINE_USER_MALLOC_MODE`, and
  `CHCORE_QEMU_NUMA_BIND`;
- migration: `DBX_READAHEAD` (default 4; 1 disables read-ahead);
- resources and timeouts: `DBX_DRAM_SIZE`, `DBX_BACKING_MIN_BYTES`,
  `DBX_GUEST_CPUS`, `TIMEOUT`, `DBX_LOG_STALL_S`, and `DBX_EXIT_TIMEOUT`.

Do not change `POLLING_TLB_BATCH_MAX` for this experiment: it sizes a shared
kernel/polling ABI structure and is not a workload tuning control.
