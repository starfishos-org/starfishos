# IPC CDF (paper Figure 11)

End-to-end artifact for local direct IPC vs cross-machine polling IPC.

## Run

```bash
./artifact-evaluation/prepare.sh          # once, global
./artifact-evaluation/1-ipc-cdf/run.sh
```

`run.sh` enables IPC instrumentation, rebuilds, and measures six client modes
(`direct_empty`, `direct`, `cross_empty`, `cross`, `cross_empty_4t`,
`cross_4t`), then calls `plot.py`. Each mode boots its own two-machine cluster
and is torn down afterwards, so no measurement runs in a guest whose state has
already been through a client process exit; `machine{0,1}.log` accumulate
across those boots.

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
./artifact-evaluation/run-all.sh --plot-only --run-subset-of-tests 1
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

`IPC_MODES`, `SKIP_BUILD`, `KEEP_QEMU`, `TIMEOUT`, `INPUT_TIMEOUT`, `OUT_DIR`,
`LOG_DIR`, `CSV_DIR`, `FIG_DIR`, `TS`.

`IPC_MODES` selects which of the six measurement points to run (default: all of
`direct_empty direct cross_empty cross cross_empty_4t cross_4t`), one boot
each. A thinned selection plots with `--allow-partial` and does not produce a
paper figure; use it to re-check the boot path or one mode, for example

```bash
IPC_MODES="cross" ./artifact-evaluation/1-ipc-cdf/run.sh
```
