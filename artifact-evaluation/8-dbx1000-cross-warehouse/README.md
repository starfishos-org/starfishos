# DBx1000 cross-warehouse transaction sweep

This experiment varies the probability that a TPC-C transaction crosses a
warehouse boundary. It compares an eight-machine StarfishOS cluster with a
matched one-machine baseline and reports the cluster's shared-CXL and summed
local-DRAM footprint.

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

- cross-warehouse ratios of 15%, 50%, and 80%;
- eight machines, 64 warehouses, and eight workers per machine;
- a one-machine baseline with eight warehouses and eight workers;
- cluster placement `MIXED_DEFAULT_CXL` + `DEFAULT_DRAM`;
- baseline placement `MIXED_DEFAULT_CXL` + `DEFAULT_CXL`;
- `WARMUP=7040000`, `MAX_TXN_PER_PART=10000`, a five-second measurement
  window, and three independent boots per ratio;
- host NUMA binding enabled (`CHCORE_QEMU_NUMA_BIND=1`); and
- DSM page-migration read-ahead of 32 pages.

The CXL-backed one-machine baseline keeps both sides on the same steady-state
memory tier. A DRAM-backed baseline answers a different question and is not
directly comparable to the default scale-out result. Similarly, results
collected with different host NUMA placement policies should not be combined.

## Prepare and run

```bash
./artifact-evaluation/prepare.sh
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
| `csv/cross_warehouse.csv` | Means, sample standard deviations, scaleup, and footprint |
| `csv/cross_warehouse_samples.csv` | Individual repetitions |
| `figures/dbx1000-cross-warehouse.png` | Throughput and footprint panels |
| `config/` | Effective run and build configuration snapshots |

The throughput panel plots the one-machine and cluster means with sample
standard deviation. Scaleup is the cluster mean divided by the baseline mean.
The footprint panel stacks shared CXL and summed machine-local DRAM.

On the reference testbed, the cluster remains faster than the matched baseline
at all three ratios, while cluster throughput decreases modestly as the
cross-warehouse ratio increases. Exact rates are hardware-dependent. A formal
result must contain all three ratios and all repetitions, non-zero CXL and DRAM
footprints for the cluster, and the configuration snapshots used to validate
the comparison.

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
  --warmup 7040000 --max-txn 10000 --measure-sec 5 \
  --repetitions 3 --ratios 15 50 80
```

## Environment overrides

The main controls are:

- scope: `RATIOS`, `NUM_MACHINES`, `DBX_NUM_WH`,
  `DBX_THREADS_PER_MACHINE`, and `DBX_REPETITIONS`;
- workload: `DBX_WARMUP`, `DBX_MAX_TXN`, and `DBX_MEASURE_SEC`;
- placement: `DBX_MALLOC_MODE`, `DBX_USER_MALLOC_MODE`,
  `DBX_BASELINE_MALLOC_MODE`, `DBX_BASELINE_USER_MALLOC_MODE`, and
  `CHCORE_QEMU_NUMA_BIND`;
- migration: `DBX_READAHEAD` (default 32; 1 disables read-ahead);
- resources and timeouts: `DBX_DRAM_SIZE`, `DBX_BACKING_MIN_BYTES`,
  `DBX_GUEST_CPUS`, `TIMEOUT`, `DBX_LOG_STALL_S`, and `DBX_EXIT_TIMEOUT`.

Do not change `POLLING_TLB_BATCH_MAX` for this experiment: it sizes a shared
kernel/polling ABI structure and is not a workload tuning control.
