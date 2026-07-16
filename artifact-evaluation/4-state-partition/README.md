# State partitioning (paper Figure 13)

Six applications on a two-machine cluster under four state-partition
configurations, normalized to the Private (all-DRAM) baseline.

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/4-state-partition/run.sh
```

Runtime: ~2–3 hours with defaults.

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
| `logs/` | `<bench>_<config>.log` per run |
| `csv/` | `state_partition.csv`, `normalized.csv` |
| `figures/` | `state_partition.png` — paper Figure 13 |

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

Paper CSV validation:

```bash
python3 artifact-evaluation/4-state-partition/plot.py \
  --csv /path/to/paper/state_partition.csv \
  --csv-dir /tmp/state-partition-check/csv \
  --fig-dir /tmp/state-partition-check/figures
```

## Env knobs

`BENCHS`, `CONFIGS`, `NUM_MACHINES` (default 2), `TIMEOUT`, `OUT_DIR`, `LOG_DIR`,
`CSV_DIR`, `FIG_DIR`, `TS`, `DBX_NUM_WH`, `DBX_WARMUP`, `DBX_MAX_TXN`, `DBX_TIMEOUT`.
