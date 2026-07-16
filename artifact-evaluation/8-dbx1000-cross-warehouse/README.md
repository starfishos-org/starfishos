# DBx1000 cross-warehouse sweep (reviewer request)

Sweeps cross-warehouse TPC-C transaction ratios on an 8-machine cluster and
reports throughput plus CXL/DRAM footprint.

Not a numbered paper figure; outputs use the experiment name as the figure stem.

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/8-dbx1000-cross-warehouse/run.sh
```

## Outputs

Each run creates `artifact-evaluation/8-dbx1000-cross-warehouse/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/` | `machine<i>_r<R>.log` |
| `csv/` | `cross_warehouse.csv`, `footprint_per_machine.csv` |
| `figures/` | `dbx1000-cross-warehouse.png` |

## Re-plot only

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 8
```

Or point `plot.py` at a specific run:

```bash
python3 artifact-evaluation/8-dbx1000-cross-warehouse/plot.py \
  --log-dir artifact-evaluation/8-dbx1000-cross-warehouse/out/<timestamp>/logs \
  --csv-dir artifact-evaluation/8-dbx1000-cross-warehouse/out/<timestamp>/csv \
  --fig-dir artifact-evaluation/8-dbx1000-cross-warehouse/out/<timestamp>/figures \
  --num-machines 8 --ratios 15 50 80
```

## Env knobs

`RATIOS`, `NUM_MACHINES`, `DRAM_SIZE`, `TIMEOUT`, `OUT_DIR`, `LOG_DIR`,
`CSV_DIR`, `FIG_DIR`, `TS`.
