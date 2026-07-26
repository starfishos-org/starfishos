# DBx1000 cross-warehouse transaction sweep

This reviewer-requested experiment varies the probability that a TPC-C
transaction crosses a warehouse boundary. It reports throughput for an
eight-machine StarfishOS cluster and a matched one-machine weak-scale baseline,
plus the cluster's shared-CXL and summed local-DRAM footprint.

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
- a matched one-machine baseline with eight warehouses and eight workers;
- the paper settings `WARMUP=7040000`, `MAX_TXN_PER_PART=10000`,
  `LOAD_UNUSED_TABLES=false`, and `ITEM_I_DATA_LEN=1000`;
- three independent boots per ratio and machine count;
- user pages starting in local DRAM, with shared kernel metadata in CXL.

The figure shows mean throughput and sample standard deviation. Scaleup is the
cluster throughput mean divided by the baseline mean. The footprint panel
stacks shared CXL and summed machine-local DRAM.

## Prepare and run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
```

Formal runs use `DRAM_SIZE=24G`, whose ivshmem BAR requires each of the first
eight `/dev/shm/numa*-$USER` files to be exactly 32 GiB. Prepare those backing
files before running; the experiment checks the exact sizes but never resizes
them. Smoke mode requires its first two backing files to be exactly 16 GiB:

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

## Known issue: the default placement hangs DBx1000 (2026-07-25)

`DBX_USER_MALLOC_MODE=DEFAULT_DRAM` (the default above) makes DBx1000 hang
during TPC-C table load: the guest prints the `TPCC init:` banner and never
reaches `PASS! SimTime`, so the runner times out. Reproduced twice on
2026-07-21 (`out/smoke_pruned_20260721`, `out/smoke_pruned_retry_20260721`).

The variable is isolated. Re-running the identical smoke with only the user
placement changed completes both points:

```bash
DBX_SMOKE=1 DBX_USER_MALLOC_MODE=DEFAULT_CXL ./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
```

(`out/diag_userCXL_smoke_20260725`: 1-machine and 2-machine points both reach
`PASS! SimTime`.) So the hang is in the DRAM-first heap plus cross-machine
page-fault migration path, not in the cross-warehouse workload changes, the
smoke scale, or the runner.

Consequences for the data currently in `out/`:

- `out/formal_20260720_1430` was produced before `run.sh` pinned any placement,
  so it inherited `kernel/dsm_config.cmake` (`MIXED_DEFAULT_CXL` +
  `DEFAULT_CXL`). Its footprint is therefore 100% CXL / 0 DRAM — not a parsing
  bug, and the reason `plot.py`'s "cluster point must contain both CXL and
  DRAM" guard rejects it.
- With `DEFAULT_CXL` the experiment runs but cannot answer the footprint half of
  Reviewer B Q3, because every user page is in CXL by construction.

Note also that `MIXED_DEFAULT_CXL` + `DEFAULT_DRAM` matches no configuration in
the paper: Figure 14's auto-scale DBx1000 point uses `MIXED_DEFAULT_CXL` +
`DEFAULT_CXL` (`5-auto-scale`), and Figure 13's K-mix/U-mix uses
`MIXED_DEFAULT_DRAM` + `DEFAULT_DRAM` (`4-state-partition`). Pick one of those
before collecting camera-ready numbers; see
[docs/06-paper-figure-map.md](../../docs/06-paper-figure-map.md).
