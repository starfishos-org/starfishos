# IPC CDF (paper Figure 11)

End-to-end artifact for local direct IPC vs cross-machine polling IPC.

## Run

```bash
./artifact-evaluation/prepare.sh          # once, global
./artifact-evaluation/1-ipc-cdf/run.sh
```

`run.sh` enables IPC instrumentation, rebuilds, boots two QEMU machines, runs
six client modes (`direct_empty`, `direct`, `cross_empty`, `cross`,
`cross_empty_4t`, `cross_4t`), then calls `plot.py`.

## Outputs

Each run creates `artifact-evaluation/1-ipc-cdf/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/` | `machine0.log`, `machine1.log` |
| `csv/` | `summary.csv`, `cdf.csv`, `breakdown.csv`, `server_timing.csv` |
| `figures/` | Paper Figure 11 PNG files |

Paper figure files in `figures/`:

- `ipc_cdf.png` — IPC latency CDF
- `ipc_read_breakdown.png` — Read 4 KiB median breakdown

## Re-plot only

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 1
```

Or point `plot.py` at a specific run:

```bash
python3 artifact-evaluation/1-ipc-cdf/plot.py \
  --log-dir artifact-evaluation/1-ipc-cdf/out/<timestamp>/logs \
  --csv-dir artifact-evaluation/1-ipc-cdf/out/<timestamp>/csv \
  --fig-dir artifact-evaluation/1-ipc-cdf/out/<timestamp>/figures
```

`--allow-partial` is for debugging interrupted runs only.

## Env knobs

`SKIP_BUILD`, `KEEP_QEMU`, `TIMEOUT`, `INPUT_TIMEOUT`, `OUT_DIR`, `LOG_DIR`,
`CSV_DIR`, `FIG_DIR`, `TS`.
