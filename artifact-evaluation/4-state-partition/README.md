# State partitioning (paper Figure 13)

Six applications under four state-partition configurations, normalized to
the Private (all-DRAM) baseline.

Camera-ready default: only the **8-machine** cluster is measured, with
**12 vCPUs per QEMU** (8 x 12 = 96 vCPUs, the paper testbed).  The multi-size
machinery is still there — `MACHINE_COUNTS="4 8"` reinstates the two-panel
sweep, `MACHINE_COUNTS="2"` (or any space-separated size list) gives a smaller
ablation, and `NUM_MACHINES=N` is accepted as a legacy alias.  The plotter
draws one panel per requested size.

**All six benchmarks scale their worker count with the panel**: eight workers
per machine (`WORKERS_PER_MACHINE`) bound to `0-7,12-19,...`, so the 8-machine
panel runs 64 workers (and a 4-machine panel would run 32).  Datasets stay fixed, so
this is strong scaling — a larger panel divides the same work over more
workers across a slower interconnect.  `run.sh` therefore issues each
benchmark's command itself and does **not** source the ramdisk
`run_<bench>.sh` scripts, which hardcode a single-machine 8-thread binding
(`0-11` / `-t 8`).

Private (`All_DRAM`) is the single-machine ideal baseline and gets **one
point per cluster size**, run at that size's **total worker count**: one
guest with `machines x 12` vCPUs executing `machines x 8` workers on the same
CPU pattern, with all the per-machine segments inside that one guest.

Both of these changed on 2026-07-31.  Before that, Private was a single
12-vCPU / 8-worker run reused by every panel — which normalized 32- and
64-worker cluster points against an 8-worker baseline, so Matrix Multiply
read ~3x and DBx1000 ~1.9x, mostly scale-out speedup reported as a placement
effect — and LevelDB / PCA / Linear Regression / Word Count ran the identical
single-machine 8-thread workload in every panel and under every config.
Runs produced before this change (`out/20260726_*`, `out/20260727_*`,
`out/merged_paperwarmup_*`) carry both problems; `plot.py` still replots
them, warning per bench about the baseline.

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/4-state-partition/run.sh
```

Runtime: ~0.85 h measured for the default 8-machine-only sweep (4 builds +
6 benches × 4 placement points).  Adding the 4-machine panel roughly doubles
it (~1.6 h measured).

## Configurations

| Config (paper label) | `DSM_MALLOC_MODE` | `DSM_USER_MALLOC_MODE` |
| --- | --- | --- |
| All_CXL (*Share*) | CXL | DEFAULT_CXL |
| Kernel_DRAM_User_CXL (*K-mix/U-share*) | MIXED_DEFAULT_DRAM | DEFAULT_CXL |
| Kernel_Page_CXL_Other_DRAM (*K-mix/U-mix*) | MIXED_DEFAULT_DRAM | DEFAULT_DRAM |
| All_DRAM (*Private*) | DRAM | DEFAULT_DRAM |

## Outputs

Each run creates `artifact-evaluation/4-state-partition/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/` | `<bench>_<config>_m<machines>.log` per point, keyed by the panel — `All_DRAM_m8` is the 8-machine-equivalent baseline measured on one 96-vCPU guest |
| `csv/` | `state_partition.csv`, `normalized.csv` (rows keyed by config + machines) |
| `figures/` | `state_partition.png` — paper Figure 13, one panel per cluster size |

## Re-plot only

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 4
```

Or point `plot.py` at a specific run:

```bash
python3 artifact-evaluation/4-state-partition/plot.py \
  --log-dir artifact-evaluation/4-state-partition/out/<timestamp>/logs \
  --csv-dir artifact-evaluation/4-state-partition/out/<timestamp>/csv \
  --fig-dir artifact-evaluation/4-state-partition/out/<timestamp>/figures
```

`--machine-counts N [N...]` selects the plotted cluster sizes (default `8`);
each panel is normalized to its own `All_DRAM_m<N>` baseline.  Legacy
single-size runs whose logs are named `<bench>_<config>.log` re-plot only when
that size is passed **explicitly**, e.g. `--machine-counts 2` — those names
carry no cluster size, so the default must not adopt them as the 8-machine
point.  Runs that predate
the per-panel Private point fall back to their one `All_DRAM_m1` log for every
panel, with a per-bench warning — those numbers mix scale-out speedup into the
placement comparison and are not camera-ready.

Paper CSV validation (legacy one-size layout):

```bash
python3 artifact-evaluation/4-state-partition/plot.py \
  --csv /path/to/paper/state_partition.csv --machine-counts 2 \
  --csv-dir /tmp/state-partition-check/csv \
  --fig-dir /tmp/state-partition-check/figures
```

## Env knobs

`BENCHS`, `CONFIGS`, `MACHINE_COUNTS` (default `"8"`; `NUM_MACHINES` is a
legacy alias), `TIMEOUT`, `OUT_DIR`, `LOG_DIR`, `CSV_DIR`, `FIG_DIR`, `TS`,
`DBX_WH_PER_MACHINE` (default 8, i.e. one warehouse per worker), `DBX_NUM_WH`
(pins a fixed warehouse total instead of scaling with the cluster),
`DBX_WARMUP` (default 7040000), `DBX_MAX_TXN` (default 10000),
`DBX_ITEM_I_DATA_LEN` (default 1000), `DBX_TIMEOUT`,
`WORKERS_PER_MACHINE` (default 8 — a panel of N machines runs 8N workers under
every config and every benchmark; `MATRIX_THREADS_PER_MACHINE` is accepted as
the legacy name from when only Matrix Multiply scaled),
`DBX_DRAM_SIZE` (default 24G, per cluster guest) and
`DBX_PRIVATE_DRAM_SIZE` (default `scale`: the Private baseline guest holds the
whole TPC-C database alone, so it gets the cluster's aggregate
`DBX_DRAM_SIZE x machines`; set a plain size such as `48G` to pin it).

The three DBx1000 TPC-C defaults match the paper and `8-dbx1000-cross-warehouse`.
Do not shorten `DBX_WARMUP` for a quicker sweep: the one-time first-touch
DRAM->CXL migration then falls inside the measured interval and every DRAM-first
placement reads as 10-30x slower than a single machine (measured 2026-07-26:
K-mix/U-mix at 8 machines is 0.01 Mtxn/s with `DBX_WARMUP=10000` and
0.48 Mtxn/s with 7040000).

## Metric units

`state_partition.csv` holds the raw metric per point: **LevelDB in ops/s**
(derived from `micros/op`), DBx1000 in Mtxn/s, and the four Phoenix apps in
microseconds. LevelDB's `MB/s` field is deliberately not used — it carries one
decimal, so below ~1 MB/s every placement collapses onto the same value.
