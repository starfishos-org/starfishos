# LevelDB filesystem recovery (paper Figure 16)

Kills machine-0 QEMU, recovers CXLFS on machine 1, and reopens the same LevelDB
database to measure recovery timeline and post-crash throughput.

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/7-recover-fs/run.sh
```

## Outputs

Each run creates `artifact-evaluation/7-recover-fs/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/` | `machine0.log`, `machine1.log`, `machine0-detector.log` |
| `csv/` | `recovery_detail.csv`, `throughput.csv` |
| `figures/` | `recovery-performance-single.png` — paper Figure 16 |

## Re-plot only

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 7
```

Or point `plot.py` at a specific run:

```bash
python3 artifact-evaluation/7-recover-fs/plot.py \
  --detail artifact-evaluation/7-recover-fs/out/<timestamp>/csv/recovery_detail.csv \
  --throughput artifact-evaluation/7-recover-fs/out/<timestamp>/csv/throughput.csv \
  --fig-dir artifact-evaluation/7-recover-fs/out/<timestamp>/figures
```

## Env knobs

`FILL_NUM`, `READ_NUM`, `THREADS`, `CRASH_DELAY`, `TIMEOUT`, `SKIP_BUILD`,
`KEEP_QEMU`, `BOOT_ONLY`, `USE_DEV_AS_DRAM`, `OUT_DIR`, `LOG_DIR`, `CSV_DIR`,
`FIG_DIR`, `TS`.
