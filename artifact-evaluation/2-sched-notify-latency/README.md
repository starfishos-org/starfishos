# Scheduler and notification latency

This artifact runs a dedicated two-machine end-to-end microbenchmark.

- **Remote scheduling:** starts immediately before a worker calls
  `sys_set_affinity`, and ends after that same worker has migrated, been chosen
  by the destination scheduler, and resumed in user mode.
- **Remote notification:** starts immediately before `sys_notify`, and ends
  after the remote thread blocked in `sys_wait` has been chosen by the
  destination scheduler and resumed in user mode.

The endpoint publishes a completion flag in shared memory.  A source-machine
observer timestamps that publication, so both timestamps are in the same
monotonic-clock domain.  This avoids treating independently booted QEMU TSCs as
synchronized.  Each guest invocation takes eight samples per metric by default;
the runner repeats the complete experiment three times.  The plot first
averages samples within each run and reports mean ± standard deviation across
runs.

Scheduling uses a fresh migrating thread per sample because this kernel does
not allow an already-bound remote thread to change affinity again.  The batch is
kept deliberately finite so remote detached-thread reclamation cannot exhaust
the current implementation; independent runner repetitions provide 24 samples
by default.  Notification reuses one persistent remote waiter for its batch.

## Run the complete evaluation

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/2-sched-notify-latency/run.sh
```

Useful overrides:

```bash
NRUNS=3 SAMPLES=8 ./artifact-evaluation/2-sched-notify-latency/run.sh
```

By default, `run.sh` writes the raw logs, parsed CSV files, and figure directly
to `artifact-evaluation/2-sched-notify-latency/`. No output-directory setting is
needed.

By default, the runner rebuilds ChCore with `./quick-build.sh` so the
microbenchmark and current kernel changes are present in the image.
`SKIP_BUILD=1` is only an
advanced shortcut when the existing image is known to match the current source
and configuration.

`run.sh` boots a fresh two-machine cluster for each run, collects the raw logs,
then invokes `plot.py`. `plot.py` parses the sample lines into `samples.csv`
and `summary.csv` before drawing the figure.

## Regenerate results and figures only

To parse the existing default `logs/` directory and redraw the figure without
rebuilding or booting QEMU:

```bash
python3 artifact-evaluation/2-sched-notify-latency/plot.py
```

By default, the generated CSV files and figure are written in this directory.
`--log-dir` and `--out-dir` can override those locations.

## Run the equivalent benchmark on Linux

`run_linux.sh` measures the same user-visible endpoints on the host Linux
kernel. Remote scheduling starts immediately before a worker changes its CPU
affinity and ends after it resumes on the destination CPU. Remote notification
starts immediately before an `eventfd` write and ends after the destination
waiter returns to user mode.

```bash
./artifact-evaluation/2-sched-notify-latency/run_linux.sh
```

The script automatically selects the first two CPUs allowed by the process.
CPUs and sample counts can also be selected explicitly:

```bash
SOURCE_CPU=0 REMOTE_CPU=12 SAMPLES=1000 NRUNS=3 \
  ./artifact-evaluation/2-sched-notify-latency/run_linux.sh
```

Linux logs, CSV summaries, and the figure are written to `linux-results/` in
this directory by default. Set `OUT_DIR` to override it. The test uses
`CLOCK_MONOTONIC_RAW`, pins the source observer and destination waiter to
different CPUs, and does not require root privileges.

## Outputs

The output location is this evaluation directory:

- `logs/runN/machine0.log`, `logs/runN/machine1.log`: raw logs;
- `samples.csv`: parsed per-sample values;
- `summary.csv`: per-metric statistics;
- `sched_notify_latency.png`: generated figure.
