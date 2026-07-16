# Auto-scaling applications (paper Figure 14)

Three throughput/runtime vs machine-count curves (1–8 machines):

- Matrix-multiply MapReduce
- DBx1000 TPC-C
- GeminiGraph PageRank

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/5-auto-scale/run.sh
```

`run.sh` sweeps StarfishOS Mixed/CXL placements, collects Linux/MPI/Tigon
baselines via `run_baselines.py`, then plots.

## Outputs

Each run creates `artifact-evaluation/5-auto-scale/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/` | `<app>_<Mixed\|CXL>_N<n>.log`, baseline logs, `tigon-source.txt` |
| `csv/` | `4000size.txt`, `db1000-p3os-tigon.csv`, `gemini-data.log` |
| `figures/` | Paper Figure 14 PNG files |

Paper figure files in `figures/`:

- `auto-scale-matrix.png`
- `db1000.png`
- `gemini-chcore.png`
- `auto-scale-legend.png`

Linux baseline build cache: `linux-build/` (gitignored).

## Re-plot only

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 5
```

From sweep logs:

```bash
python3 artifact-evaluation/5-auto-scale/plot.py \
  --log-dir artifact-evaluation/5-auto-scale/out/<timestamp>/logs \
  --csv-dir artifact-evaluation/5-auto-scale/out/<timestamp>/csv \
  --fig-dir artifact-evaluation/5-auto-scale/out/<timestamp>/figures
```

From paper data files:

```bash
python3 artifact-evaluation/5-auto-scale/plot.py \
  --csv-dir /tmp/as-check/csv \
  --fig-dir /tmp/as-check/figures \
  --matrix-data /path/to/paper/4000size.txt \
  --db1000-data /path/to/paper/db1000-p3os-tigon.csv \
  --gemini-data /path/to/paper/gemini-data.log
```

## Env knobs

`APPS`, `MACHINES`, `CONFIGS`, `TIMEOUT`, `RUN_BASELINES`, `BASELINE_STAGES`,
`TIGON_DIR`, `TIGON_SETUP`, `TIGON_IMAGE_ATTEMPTS`, `OUT_DIR`, `LOG_DIR`,
`CSV_DIR`, `FIG_DIR`, `TS`.
