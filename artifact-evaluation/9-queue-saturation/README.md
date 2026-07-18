# Per-service-queue tail latency and saturation throughput (camera-ready)

Reviewer B (paper Figure 11b) asked for tail latency and saturation
throughput per service queue.  This sweep drives the remote polling service
queue on a two-machine cluster (client on machine 1, service on machine 0)
with growing client concurrency for two request services sharing the CXL
durable queue:

| Queue | Request | What it measures |
| --- | --- | --- |
| `empty` | `POLLING_REQ_EMPTY` | raw service-queue enqueue/dequeue cost |
| `read` | `POLLING_FS_REQ_READ` | 4 KiB read served by the tmpfs-backed polling FS service |

Each point reports client-side latency percentiles (p50–p99 tail latency)
and aggregate throughput from the client's wall clock; the saturation
throughput is the highest achieved rate in the sweep.

## Run

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/9-queue-saturation/run.sh
```

## Outputs

Each run creates `artifact-evaluation/9-queue-saturation/out/<timestamp>/`:

| Directory | Contents |
| --- | --- |
| `logs/` | `machine0.log`, `machine1.log` |
| `csv/` | `saturation.csv` — one row per (queue, threads) point |
| `figures/` | `queue_saturation.png` — throughput vs load + p99 vs throughput |

## Re-plot only

```bash
python3 artifact-evaluation/run_all.py --plot-only --run-subset-of-tests 9
```

Or point `plot.py` at a specific run:

```bash
python3 artifact-evaluation/9-queue-saturation/plot.py \
  --log-dir artifact-evaluation/9-queue-saturation/out/<timestamp>/logs \
  --csv-dir artifact-evaluation/9-queue-saturation/out/<timestamp>/csv \
  --fig-dir artifact-evaluation/9-queue-saturation/out/<timestamp>/figures
```

`--allow-partial` is for debugging interrupted sweeps only.

## Env knobs

`THREADS` (default `"1 2 4 8"`, must stay below the guest vCPU count because
the client spin-waits), `QUEUES` (default `"empty read"`), `ITERS` (default
20000 per thread), `TIMEOUT`, `SKIP_BUILD`, `QSAT_CPU_NUM`, `OUT_DIR`,
`LOG_DIR`, `CSV_DIR`, `FIG_DIR`, `TS`.
