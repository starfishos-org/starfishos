# State partitioning (paper Figure 13)

Reproduces **Figure 13: Performance across state-partition choices** — six
applications on a **2-machine** cluster under four state-partition
configurations, normalized to the *Private* (all-DRAM, ideal) setup.

## Run

```bash
./artifact-evaluation/prepare.sh                  # once, global
./artifact-evaluation/state-partition/run.sh
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
- `figures/fig13-state-partition.pdf` / `.eps`

## Re-parse an existing run

```bash
python3 artifact-evaluation/state-partition/parse_and_plot.py \
  --log-dir artifact-evaluation/state-partition/out/<ts>/logs \
  --out-dir artifact-evaluation/state-partition/out/<ts>
```

## Env knobs

`BENCHS`, `CONFIGS`, `NUM_MACHINES` (default 2), `TIMEOUT` (default 1200 s),
`OUT_DIR`.
