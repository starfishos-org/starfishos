# Per-service-queue tail latency and saturation throughput (camera-ready)

Reviewer B (paper Figure 11b) asked for tail latency and saturation
throughput per service queue.  This sweep drives the remote polling service
queue on a two-machine cluster (client on machine 1, service on machine 0)
with growing client concurrency for two request services sharing the CXL
durable queue:

| Queue | Request | What it measures |
| --- | --- | --- |
| `empty` | `POLLING_REQ_EMPTY` | raw service-queue enqueue/dequeue cost |
| `read` | `POLLING_FS_REQ_READ` | positioned 4 KiB read served by the tmpfs-backed polling FS service |

The read worker opens one descriptor per client thread before measurement and
uses offset-zero reads, so its timed interval contains only read requests (not
an open/read/close mixture). Ready/go/finish/cleanup barriers exclude thread
creation, descriptor setup, and cleanup from aggregate throughput.

Each `(queue, threads)` point is repeated three times by default. The plotted
client-side latency percentiles and aggregate throughput are medians across
those repeats. A queue is labelled saturated only when the final two
consecutive load intervals both gain at most `PLATEAU_THRESHOLD_PCT` (5% by
default). Otherwise the result is explicitly reported as a peak-observed lower
bound, not a saturation throughput. This prevents one noisy dip from being
reported as a measured saturation point.

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
| `csv/` | `trials.csv` — raw repeats; `saturation.csv` — per-point medians; `queue_summary.csv` — saturation status and observed peak |
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

`--allow-partial` is for debugging interrupted sweeps only. It may omit missing
points, but it still rejects inconsistent summary/throughput fields. A point is
complete only after its successful client-exit marker is followed by a guest
shell prompt.

## Env knobs

`THREADS` (default `"1 2 4 6 8 10"`, must stay below the guest vCPU count because
the client spin-waits), `QUEUES` (default `"empty read"`), `REPEATS` (default
3), `ITERS` (default 20000 per thread), `PLATEAU_THRESHOLD_PCT` (default 5),
`TIMEOUT`, `SKIP_BUILD`, `QSAT_CPU_NUM`, `OUT_DIR`, `LOG_DIR`, `CSV_DIR`,
`FIG_DIR`, `TS`. Repeated entries in `THREADS` or `QUEUES` are rejected.
