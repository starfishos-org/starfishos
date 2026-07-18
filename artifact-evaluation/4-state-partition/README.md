# State partitioning (paper Figure 13)

Six applications under four state-partition configurations, normalized to
the Private (all-DRAM) baseline.

Camera-ready default (shepherd revision plan): the three shared placements
(Share / K-mix/U-share / K-mix/U-mix) run at **both 4 and 8 machines** with
**12 vCPUs per QEMU**, so the benefit of partitioned placement can be seen
growing with cluster size (Reviewer B).  Private (`All_DRAM`) remains the
single-machine ideal baseline shared by every panel.  Override with
`MACHINE_COUNTS="2"` (or any space-separated size list) for smaller
ablations; `NUM_MACHINES=N` is accepted as a legacy alias.

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/4-state-partition/run.sh
```

Runtime: most of a day with the default 4+8 sweep (7 builds + 6 benches ×
7 placement points).

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
| `logs/` | `<bench>_<config>_m<machines>.log` per point (`All_DRAM` is `m1`) |
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

`--machine-counts N [N...]` selects the plotted cluster sizes (default `4 8`).
Legacy single-size runs whose logs are named `<bench>_<config>.log` re-plot
with a single `--machine-counts` value, e.g. `--machine-counts 2`.

Paper CSV validation (legacy one-size layout):

```bash
python3 artifact-evaluation/4-state-partition/plot.py \
  --csv /path/to/paper/state_partition.csv --machine-counts 2 \
  --csv-dir /tmp/state-partition-check/csv \
  --fig-dir /tmp/state-partition-check/figures
```

## Env knobs

`BENCHS`, `CONFIGS`, `MACHINE_COUNTS` (default `"4 8"`; `NUM_MACHINES` is a
legacy alias), `TIMEOUT`, `OUT_DIR`, `LOG_DIR`, `CSV_DIR`, `FIG_DIR`, `TS`,
`DBX_NUM_WH` (default: largest machine count), `DBX_WARMUP`, `DBX_MAX_TXN`,
`DBX_TIMEOUT`, `MATRIX_THREADS_PER_MACHINE` (default 8).
