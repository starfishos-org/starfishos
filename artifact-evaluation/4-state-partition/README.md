# State partitioning (paper Figure 13)

Reproduces **Figure 13: Performance across state-partition choices** — six
applications on a **2-machine** cluster under four state-partition
configurations, normalized to the *Private* (all-DRAM, ideal) setup.

## Run

```bash
./artifact-evaluation/prepare.sh                  # once, global
./artifact-evaluation/4-state-partition/run.sh
```

Runtime: ~2–3 hours with defaults (4 configs × 1 build each × 6 benches ×
one 2-machine QEMU cluster boot per bench).

## Configurations

All five per-type placement modes (`THREADCTX/PGTABLE/STACK/OBJECT/PAGE`)
stay `CXL` in every config except `All_DRAM`:

| Config (paper label) | `DSM_MALLOC_MODE` | `DSM_USER_MALLOC_MODE` |
|---|---|---|
| All_CXL (*Share*) | CXL | DEFAULT_CXL |
| Kernel_DRAM_User_CXL (*K-mix/U-share*) | MIXED_DEFAULT_DRAM | DEFAULT_CXL |
| Kernel_Page_CXL_Other_DRAM (*K-mix/U-mix*) | MIXED_DEFAULT_DRAM | DEFAULT_DRAM |
| All_DRAM (*Private*) | DRAM | DEFAULT_DRAM |

## Benchmarks

Each bench is launched via its ramdisk script (`source run_<bench>.sh`):
LevelDB (`fillbatch`, MB/s ↑), DBx1000 (`thp=` ↑), and four Phoenix apps —
PCA, Matrix Multiply, Linear Regression, Word Count (`library:` exec time ↓).
Throughput benches normalize as `v / v_private`; time benches as
`t_private / t`.

## Outputs (under `out/<timestamp>/`)

- `logs/<bench>_<config>.log` — machine-0 log per run
- `results/state_partition.csv` — raw metrics (same layout as the paper's
  `eval/state_partition.csv`: one row per config, one column per bench)
- `results/normalized.csv` — normalized to Private
- `figures/state_partition.pdf` / `.eps`

## Re-parse an existing run

```bash
python3 artifact-evaluation/4-state-partition/plot.py \
  --log-dir artifact-evaluation/4-state-partition/out/<ts>/logs \
  --out-dir artifact-evaluation/4-state-partition/out/<ts>
```

To verify the plotter directly with the paper-format CSV:

```bash
python3 artifact-evaluation/4-state-partition/plot.py \
  --csv /mnt/disk1/yjs/p3os-paper/eval/state_partition.csv \
  --out-dir /tmp/state-partition-check
```

## Env knobs

`BENCHS`, `CONFIGS`, `NUM_MACHINES` (default 2), `TIMEOUT` (default 1200 s),
`OUT_DIR`.

The default run always collects the full 6 × 4 matrix. DBx1000 is rebuilt with
`NUM_MACHINES` total warehouses (one per machine at the default two-machine
scale), and that total remains fixed for the single-machine All-DRAM baseline
so the working set is comparable. Override it with `DBX_NUM_WH`, which must be
a positive multiple of `NUM_MACHINES`; `DBX_WARMUP` and `DBX_MAX_TXN` control
the reduced run length. Missing points are fatal by default; use
`plot.py --allow-partial` only to inspect an interrupted sweep.
