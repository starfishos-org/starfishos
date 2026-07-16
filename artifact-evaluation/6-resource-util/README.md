# Resource utilization / co-location (paper Figure 15)

Reproduces paper Figure 15 (`real`): 12 applications under `single`, `stress`,
and `p3os`, normalized to each application's single-run baseline.

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/6-resource-util/run.sh
```

## Outputs

Each run creates `artifact-evaluation/6-resource-util/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/` | `<bench>_<cond>.log` and archived machine logs |
| `csv/` | `real.csv` |
| `figures/` | `real.png` — paper Figure 15 |

## Re-plot only

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 6
```

Or point `plot.py` at a specific run:

```bash
python3 artifact-evaluation/6-resource-util/plot.py \
  --log-dir artifact-evaluation/6-resource-util/out/<timestamp>/logs \
  --csv-dir artifact-evaluation/6-resource-util/out/<timestamp>/csv \
  --fig-dir artifact-evaluation/6-resource-util/out/<timestamp>/figures
```

Paper CSV validation:

```bash
python3 artifact-evaluation/6-resource-util/plot.py \
  --csv /path/to/paper/real.csv \
  --fig-dir /tmp/real-check/figures
```

## Env knobs

`STRESS_TYPES`, `CONDS`, `SINGLE_BENCHES`, `TIMEOUT`, `OUT_DIR`, `LOG_DIR`,
`CSV_DIR`, `FIG_DIR`, `TS`.
