# Scheduler and notification latency (paper Section 8.2)

Measures four end-to-end latencies on a two-machine cluster:

- local / cross-machine scheduling
- local / cross-machine notification

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/2-sched-notify-latency/run.sh
```

Example overrides:

```bash
NRUNS=3 SAMPLES=8 LOCAL_CPU=4 REMOTE_CPU=12 GUEST_CPU_NUM=12 \
  ./artifact-evaluation/2-sched-notify-latency/run.sh
```

## Outputs

Each run creates `artifact-evaluation/2-sched-notify-latency/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/runN/` | `machine0.log`, `machine1.log` per repetition |
| `csv/` | `samples.csv`, `summary.csv` |
| `figures/` | `sched_notify_latency.png` |

Linux host baseline (`run_linux.sh`, optional) writes the same layout under
`linux-results/` (`logs/`, `csv/`, `figures/`).

## Re-plot only

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 2
```

Or point `plot.py` at a specific run:

```bash
python3 artifact-evaluation/2-sched-notify-latency/plot.py \
  --log-dir artifact-evaluation/2-sched-notify-latency/out/<timestamp>/logs \
  --csv-dir artifact-evaluation/2-sched-notify-latency/out/<timestamp>/csv \
  --fig-dir artifact-evaluation/2-sched-notify-latency/out/<timestamp>/figures
```

## Linux reference

```bash
./artifact-evaluation/2-sched-notify-latency/run_linux.sh
# or via one-click:
python3 artifact-evaluation/run_all.py --run-subset-of-tests 2
```

## Env knobs

`NRUNS`, `SAMPLES`, `LOCAL_CPU`, `REMOTE_CPU`, `GUEST_CPU_NUM` (default 12,
overrides chcore.ini for QEMU boot), `SKIP_BUILD`, `KEEP_QEMU`,
`TIMEOUT`, `OUT_DIR`, `LOG_DIR`, `CSV_DIR`, `FIG_DIR`, `TS`.
