# DBx1000 cross-warehouse transaction sweep

This experiment varies the probability that a TPC-C transaction crosses a
warehouse boundary. It compares an eight-machine StarfishOS cluster with a
matched one-machine baseline and reports the cluster's shared-CXL and summed
local-DRAM access volume during the timed interval.

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

- cross-warehouse ratios of 0%, 5%, 10%, 15%, 50%, 80%, and 100%;
- eight machines, 64 warehouses, and eight workers per machine;
- a one-machine baseline with eight warehouses and eight workers;
- cluster placement `MIXED_DEFAULT_CXL` + `DEFAULT_DRAM`;
- baseline placement `MIXED_DEFAULT_CXL` + `DEFAULT_CXL`;
- `WARMUP=512000` for the eight-machine run (64000 for the matched one-machine
  baseline), `MAX_TXN_PER_PART=10000`, a five-second measurement
  window, and three independent boots per ratio;
- a 1024 MiB CXL residency cap, selected with `DBX_CXL_DEMOTE_LIMIT_MB=1024`;
- CLOCK/second-chance CXL demotion enabled by default (`DBX_CXL_DEMOTE=ON`);
- host NUMA binding enabled (`CHCORE_QEMU_NUMA_BIND=1`); and
- DSM page-migration read-ahead of four pages.

The CXL-backed one-machine baseline keeps both sides on the same steady-state
memory tier. A DRAM-backed baseline answers a different question and is not
directly comparable to the default scale-out result. Similarly, results
collected with different host NUMA placement policies should not be combined.

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
| `figures/dbx1000-cross-warehouse.png` | Throughput, access-volume, and post-exec resident-footprint panels |
| `config/` | Effective run and build configuration snapshots |

The throughput panel plots the one-machine and cluster means with sample
standard deviation. Scaleup is the cluster mean divided by the baseline mean.
The access-volume panel plots the shared-CXL and machine-local DRAM bytes
reported by the aggregate `cxlprof exec: all machines bytes` record on a log
scale. These are bytes accessed by the workers during the timed interval, not
resident bytes or the final `VMSPACE MEMORY` footprint. The CSV columns use the
explicit names `cxl_access_mib` and `dram_access_mib`.

The resident-footprint panel is a separate stacked view derived from the
post-execution `VMSPACE MEMORY` snapshot. The CSV also records the post-warmup
snapshot. Resident page counts are converted using 4096 bytes per page and use
the explicit `post_warmup_*_resident_mib` and `post_exec_*_resident_mib`
columns; they are never substituted for the access counters.

Exact rates are hardware-dependent. A formal result must contain all seven
ratios and all repetitions, a positive total access volume for the cluster,
and the configuration snapshots used to validate the comparison. A zero CXL
access volume is valid for a ratio whose timed interval performs no CXL access.

## Recorded one-repetition result

The following 2026-08-07 runs use `WARMUP=5120`, a five-second measurement
window, a 1024 MiB CXL demotion limit, page-migration read-ahead of 32 pages,
and one repetition per point. The access columns are the aggregate `cxlprof`
byte counters converted to MiB; they are not derived from `VMSPACE MEMORY` and
are not normalized by committed transaction count.

| Ratio | Demote OFF throughput (Mtxn/s) | OFF CXL access (MiB) | OFF DRAM access (MiB) | Demote ON throughput (Mtxn/s) | ON CXL access (MiB) | ON DRAM access (MiB) |
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

### Targeted 100% result with extended warmup

The following 2026-08-08 one-repetition run uses a 100% cross-warehouse ratio,
page-migration read-ahead of four pages, demotion disabled, and a 1024 MiB CXL
limit. The eight-machine warmup is 512000 transactions (64000 for the matched
one-machine baseline), followed by the same five-second timed interval.

| Machines | Throughput (Mtxn/s) | Committed txns | Aborts | CXL access (MiB) | DRAM access (MiB) | Total access (GB) |
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

For an overridden or smoke output, invoke `plot.py` with matching scope
arguments:

```bash
python3 artifact-evaluation/8-dbx1000-cross-warehouse/plot.py \
  --log-dir artifact-evaluation/8-dbx1000-cross-warehouse/out/<timestamp>/logs \
  --csv-dir artifact-evaluation/8-dbx1000-cross-warehouse/out/<timestamp>/csv \
  --fig-dir artifact-evaluation/8-dbx1000-cross-warehouse/out/<timestamp>/figures \
  --num-machines 8 --num-warehouses 64 --threads-per-machine 8 \
  --warmup 512000 --max-txn 10000 --measure-sec 5 \
  --repetitions 3 --ratios 0 5 10 15 50 80 100
```

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
